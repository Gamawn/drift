#include "discord.h"
#include "backend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>
#include <QtEndian>

#include <cstdlib>

namespace {
// Drift's application in Discord's developer portal; its name is what "Listening to …" shows.
// The id is public. Without one Drift never looks for Discord.
const char kClientId[] = "1569436736373657762";

enum Opcode { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

// Discord refuses an activity whose text is under 2 or over 128 characters.
QString text(QString value) {
    value.truncate(128);
    while (value.size() < 2)
        value += QChar(0x200B); // zero-width space
    return value;
}
}

Discord::Discord(Backend *backend, QObject *parent) : QObject(parent), m_backend(backend) {
    m_enabled = QSettings().value("discord", true).toBool();

    m_update.setSingleShot(true);
    m_update.setInterval(1000);
    connect(&m_update, &QTimer::timeout, this, &Discord::update);
    for (auto changed : {&Backend::currentChanged, &Backend::playingChanged, &Backend::durationChanged})
        connect(backend, changed, &m_update, qOverload<>(&QTimer::start));
    // A seek, or a track that took a while to load, leaves Discord's progress bar wrong.
    connect(backend, &Backend::positionChanged, this, [this] {
        if (!m_shown)
            return;
        const int expected = m_shownPosition + int(m_shownAt.elapsed() / 1000);
        if (std::abs(m_backend->position() - expected) > 2)
            m_update.start();
    });

    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_socket.write(frame(Handshake, {{"v", 1}, {"client_id", kClientId}}));
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &Discord::readSocket);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        m_ready = m_shown = false;
        m_buffer.clear();
    });
    // Not from inside the signal: the socket is still finishing the failed attempt.
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (!m_ready)
            QMetaObject::invokeMethod(this, &Discord::connectNext, Qt::QueuedConnection);
    });
}

void Discord::setEnabled(bool enabled) {
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    QSettings().setValue("discord", enabled);
    emit enabledChanged();
    update();
}

QByteArray Discord::frame(int opcode, const QJsonObject &payload) {
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray header(8, Qt::Uninitialized);
    qToLittleEndian<qint32>(opcode, header.data());
    qToLittleEndian<qint32>(qint32(json.size()), header.data() + 4);
    return header + json;
}

QJsonObject Discord::activity(const QVariantMap &track, int position, int duration, qint64 now) {
    const QString id = track.value("id").toString();
    QJsonObject activity{
        {"type", 2}, // Listening
        {"status_display_type", 2}, // the member list names the song rather than "Drift"
        {"details", text(track.value("title").toString())},
        {"assets", QJsonObject{{"large_image", "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"}}},
        // Discord takes two buttons at most, and only web links: the first is docs/index.html,
        // which passes the track on to the viewer's own Drift as drift://play/<id>.
        {"buttons", QJsonArray{QJsonObject{{"label", "Listen in Drift"},
                                           {"url", "https://gamawn.github.io/drift/?v=" + id}},
                               QJsonObject{{"label", "Listen on YouTube Music"},
                                           {"url", "https://music.youtube.com/watch?v=" + id}}}},
    };
    const QString artist = track.value("artist").toString();
    if (!artist.isEmpty())
        activity.insert("state", text(artist));
    if (duration > 0) {
        const qint64 start = now - qint64(position) * 1000;
        activity.insert("timestamps", QJsonObject{{"start", start}, {"end", start + qint64(duration) * 1000}});
    }
    return activity;
}

QStringList Discord::socketPaths() {
    QStringList paths;
#ifdef Q_OS_WIN
    for (int i = 0; i < 10; ++i)
        paths << QString("\\\\.\\pipe\\discord-ipc-%1").arg(i);
#else
    QString base = "/tmp";
    for (const char *name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (!qEnvironmentVariableIsEmpty(name)) {
            base = qEnvironmentVariable(name);
            break;
        }
    }
    // Flatpak and Snap keep Discord's socket in their own folders.
    for (const char *folder : {"", "app/com.discordapp.Discord/", "snap.discord/"}) {
        for (int i = 0; i < 10; ++i) {
            const QString path = QString("%1/%2discord-ipc-%3").arg(base, folder).arg(i);
            if (QFile::exists(path))
                paths << path;
        }
    }
#endif
    return paths;
}

void Discord::update() {
    const bool show = m_enabled && m_backend->playing();
    if (!m_ready) {
        // Only a track to show is worth looking for Discord; it is asked again once ready.
        if (show && kClientId[0] && m_socket.state() == QLocalSocket::UnconnectedState) {
            m_paths = socketPaths();
            connectNext();
        }
        return;
    }
    if (!show && !m_shown)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonValue activity =
        show ? QJsonValue(Discord::activity(m_backend->current(), m_backend->position(), m_backend->duration(), now))
             : QJsonValue(QJsonValue::Null);
    m_socket.write(frame(Frame, {
        {"cmd", "SET_ACTIVITY"},
        {"args", QJsonObject{{"pid", QCoreApplication::applicationPid()}, {"activity", activity}}},
        {"nonce", QUuid::createUuid().toString(QUuid::WithoutBraces)},
    }));
    m_shown = show;
    m_shownPosition = m_backend->position();
    m_shownAt.start();
}

void Discord::connectNext() {
    if (m_ready || m_paths.isEmpty())
        return;
    m_socket.abort();
    m_socket.connectToServer(m_paths.takeFirst());
}

void Discord::readSocket() {
    m_buffer += m_socket.readAll();
    while (m_buffer.size() >= 8) {
        const auto opcode = qFromLittleEndian<qint32>(m_buffer.constData());
        const auto length = qFromLittleEndian<qint32>(m_buffer.constData() + 4);
        if (length < 0 || length > (1 << 20)) {
            m_socket.abort();
            return;
        }
        if (m_buffer.size() < 8 + length)
            return;
        const QJsonObject message = QJsonDocument::fromJson(m_buffer.mid(8, length)).object();
        m_buffer.remove(0, 8 + length);

        if (opcode == Ping) {
            m_socket.write(frame(Pong, message));
        } else if (opcode == Close) { // an unknown client id, for one
            m_socket.abort();
            return;
        } else if (opcode == Frame && message.value("evt").toString() == "READY") {
            m_ready = true;
            update();
        }
    }
}
