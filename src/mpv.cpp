#include "mpv.h"
#include "process.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#endif

Mpv::Mpv(const QString &program, QObject *parent) : QObject(parent), m_program(program) {
#ifdef Q_OS_WIN
    // mpv on Windows listens on a named pipe, which QLocalSocket reaches by the same path.
    m_socketPath = QString("\\\\.\\pipe\\drift-mpv-%1").arg(QCoreApplication::applicationPid());
#else
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    QDir().mkpath(runtime); // macOS gives ~/Library/Application Support, missing under Qt test mode
    m_socketPath = QString("%1/drift-mpv-%2.sock").arg(runtime).arg(QCoreApplication::applicationPid());
#endif
    hideConsole(m_process);
#ifdef Q_OS_LINUX
    // An idle mpv outlives a crashed or killed drift forever; the kernel ends it with us.
    m_process.setChildProcessModifier([] { prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif

    m_retry.setInterval(50);
    connect(&m_retry, &QTimer::timeout, this, &Mpv::connectSocket);

    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_retry.stop();
        int id = 1;
        for (const char *name : {"time-pos", "duration", "pause", "volume"})
            write({"observe_property", id++, name});
        for (const auto &args : std::as_const(m_pending))
            write(args);
        m_pending.clear();
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &Mpv::readSocket);

    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_retry.stop();
            emit failed(tr("mpv isn't installed. Install it with: omarchy-pkg-add mpv"));
        }
    });
    connect(&m_process, &QProcess::finished, this, [this] {
        m_retry.stop();
        m_socket.abort();
        if (!m_quitting)
            emit failed(tr("mpv stopped unexpectedly. Press N to try the next track."));
    });
}

Mpv::~Mpv() {
    m_quitting = true;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
#ifndef Q_OS_WIN
    QFile::remove(m_socketPath);
#endif
}

void Mpv::start() {
#ifndef Q_OS_WIN
    QFile::remove(m_socketPath);
#endif
    m_process.start(m_program, {
        "--idle=yes", "--no-video", "--force-window=no", "--audio-display=no", "--no-terminal",
        "--ytdl-format=bestaudio/best",
        // 1 MiB is about a minute of YouTube audio ahead, 256 KiB covers a 10-second
        // seek back; mpv's default is 150 MiB.
        "--demuxer-max-bytes=1MiB", "--demuxer-max-back-bytes=256KiB",
        // Skip the Lua scripts a headless player never uses (8 threads, ~6 MB); ytdl_hook
        // stays, as a track whose stream wasn't resolved is loaded by its watch URL.
        // Not --load-console: mpv before 0.40 refuses that name and won't start.
        "--load-scripts=no", "--osc=no", "--load-stats-overlay=no", "--load-auto-profiles=no",
        "--input-ipc-server=" + m_socketPath,
    });
    m_attempts = 0;
    m_retry.start();
}

void Mpv::connectSocket() {
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        return;
    if (++m_attempts > 100) {
        m_retry.stop();
        emit failed(tr("Couldn't connect to mpv."));
        return;
    }
    m_socket.connectToServer(m_socketPath);
}

void Mpv::load(const QString &url) {
    if (m_process.state() == QProcess::NotRunning)
        start();
    command({"loadfile", url, "replace"});
    command({"set_property", "pause", false});
}

void Mpv::command(const QJsonArray &args) {
    if (m_socket.state() == QLocalSocket::ConnectedState)
        write(args);
    else
        m_pending.append(args);
}

void Mpv::write(const QJsonArray &args) {
    m_socket.write(QJsonDocument(QJsonObject{{"command", args}}).toJson(QJsonDocument::Compact) + '\n');
}

void Mpv::readSocket() {
    while (m_socket.canReadLine()) {
        const QJsonObject message = QJsonDocument::fromJson(m_socket.readLine()).object();
        const QString event = message.value("event").toString();
        if (event == "property-change")
            emit propertyChanged(message.value("name").toString(), message.value("data").toVariant());
        // "stop" and "redirect" come from our own loads replacing a track.
        else if (event == "end-file" && message.value("reason").toString() == "eof")
            emit ended(false);
        else if (event == "end-file" && message.value("reason").toString() == "error")
            emit ended(true);
    }
}
