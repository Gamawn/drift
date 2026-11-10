#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QVariant>

class Backend;

// Shows the playing track on the user's Discord profile ("Listening to Drift")
// over the local IPC socket Discord's client listens on. Nothing here polls:
// Discord is looked for when a track starts or resumes, and it draws the
// progress bar itself from the start and end times, so a track costs one message.
class Discord : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    // room: this Drift's own (room.cpp), which the first button invites into.
    explicit Discord(Backend *backend, const QString &room, QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    // The wire format, public so tests can check it without Discord.
    static QByteArray frame(int opcode, const QJsonObject &payload);
    static QJsonObject activity(const QVariantMap &track, int position, int duration, qint64 now, const QString &room);
    static QStringList socketPaths();

signals:
    void enabledChanged();
    // Discord was told what plays, or that nothing does; the room hears the same.
    void announced(bool playing);

private:
    void update();
    void connectNext();
    void readSocket();

    Backend *m_backend;
    QString m_room;
    bool m_enabled = true;
    QLocalSocket m_socket;
    QStringList m_paths;
    QByteArray m_buffer;
    // Discord answered the handshake and takes activities.
    bool m_ready = false;
    // Collects a burst of changes (a skip, then the new track's duration) into one message;
    // Discord drops updates past 5 in 20 seconds.
    QTimer m_update;
    // What Discord is extrapolating the progress bar from, to notice when playback left it.
    bool m_shown = false;
    int m_shownPosition = 0;
    QElapsedTimer m_shownAt;
};
