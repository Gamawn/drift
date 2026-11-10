#pragma once

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>

#include "backend.h"

// Listening along. Every Drift has a room it tells what it plays (the host's side), and the
// Discord button hands that room's id to other Drifts as drift://join/<id>; one that joined
// plays the host's track at the host's position (Backend::follow) and can't change either.
// The room itself is a topic on ntfy, a public publish/subscribe server, so Drift needs none.
class Room : public QObject {
    Q_OBJECT

public:
    struct State {
        Track track; // no id: nothing to play
        int position = 0;
        bool playing = false;
    };

    explicit Room(Backend *backend, QObject *parent = nullptr);
    ~Room() override;

    // This Drift's own room, a new one at every launch.
    QString id() const { return m_id; }
    // Tells the room what plays here, whenever Discord is told.
    void publish(bool playing);
    void join(const QString &room);

    // The room in a drift://join/<id> link, or nothing for anything else.
    static QString linkRoom(const QString &link);
    // The wire format, public so tests can check it without the network.
    static QByteArray message(const QVariantMap &track, int position, bool playing, qint64 now);
    // What a line of the room's stream says is playing at `now`.
    static State parse(const QByteArray &line, qint64 now);

private:
    void listen();

    Backend *m_backend;
    QString m_id, m_joined;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_stream;
    // What the room was told last, to say nothing when nothing changed.
    QString m_sentId;
    bool m_sentPlaying = false;
    int m_sentPosition = 0;
    QElapsedTimer m_sentAt;
};
