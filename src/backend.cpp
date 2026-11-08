#include "backend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
#include <utility>

namespace {
// Fetch more radio once fewer tracks than this are waiting.
constexpr int kLowWater = 5;
// Related tracks taken from each playlist song's mix before the next song.
constexpr int kRadioPerSeed = 5;
// Consecutive failed mix lookups before the radio stops and says so.
constexpr int kMaxFailures = 3;

QString watchUrl(const QString &id) { return "https://www.youtube.com/watch?v=" + id; }

// YouTube Music's own radio for a song. yt-dlp only reaches YouTube's mixes,
// which drift into lyric videos and memes; YouTube Music's stays on songs.
// The client version is any recent web one; the endpoint checks only its shape.
QNetworkRequest radioRequest() {
    QNetworkRequest request(QUrl("https://music.youtube.com/youtubei/v1/next?prettyPrint=false"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Origin", "https://music.youtube.com");
    request.setTransferTimeout(15000);
    return request;
}

QByteArray radioBody(const QString &id) {
    const QJsonObject client{{"clientName", "WEB_REMIX"}, {"clientVersion", "1.20250101.01.00"}, {"hl", "en"}};
    return QJsonDocument(QJsonObject{{"context", QJsonObject{{"client", client}}}, {"videoId", id},
                                     {"playlistId", "RDAMVM" + id}, {"isAudioOnly", true}})
        .toJson(QJsonDocument::Compact);
}

QString runs(const QJsonValue &text) {
    QString joined;
    for (const auto &run : text.toObject().value("runs").toArray())
        joined += run.toObject().value("text").toString();
    return joined;
}

void collectRenderers(const QJsonValue &value, QList<QJsonObject> &found) {
    if (value.isArray()) {
        for (const auto &item : value.toArray())
            collectRenderers(item, found);
    } else if (value.isObject()) {
        const auto object = value.toObject();
        if (object.contains("playlistPanelVideoRenderer"))
            found.append(object.value("playlistPanelVideoRenderer").toObject());
        else
            for (const auto &child : object)
                collectRenderers(child, found);
    }
}

QString lastLine(const QByteArray &output) {
    const auto lines = QString::fromUtf8(output).trimmed().split('\n');
    return QString(lines.last()).remove(QRegularExpression("^ERROR:\\s*")).trimmed();
}
}

QVariantMap Track::toMap() const {
    if (id.isEmpty())
        return {};
    return {{"id", id}, {"title", title}, {"artist", artist}, {"duration", duration}, {"fromPlaylist", fromPlaylist}};
}

Backend::Backend(const QString &ytdlp, const QString &mpv, QObject *parent)
    : QObject(parent), m_ytdlp(ytdlp), m_mpv(mpv) {
    QSettings settings;
    m_playlistUrl = settings.value("playlistUrl").toString();
    m_volume = std::clamp(settings.value("volume", 80).toInt(), 0, 100);
    m_mpv.command({"set_property", "volume", m_volume});

    connect(&m_fetch, &QProcess::finished, this, &Backend::playlistLoaded);
    connect(&m_fetch, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_loadingPlaylist = false;
        setStatus({});
        setError(tr("yt-dlp isn't installed. Install it with: omarchy-pkg-add yt-dlp"));
    });

    connect(&m_resolve, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QString url = QString::fromUtf8(m_resolve.readAllStandardOutput()).section('\n', 0, 0).trimmed();
        if (code == 0 && status == QProcess::NormalExit && url.startsWith("http")) {
            m_streamId = m_resolvingId;
            m_streamUrl = url;
        }
        resolveNext();
    });

    connect(&m_mpv, &Mpv::failed, this, [this](const QString &message) { setError(message); });
    connect(&m_mpv, &Mpv::ended, this, &Backend::trackEnded);
    connect(&m_mpv, &Mpv::propertyChanged, this, [this](const QString &name, const QVariant &value) {
        if (name == "time-pos") {
            // mpv reports many times a second; the window only shows seconds.
            const int position = value.isNull() ? 0 : int(value.toDouble());
            if (position > 0 && m_errorClearsOnPlay)
                setError({});
            if (position != m_position) {
                m_position = position;
                emit positionChanged();
            }
        } else if (name == "duration" && !value.isNull() && int(value.toDouble()) != m_duration) {
            m_duration = int(value.toDouble());
            emit durationChanged();
        } else if (name == "pause" && value.toBool() != m_paused) {
            m_paused = value.toBool();
            emit playingChanged();
        } else if (name == "volume" && !value.isNull() && qRound(value.toDouble()) != m_volume) {
            m_volume = qRound(value.toDouble());
            emit volumeChanged();
        }
    });
}

