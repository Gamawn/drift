#include "room.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdlib>

namespace {
// Any ntfy server does; this one is public and free.
// shortcut: ntfy.sh takes 250 messages a day from one address (about one a track) and lets whoever
// knows a room's id speak in it, so a guest could pose as the host; run an ntfy of our own with
// write access per room if either gets in the way.
const char kServer[] = "https://ntfy.sh/drift-";
}

Room::Room(Backend *backend, QObject *parent)
    : QObject(parent), m_backend(backend), m_id(QUuid::createUuid().toString(QUuid::Id128)) {
    // Left the room: listen() drops the stream.
    connect(backend, &Backend::followingChanged, this, [this] {
        if (!m_backend->following())
            listen();
    });
    // The track publish() was waiting for is heard now.
    connect(backend, &Backend::positionChanged, this, [this] {
        if (m_held && m_backend->position() > 0)
            publish(m_backend->playing());
    });
}

Room::~Room() {
    if (m_stream) {
        m_stream->disconnect(this);
        m_stream->abort();
    }
}

QString Room::linkRoom(const QString &link) {
    static const QRegularExpression pattern(R"(^drift://join/([a-f0-9]{32})/?$)",
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern.match(link.trimmed()).captured(1).toLower();
}

QByteArray Room::message(const QVariantMap &track, int position, bool playing, qint64 now) {
    return QJsonDocument(QJsonObject{
        {"id", track.value("id").toString()},
        // ntfy takes 4096 bytes; a title is a few dozen.
        {"title", track.value("title").toString().left(200)},
        {"artist", track.value("artist").toString().left(200)},
        {"duration", track.value("duration").toDouble()},
        {"position", position},
        {"playing", playing},
        {"at", now},
    }).toJson(QJsonDocument::Compact);
}

Room::State Room::parse(const QByteArray &line, qint64 now) {
    const QJsonObject event = QJsonDocument::fromJson(line).object();
    if (event.value("event").toString() != "message")
        return {};
    const QJsonObject said = QJsonDocument::fromJson(event.value("message").toString().toUtf8()).object();
    // Anyone who knows the room can write to it, so only a real video id gets through.
    static const QRegularExpression videoId("^[A-Za-z0-9_-]{11}$");
    State state;
    state.track.id = said.value("id").toString();
    if (!videoId.match(state.track.id).hasMatch())
        return {};
    state.track.title = said.value("title").toString().left(200);
    state.track.artist = said.value("artist").toString().left(200);
    state.track.duration = std::max(0.0, said.value("duration").toDouble());
    state.playing = said.value("playing").toBool();
    // shortcut: trusts both clocks to be set right (NTP); measure the offset if rooms drift apart.
    const qint64 age = state.playing ? std::max<qint64>(0, now - said.value("at").toInteger(now)) / 1000 : 0;
    const qint64 position = std::max(0, said.value("position").toInt()) + age;
    // The last thing a host said before leaving: that track is long over.
    if (position > 24 * 3600 || (state.track.duration > 0 && position >= state.track.duration))
        return {};
    state.position = int(position);
    return state;
}

void Room::publish(bool playing) {
    const QVariantMap track = m_backend->current();
    const QString id = track.value("id").toString();
    const int position = m_backend->position();
    // A track is announced a second after it's picked, and its stream may take a few more to
    // start: saying "playing, at 0" now would put a guest ahead by that much. The first second
    // heard is said instead, which also lands on the second's edge.
    m_held = playing && position == 0 && !id.isEmpty();
    if (m_held)
        return;
    const int expected = m_sentPosition + (m_sentPlaying ? int(m_sentAt.elapsed() / 1000) : 0);
    if (id.isEmpty() || (id == m_sentId && playing == m_sentPlaying && std::abs(position - expected) <= 2))
        return;
    m_sentId = id;
    m_sentPlaying = playing;
    m_sentPosition = position;
    m_sentAt.start();
    QNetworkRequest request(QUrl(kServer + m_id));
    request.setTransferTimeout(15000);
    // A message that didn't get there isn't sent again; the next track's will.
    QNetworkReply *reply = m_network.post(request, message(track, position, playing, QDateTime::currentMSecsSinceEpoch()));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void Room::join(const QString &room) {
    if (room.isEmpty() || room == m_id) // one's own Discord button
        return;
    m_joined = room;
    m_backend->setFollowing(true);
    listen();
}

void Room::listen() {
    if (m_stream) {
        m_stream->disconnect(this);
        m_stream->abort();
        m_stream->deleteLater();
        m_stream = nullptr;
    }
    if (!m_backend->following())
        return;
    // One JSON line per message, starting with the last one said, and a keepalive every 45 seconds.
    QNetworkRequest request(QUrl(kServer + m_joined + "/json?since=latest"));
    request.setTransferTimeout(120000);
    m_stream = m_network.get(request);
    connect(m_stream, &QNetworkReply::readyRead, this, [this] {
        while (m_stream && m_stream->canReadLine()) {
            const State state = parse(m_stream->readLine(), QDateTime::currentMSecsSinceEpoch());
            if (!state.track.id.isEmpty())
                m_backend->follow(state.track, state.position, state.playing);
        }
    });
    // The server closes a stream now and then, and Wi-Fi drops; listen() checks the room wasn't left.
    connect(m_stream, &QNetworkReply::finished, this, [this] { QTimer::singleShot(5000, this, &Room::listen); });
}
