#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QStringList>
#include <QVariantMap>
#include <functional>

class Backend;

// Publishes Drift on D-Bus as an MPRIS player, so media keys, playerctl and
// Omarchy's bar control it without switching to its window.
class Mpris : public QObject {
    Q_OBJECT

public:
    // Tests pass their own service name, so they never clash with a running Drift.
    Mpris(Backend *backend, std::function<void()> raise, std::function<void()> quit,
          const QString &service = "org.mpris.MediaPlayer2.drift", QObject *parent = nullptr);
};

class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ yes)
    Q_PROPERTY(bool CanRaise READ yes)
    Q_PROPERTY(bool HasTrackList READ no)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ none)
    Q_PROPERTY(QStringList SupportedMimeTypes READ none)

public:
    MprisRoot(QObject *parent, std::function<void()> raise, std::function<void()> quit)
        : QDBusAbstractAdaptor(parent), m_raise(std::move(raise)), m_quit(std::move(quit)) {}

    bool yes() const { return true; }
    bool no() const { return false; }
    QString identity() const { return "Drift"; }
    QString desktopEntry() const { return "drift"; }
    QStringList none() const { return {}; }

public slots:
    void Raise() { m_raise(); }
    void Quit() { m_quit(); }

private:
    std::function<void()> m_raise, m_quit;
};

class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ rate)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(bool CanGoNext READ hasTrack)
    Q_PROPERTY(bool CanGoPrevious READ no)
    Q_PROPERTY(bool CanPlay READ yes)
    Q_PROPERTY(bool CanPause READ hasTrack)
    Q_PROPERTY(bool CanSeek READ hasTrack)
    Q_PROPERTY(bool CanControl READ yes)

public:
    MprisPlayer(QObject *parent, Backend *backend) : QDBusAbstractAdaptor(parent), m_backend(backend) {}

    QString playbackStatus() const;
    double rate() const { return 1.0; }
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double volume);
    qlonglong position() const;
    bool hasTrack() const;
    bool yes() const { return true; }
    bool no() const { return false; }

signals:
    void Seeked(qlonglong position);

public slots:
    void Next();
    void Previous() {}
    void Pause();
    void PlayPause();
    void Stop() { Pause(); }
    void Play();
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath &track, qlonglong position);
    void OpenUri(const QString &) {}

private:
    QDBusObjectPath trackId() const;

    Backend *m_backend;
};