Backend::~Backend() {
    if (m_mixReply) {
        m_mixReply->disconnect(this);
        m_mixReply->abort();
    }
    for (QProcess *process : {&m_fetch, &m_resolve}) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

QVariantList Backend::upcoming() const {
    QVariantList list;
    for (const auto &track : m_queue)
        list.append(track.toMap());
    return list;
}

QList<Track> Backend::parseEntries(const QByteArray &json) {
    QList<Track> tracks;
    static const QRegularExpression topic(" - Topic$");
    const auto entries = QJsonDocument::fromJson(json).object().value("entries").toArray();
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        Track track;
        track.id = entry.value("id").toString();
        track.title = entry.value("title").toString();
        track.artist = entry.value("channel").toString();
        if (track.artist.isEmpty())
            track.artist = entry.value("uploader").toString();
        track.artist.remove(topic);
        track.duration = entry.value("duration").toDouble();
        // Private and deleted videos stay in playlists as "[Private video]".
        if (track.id.isEmpty() || track.title.isEmpty() || (track.title.startsWith('[') && track.title.endsWith(']')))
            continue;
        tracks.append(track);
    }
    return tracks;
}

QList<Track> Backend::parseRadio(const QByteArray &json) {
    QList<QJsonObject> renderers;
    collectRenderers(QJsonDocument::fromJson(json).object(), renderers);
    QList<Track> tracks;
    for (const auto &renderer : renderers) {
        Track track;
        track.id = renderer.value("videoId").toString();
        track.title = runs(renderer.value("title"));
        // "Shakira & Burna Boy • 1.3B views • 8.8M likes"
        track.artist = runs(renderer.value("longBylineText")).section(QString::fromUtf8(" • "), 0, 0);
        int seconds = 0;
        for (const auto &part : runs(renderer.value("lengthText")).split(':'))
            seconds = seconds * 60 + part.toInt();
        track.duration = seconds;
        if (!track.id.isEmpty() && !track.title.isEmpty())
            tracks.append(track);
    }
    return tracks;
}

// The same song often comes back as an official video, a lyric video, and an
// audio upload; this key ignores the "Artist - " prefix and the "(Official
// Video)" tags so the radio plays it once.
QString Backend::songKey(const Track &track) {
    QString title = track.title.toLower();
    const int dash = title.indexOf(" - ");
    if (dash >= 0)
        title = title.mid(dash + 3);
    static const QRegularExpression tags(R"(\s*[\(\[][^\)\]]*[\)\]])");
    title = title.remove(tags).simplified();
    return title.isEmpty() ? track.id : title;
}

void Backend::setSeeds(QList<Track> seeds) {
    for (auto &seed : seeds)
        seed.fromPlaylist = true;
    m_seeds = seeds;
    m_seedOrder.clear();
    for (int i = 0; i < m_seeds.size(); ++i)
        m_seedOrder.append(i);
    m_seedCursor = m_seedOrder.size();
    m_cycle = -1;
}

Track Backend::nextSeed() {
    if (m_seedCursor >= m_seedOrder.size()) {
        std::shuffle(m_seedOrder.begin(), m_seedOrder.end(), *QRandomGenerator::global());
        m_seedCursor = 0;
        // Every playlist song has led the radio once; let them play again.
        if (++m_cycle > 0)
            for (const auto &seed : std::as_const(m_seeds))
                m_seen.remove(songKey(seed));
    }
    return m_seeds.at(m_seedOrder.at(m_seedCursor++));
}

void Backend::addRadio(const Track &seed, const QList<Track> &mix) {
    int added = 0;
    for (const auto &track : mix) {
        if (added == kRadioPerSeed)
            break;
        const QString key = songKey(track);
        if (track.id == seed.id || m_seen.contains(key))
            continue;
        m_seen.insert(key);
        m_queue.append(track);
        ++added;
    }
    queueChanged();
}

void Backend::start(const QString &input) {
    const QString url = input.trimmed();
    if (url.isEmpty())
        return;
    m_playlistUrl = url;
    QSettings().setValue("playlistUrl", url);
    emit playlistUrlChanged();

    // The current track keeps playing until the new playlist is ready.
    m_loadingPlaylist = false;
    if (m_fetch.state() != QProcess::NotRunning) {
        m_fetch.kill();
        m_fetch.waitForFinished(1000);
    }
    if (m_mixReply) {
        m_mixReply->disconnect(this);
        m_mixReply->abort();
        m_mixReply->deleteLater();
    }
    m_seeds.clear();
    m_queue.clear();
    m_seen.clear();
    m_failures = 0;
    m_waiting = false;
    setError({});
    emit upcomingChanged();
    setStatus(tr("Loading playlist…"));
    m_loadingPlaylist = true;
    // "--" keeps a pasted link from ever being read as a yt-dlp option.
    m_fetch.start(m_ytdlp, {"--flat-playlist", "-J", "--no-warnings", "--", url});
}

