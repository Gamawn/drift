#include "backend.h"
#include "process.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace {
// Fetch more radio once fewer tracks than this are waiting.
constexpr int kLowWater = 5;
// Related tracks taken from each playlist song's mix before the next song.
constexpr int kRadioPerSeed = 5;
// A song's radio hops from mix to mix; this many tracks from each mix before
// hopping keeps it close to the song it started from.
constexpr int kChainPerSeed = 10;
// Consecutive failures (mix lookups, or tracks that won't play) before we say so.
constexpr int kMaxFailures = 3;

QString watchUrl(const QString &id) { return "https://www.youtube.com/watch?v=" + id; }

// YouTube Music's own API, for radio and search. yt-dlp only reaches
// YouTube's mixes and search, which drift into lyric videos and memes;
// YouTube Music's stay on songs. The client version is any recent web one;
// the endpoint checks only its shape.
QNetworkRequest musicRequest(const QString &endpoint) {
    QNetworkRequest request(QUrl("https://music.youtube.com/youtubei/v1/" + endpoint + "?prettyPrint=false"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Origin", "https://music.youtube.com");
    request.setTransferTimeout(15000);
    return request;
}

QByteArray musicBody(QJsonObject body) {
    const QJsonObject client{{"clientName", "WEB_REMIX"}, {"clientVersion", "1.20250101.01.00"}, {"hl", "en"}};
    body.insert("context", QJsonObject{{"client", client}});
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString runs(const QJsonValue &text) {
    QString joined;
    for (const auto &run : text.toObject().value("runs").toArray())
        joined += run.toObject().value("text").toString();
    return joined;
}

void collectRenderers(const QJsonValue &value, const QString &type, QList<QJsonObject> &found) {
    if (value.isArray()) {
        for (const auto &item : value.toArray())
            collectRenderers(item, type, found);
    } else if (value.isObject()) {
        const auto object = value.toObject();
        if (object.contains(type))
            found.append(object.value(type).toObject());
        else
            for (const auto &child : object)
                collectRenderers(child, type, found);
    }
}

int seconds(const QString &clock) {
    int total = 0;
    for (const auto &part : clock.split(':'))
        total = total * 60 + part.toInt();
    return total;
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
    hideConsole(m_fetch);
    hideConsole(m_resolve);

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
        // A failed track is marked tried too, or resolveNext would start yt-dlp for it again at once.
        m_streamId = m_resolvingId;
        m_streamUrl = code == 0 && status == QProcess::NormalExit && url.startsWith("http") ? url : QString();
        resolveNext();
    });

    connect(&m_mpv, &Mpv::failed, this, [this](const QString &message) { setError(message); });
    connect(&m_mpv, &Mpv::ended, this, &Backend::trackEnded);
    connect(&m_mpv, &Mpv::propertyChanged, this, [this](const QString &name, const QVariant &value) {
        if (name == "time-pos") {
            // mpv reports many times a second; the window only shows seconds.
            const int position = value.isNull() ? 0 : int(value.toDouble());
            if (position > 0) {
                m_playFailures = 0;
                if (m_errorClearsOnPlay)
                    setError({});
            }
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
    for (QNetworkReply *reply : {m_mixReply.data(), m_searchReply.data()}) {
        if (reply) {
            reply->disconnect(this);
            reply->abort();
        }
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

QVariantList Backend::results() const {
    QVariantList list;
    for (const auto &track : m_results)
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
        track.fromPlaylist = true;
        // Private and deleted videos stay in playlists as "[Private video]".
        if (track.id.isEmpty() || track.title.isEmpty() || (track.title.startsWith('[') && track.title.endsWith(']')))
            continue;
        tracks.append(track);
    }
    return tracks;
}

QList<Track> Backend::parseRadio(const QByteArray &json) {
    QList<QJsonObject> renderers;
    collectRenderers(QJsonDocument::fromJson(json).object(), "playlistPanelVideoRenderer", renderers);
    QList<Track> tracks;
    for (const auto &renderer : renderers) {
        Track track;
        track.id = renderer.value("videoId").toString();
        track.title = runs(renderer.value("title"));
        // "Shakira & Burna Boy • 1.3B views • 8.8M likes"
        track.artist = runs(renderer.value("longBylineText")).section(QString::fromUtf8(" • "), 0, 0);
        track.duration = seconds(runs(renderer.value("lengthText")));
        if (!track.id.isEmpty() && !track.title.isEmpty())
            tracks.append(track);
    }
    return tracks;
}

QList<Track> Backend::parseSearch(const QByteArray &json) {
    QList<QJsonObject> renderers;
    collectRenderers(QJsonDocument::fromJson(json).object(), "musicResponsiveListItemRenderer", renderers);
    QList<Track> tracks;
    for (const auto &renderer : renderers) {
        const auto columns = renderer.value("flexColumns").toArray();
        auto column = [&](int i) { return runs(columns.at(i).toObject().value("musicResponsiveListItemFlexColumnRenderer").toObject().value("text")); };
        Track track;
        track.id = renderer.value("playlistItemData").toObject().value("videoId").toString();
        track.title = column(0);
        // "Кино • Группа крови • 4:45": artist, album, length.
        const QStringList details = column(1).split(QString::fromUtf8(" • "));
        track.artist = details.first();
        if (details.size() > 1 && details.last().contains(':'))
            track.duration = seconds(details.last());
        if (!track.id.isEmpty() && !track.title.isEmpty())
            tracks.append(track);
    }
    return tracks;
}

bool Backend::isLink(const QString &text) {
    const QString trimmed = text.trimmed();
    return trimmed.startsWith("http://") || trimmed.startsWith("https://") || trimmed.contains("list=");
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

void Backend::setSeeds(const QList<Track> &seeds) {
    m_seeds = seeds;
    m_seedOrder.clear();
    for (int i = 0; i < m_seeds.size(); ++i)
        m_seedOrder.append(i);
    m_seedCursor = m_seedOrder.size();
    m_cycle = -1;
    m_emptyMixes = 0;
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
        if (added == (m_chain ? kChainPerSeed : kRadioPerSeed))
            break;
        const QString key = songKey(track);
        if (track.id == seed.id || m_seen.contains(key))
            continue;
        m_seen.insert(key);
        m_queue.append(track);
        ++added;
    }
    // A song's radio goes on from the newest track it found, or, when this
    // mix had nothing new, from somewhere else in it, so it never gets stuck.
    // Once a whole pass over the playlist found nothing new, its radio may repeat.
    m_emptyMixes = added > 0 || m_chain ? 0 : m_emptyMixes + 1;
    if (m_emptyMixes > 0 && m_emptyMixes >= m_seeds.size()) {
        m_seen.clear();
        m_emptyMixes = 0;
    }
    if (m_chain && added > 0)
        m_chainSeed = m_queue.last();
    else if (m_chain && !mix.isEmpty())
        m_chainSeed = mix.at(QRandomGenerator::global()->bounded(int(mix.size())));
    queueChanged();
}

void Backend::reset() {
    m_loadingPlaylist = false;
    if (m_fetch.state() != QProcess::NotRunning) {
        m_fetch.kill();
        m_fetch.waitForFinished(1000);
    }
    if (m_mixReply) {
        m_mixReply->disconnect(this);
        m_mixReply->abort();
        m_mixReply->deleteLater();
        m_mixReply = nullptr;
    }
    m_seeds.clear();
    m_queue.clear();
    m_seen.clear();
    m_chain = false;
    m_failures = 0;
    m_waiting = false;
    setError({});
    emit upcomingChanged();
}

void Backend::submit(const QString &text) {
    if (isLink(text))
        start(text);
    else
        search(text);
}

void Backend::search(const QString &input) {
    const QString query = input.trimmed();
    if (query.isEmpty())
        return;
    if (m_searchReply) {
        m_searchReply->disconnect(this);
        m_searchReply->abort();
        m_searchReply->deleteLater();
    }
    setError({});
    setStatus(tr("Searching…"));
    // The "params" value is YouTube Music's own "Songs" filter.
    m_searchReply = m_network.post(musicRequest("search"),
                                   musicBody({{"query", query}, {"params", "EgWKAQIIAWoKEAkQBRAKEAMQBA%3D%3D"}}));
    connect(m_searchReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = std::exchange(m_searchReply, nullptr);
        reply->deleteLater();
        setStatus({});
        if (reply->error() != QNetworkReply::NoError) {
            setError(tr("Couldn't search YouTube Music: %1").arg(reply->errorString()));
            return;
        }
        m_results = parseSearch(reply->readAll());
        if (m_results.isEmpty())
            setStatus(tr("Nothing found."));
        emit resultsChanged();
    });
}

void Backend::clearResults() {
    m_results.clear();
    emit resultsChanged();
}

void Backend::playSimilar(int index) {
    if (index < 0 || index >= m_results.size())
        return;
    const Track song = m_results.at(index);
    clearResults();
    reset();
    setSeeds({song});
    m_chain = true;
    m_chainSeed = song;
    m_waiting = true;
    refill();
}

void Backend::start(const QString &input) {
    const QString url = input.trimmed();
    if (url.isEmpty())
        return;
    m_playlistUrl = url;
    QSettings().setValue("playlistUrl", url);
    emit playlistUrlChanged();

    // The current track keeps playing until the new playlist is ready.
    clearResults();
    reset();
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
    QNetworkReply *reply = std::exchange(m_mixReply, nullptr);
    reply->deleteLater();
    const auto mix = reply->error() == QNetworkReply::NoError ? parseRadio(reply->readAll()) : QList<Track>{};
    if (!mix.isEmpty()) {
        if (std::exchange(m_failures, 0) >= kMaxFailures)
            setError({});
        addRadio(m_mixSeed, mix);
        refill();
        return;
    }
    if (++m_failures >= kMaxFailures)
        setError(reply->error() != QNetworkReply::NoError
                     ? tr("Couldn't reach YouTube Music: %1").arg(reply->errorString())
                     : tr("YouTube Music returned no tracks."));
    // Offline, retrying at once fails at once; waiting rides out a Wi-Fi reconnect.
    QTimer::singleShot(std::min(m_failures, 6) * 5000, this, &Backend::refill);
}

void Backend::refill() {
    if (m_seeds.isEmpty() || m_mixReply || m_queue.size() >= kLowWater)
        return;
    const Track seed = m_chain ? m_chainSeed : nextSeed();
    m_mixSeed = seed;
    m_mixReply = m_network.post(musicRequest("next"),
                                musicBody({{"videoId", seed.id}, {"playlistId", "RDAMVM" + seed.id}, {"isAudioOnly", true}}));
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
    m_loadedStream = m_streamId == m_current.id && !m_streamUrl.isEmpty();
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
    if (failed && ++m_playFailures >= kMaxFailures) {
        // Every track failing means yt-dlp or YouTube is broken; skipping on would never end.
        setError(tr("Couldn't play several tracks in a row. Press N to try again."));
        m_current = {};
        emit currentChanged();
        emit playingChanged();
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
    m_playFailures = 0;
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
