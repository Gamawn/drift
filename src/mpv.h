#pragma once

#include <QJsonArray>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariant>

// Plays audio through an mpv process driven over its JSON IPC socket. mpv
// handles YouTube's streams far better than an in-process decoder, and with
// video off and a small demuxer cache it stays light. It starts on the first
// load, so the app costs nothing until something plays.
class Mpv : public QObject {
    Q_OBJECT

public:
    explicit Mpv(const QString &program, QObject *parent = nullptr);
    ~Mpv() override;

    void load(const QString &url);
    // Sent once mpv is listening; commands before that wait in order.
    void command(const QJsonArray &args);

signals:
    void propertyChanged(const QString &name, const QVariant &value);
    void ended(bool failed);
    void failed(const QString &message);

private:
    void start();
    void connectSocket();
    void readSocket();
    void write(const QJsonArray &args);

    QString m_program;
    QString m_socketPath;
    QProcess m_process;
    QLocalSocket m_socket;
    QTimer m_retry;
    int m_attempts = 0;
    bool m_quitting = false;
    QList<QJsonArray> m_pending;
};
