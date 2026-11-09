#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

// Follows Drift's releases on GitHub. A copy put there by the Windows installer
// downloads the newer installer in the background, checks it against the
// SHA-256 GitHub publishes for it and runs it when asked, or on the next
// launch. Anything else (the portable zip, a package) only says that a newer
// version is out.
class Updater : public QObject {
    Q_OBJECT
    // The newer version on offer; empty while there is nothing to show.
    Q_PROPERTY(QString version READ version NOTIFY changed)
    // Its installer is downloaded and verified, so install() runs it rather than opening the release page.
    Q_PROPERTY(bool ready READ ready NOTIFY changed)

public:
    struct Release {
        QString version;
        QUrl page;
        QUrl installer;
        QByteArray sha256;
    };

    explicit Updater(QObject *parent = nullptr);

    QString version() const { return m_version; }
    bool ready() const { return m_ready; }

    // Runs an update downloaded by an earlier run. True means the installer is
    // taking over and Drift should exit; the installer starts it again.
    bool installPending();
    void check();
    Q_INVOKABLE void install();

    // Public so tests can drive them with fixtures and no network.
    static Release parseRelease(const QByteArray &json);
    static bool isNewer(const QString &candidate, const QString &current);

signals:
    void changed();

private:
    void download();
    void offer(bool ready);
    bool run(const QString &path, const QString &version);

    QNetworkAccessManager m_network;
    // Drift stays in the tray for days, so launch isn't the only time to look.
    QTimer m_daily;
    bool m_busy = false;
    Release m_release;
    QString m_path;
    QString m_version;
    bool m_ready = false;
};
