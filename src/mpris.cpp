#include "mpris.h"
#include "backend.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QRegularExpression>

namespace {
const QString kPath = "/org/mpris/MediaPlayer2";
constexpr qlonglong kSecond = 1000000; // MPRIS counts microseconds
}

Mpris::Mpris(Backend *backend, std::function<void()> raise, std::function<void()> quit, const QString &service,
             QObject *parent)
    : QObject(parent) {
    new MprisRoot(this, std::move(raise), std::move(quit));
    auto *player = new MprisPlayer(this, backend);

    // Clients cache properties and only hear about changes we announce.
    auto announce = [player](const QStringList &names) {
        QVariantMap changed;
        for (const auto &name : names)
            changed.insert(name, player->property(name.toUtf8().constData()));
        auto signal = QDBusMessage::createSignal(kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged");
        signal << QString("org.mpris.MediaPlayer2.Player") << changed << QStringList();
        QDBusConnection::sessionBus().send(signal);
    };
    connect(backend, &Backend::currentChanged, this, [announce] {
        announce({"Metadata", "PlaybackStatus", "CanGoNext", "CanPause", "CanSeek"});
    });
    connect(backend, &Backend::durationChanged, this, [announce] { announce({"Metadata"}); });
    connect(backend, &Backend::playingChanged, this, [announce] { announce({"PlaybackStatus"}); });
    connect(backend, &Backend::volumeChanged, this, [announce] { announce({"Volume"}); });
    // Position isn't announced as it ticks; clients extrapolate it and only
    // need telling when it jumps, which is a seek.
    connect(backend, &Backend::positionChanged, this, [backend, player, last = 0]() mutable {
        if (std::abs(backend->position() - last) > 1)
            emit player->Seeked(player->position());
        last = backend->position();
    });

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(kPath, this) || !bus.registerService(service))
        qWarning("Drift couldn't register %s on the session bus", qPrintable(service));
}

QString MprisPlayer::playbackStatus() const {
    if (!hasTrack())
        return "Stopped";
    return m_backend->playing() ? "Playing" : "Paused";
}

QDBusObjectPath MprisPlayer::trackId() const {
    if (!hasTrack())
        return QDBusObjectPath("/org/mpris/MediaPlayer2/TrackList/NoTrack");
    // Object paths allow only letters, digits and "_"; YouTube ids also use "-".
    static const QRegularExpression invalid("[^A-Za-z0-9_]");
    return QDBusObjectPath("/org/drift/track/" + m_backend->current().value("id").toString().replace(invalid, "_"));
}

QVariantMap MprisPlayer::metadata() const {
    QVariantMap map{{"mpris:trackid", QVariant::fromValue(trackId())}};
    if (!hasTrack())
        return map;
    const QVariantMap current = m_backend->current();
    map.insert("xesam:title", current.value("title").toString());
    map.insert("xesam:artist", QStringList{current.value("artist").toString()});
    map.insert("mpris:artUrl", "https://i.ytimg.com/vi/" + current.value("id").toString() + "/mqdefault.jpg");
    if (m_backend->duration() > 0)
        map.insert("mpris:length", qlonglong(m_backend->duration()) * kSecond);
    return map;
}

double MprisPlayer::volume() const { return m_backend->volume() / 100.0; }

void MprisPlayer::setVolume(double volume) { m_backend->setVolume(qRound(volume * 100)); }

qlonglong MprisPlayer::position() const { return qlonglong(m_backend->position()) * kSecond; }

bool MprisPlayer::hasTrack() const { return !m_backend->current().isEmpty(); }

void MprisPlayer::Next() { m_backend->next(); }

void MprisPlayer::Pause() {
    if (m_backend->playing())
        m_backend->togglePause();
}

void MprisPlayer::PlayPause() { m_backend->togglePause(); }

void MprisPlayer::Play() {
    if (!m_backend->playing())
        m_backend->togglePause();
}

void MprisPlayer::Seek(qlonglong offset) { m_backend->seek(int(offset / kSecond)); }

void MprisPlayer::SetPosition(const QDBusObjectPath &track, qlonglong position) {
    if (track == trackId())
        m_backend->seekTo(int(position / kSecond));
}