void Backend::playlistLoaded(int code, QProcess::ExitStatus status) {
    if (!std::exchange(m_loadingPlaylist, false))
        return;
    const bool ok = code == 0 && status == QProcess::NormalExit;
    const auto seeds = ok ? parseEntries(m_fetch.readAllStandardOutput()) : QList<Track>{};
    if (seeds.isEmpty()) {
        setStatus({});
        setError(ok ? tr("That playlist has no playable tracks.")
                    : tr("Couldn't load the playlist: %1").arg(lastLine(m_fetch.readAllStandardError())));
        return;
    }
    setSeeds(seeds);
    setStatus(tr("%n tracks in playlist", "", int(seeds.size())));
    m_waiting = true;
    refill();
}

void Backend::mixLoaded() {
    QNetworkReply *reply = m_mixReply;
    reply->deleteLater();
    const auto mix = reply->error() == QNetworkReply::NoError ? parseRadio(reply->readAll()) : QList<Track>{};
    if (!mix.isEmpty()) {
        m_failures = 0;
        addRadio(m_mixSeed, mix);
    } else if (++m_failures >= kMaxFailures) {
        setError(tr("Couldn't reach YouTube Music: %1").arg(reply->errorString()));
    }
    refill();
}

void Backend::refill() {
    if (m_seeds.isEmpty() || m_mixReply || m_queue.size() >= kLowWater || m_failures >= kMaxFailures)
        return;
    const Track seed = nextSeed();
    m_mixSeed = seed;
    m_mixReply = m_network.post(radioRequest(), radioBody(seed.id));
    connect(m_mixReply, &QNetworkReply::finished, this, &Backend::mixLoaded);
    if (!m_seen.contains(songKey(seed))) {
        m_seen.insert(songKey(seed));
        m_queue.append(seed);
        queueChanged();
    }
}

void Backend::queueChanged() {
    emit upcomingChanged();
    if (m_waiting && !m_queue.isEmpty())
        playNext();
    else
        resolveNext();
}

void Backend::playNext() {
    if (m_queue.isEmpty()) {
        m_waiting = true;
        setStatus(tr("Finding the next track…"));
        refill();
        return;
    }
    m_waiting = false;
    m_current = m_queue.takeFirst();
    // A stream resolved ahead of time starts at once; otherwise mpv asks
    // yt-dlp itself, which takes a couple of seconds.
    m_loadedStream = m_streamId == m_current.id;
    m_mpv.load(m_loadedStream ? m_streamUrl : watchUrl(m_current.id));
    m_streamId.clear();
    m_streamUrl.clear();

    m_position = 0;
    m_duration = int(m_current.duration);
    setStatus({});
    emit currentChanged();
    emit positionChanged();
    emit durationChanged();
    emit playingChanged();
    emit upcomingChanged();
    refill();
    resolveNext();
}

void Backend::resolveNext() {
    if (m_queue.isEmpty() || m_resolve.state() != QProcess::NotRunning || m_streamId == m_queue.first().id)
        return;
    m_resolvingId = m_queue.first().id;
    m_resolve.start(m_ytdlp, {"-f", "bestaudio/best", "-g", "--no-playlist", "--no-warnings", "--", watchUrl(m_resolvingId)});
}

void Backend::trackEnded(bool failed) {
    if (failed && m_loadedStream) {
        // A resolved stream link expires after a few hours; try the page instead.
        m_loadedStream = false;
        m_mpv.load(watchUrl(m_current.id));
        return;
    }
    if (failed)
        setError(tr("Couldn't play “%1”, skipped it.").arg(m_current.title), true);
    playNext();
}

void Backend::togglePause() {
    if (!m_current.id.isEmpty())
        m_mpv.command({"set_property", "pause", !m_paused});
    else if (!m_seeds.isEmpty())
        playNext();
    else if (!m_playlistUrl.isEmpty() && !m_loadingPlaylist)
        start(m_playlistUrl);
}

void Backend::next() {
    m_failures = 0;
    setError({});
    if (!m_seeds.isEmpty())
        playNext();
}

void Backend::seek(int seconds) {
    if (!m_current.id.isEmpty())
        m_mpv.command({"seek", seconds, "relative"});
}

void Backend::seekTo(int seconds) {
    if (!m_current.id.isEmpty())
        m_mpv.command({"seek", seconds, "absolute"});
}

void Backend::changeVolume(int delta) {
    const int volume = std::clamp(m_volume + delta, 0, 100);
    if (volume == m_volume)
        return;
    m_volume = volume;
    m_mpv.command({"set_property", "volume", m_volume});
    QSettings().setValue("volume", m_volume);
    emit volumeChanged();
}

void Backend::setStatus(const QString &status) {
    if (status == m_status)
        return;
    m_status = status;
    emit statusChanged();
}

void Backend::setError(const QString &error, bool clearsOnPlay) {
    m_errorClearsOnPlay = clearsOnPlay;
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}
