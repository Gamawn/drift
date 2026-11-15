#include "updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVersionNumber>

#include <memory>
#include <utility>

namespace {
const char kLatest[] = "https://api.github.com/repos/Gamawn/drift/releases/latest";
// Anything a release points to outside of this is ignored.
const char kReleases[] = "https://github.com/Gamawn/drift/releases/";
#ifdef Q_OS_MACOS
const char kInstaller[] = "Drift-macos.dmg";
#else
const char kInstaller[] = "Drift-setup-x64.exe";
#endif

QNetworkRequest request(const QUrl &url) {
    QNetworkRequest request(url);
    // GitHub's API refuses requests without a user agent.
    request.setHeader(QNetworkRequest::UserAgentHeader, "drift/" + QCoreApplication::applicationVersion());
    request.setTransferTimeout(30000);
    return request;
}

QByteArray hashOf(const QString &path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
        return {};
    return hash.result().toHex();
}

#ifndef Q_OS_MACOS
// /SILENT leaves only the progress window; /RELAUNCH is drift.iss's own and starts Drift again.
QStringList arguments() {
    return {"/SILENT", "/NORESTART", "/RELAUNCH=1"};
}
#endif

#ifdef Q_OS_MACOS
// Drift.app, from Drift.app/Contents/MacOS.
QString bundle() {
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
}

// Once this Drift has quit, puts the Drift.app from the disk image in its place
// and opens it. A copy that fails leaves the old one, which is opened instead.
bool replaceBundle(const QString &image) {
    const char script[] = R"(
while kill -0 "$3" 2>/dev/null; do sleep 0.2; done
mount=$(mktemp -d) && hdiutil attach -nobrowse -readonly -mountpoint "$mount" "$1" >/dev/null &&
    rm -rf "$2.new" && ditto "$mount/Drift.app" "$2.new" && rm -rf "$2" && mv "$2.new" "$2"
hdiutil detach "$mount" >/dev/null
rm -rf "$2.new" "$mount"
open "$2"
)";
    return QProcess::startDetached("/bin/sh", {"-c", script, "sh", image, bundle(),
                                               QString::number(QCoreApplication::applicationPid())});
}
#endif

// Only a copy the installer put there can be updated by running the installer again;
// on macOS, a Drift.app in a folder Drift may write to (not one run straight from the disk image).
bool installed() {
#if defined(Q_OS_WIN)
    return QFile::exists(QCoreApplication::applicationDirPath() + "/unins000.exe");
#elif defined(Q_OS_MACOS)
    return bundle().endsWith(".app") && QFileInfo(QFileInfo(bundle()).absolutePath()).isWritable();
#else
    return false;
#endif
}
}

Updater::Updater(QObject *parent) : QObject(parent) {
    m_daily.setInterval(24 * 60 * 60 * 1000);
    connect(&m_daily, &QTimer::timeout, this, &Updater::check);
    m_daily.start();
}

Updater::Release Updater::parseRelease(const QByteArray &json) {
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    Release release;
    QString tag = object.value("tag_name").toString();
    if (tag.startsWith('v'))
        tag.remove(0, 1);
    // Through QVersionNumber, as the version ends up in a file name.
    release.version = QVersionNumber::fromString(tag).toString();
    const QString page = object.value("html_url").toString();
    release.page = QUrl(page.startsWith(kReleases) ? page : QString(kReleases) + "latest");
    for (const QJsonValue &value : object.value("assets").toArray()) {
        const QJsonObject asset = value.toObject();
        const QString url = asset.value("browser_download_url").toString();
        if (!url.startsWith(kReleases))
            continue;
        // GitHub hashes every uploaded asset itself: "sha256:<hex>".
        const QByteArray digest = asset.value("digest").toString().toLatin1().toLower();
        if (asset.value("name").toString() != kInstaller || !digest.startsWith("sha256:") || digest.size() != 71)
            continue;
        release.installer = QUrl(url);
        release.sha256 = digest.mid(7);
    }
    return release;
}

bool Updater::isNewer(const QString &candidate, const QString &current) {
    const QVersionNumber version = QVersionNumber::fromString(candidate).normalized();
    return !version.isNull() && version > QVersionNumber::fromString(current).normalized();
}

bool Updater::installPending() {
    QSettings settings;
    const QString version = settings.value("update/version").toString();
    const QString path = settings.value("update/installer").toString();
    const QByteArray expected = settings.value("update/sha256").toByteArray();
    if (version.isEmpty())
        return false;
    auto forget = [&] {
        QFile::remove(path);
        settings.remove("update");
        return false;
    };
    // Already installed, by the button or by hand.
    if (!isNewer(version, QCoreApplication::applicationVersion()))
        return forget();
    // One try per version: if its installer failed, Drift still has to start.
    if (settings.value("update/attempted").toString() == version || !installed())
        return false;
    if (expected.isEmpty() || hashOf(path) != expected)
        return forget();
    settings.setValue("update/attempted", version);
    settings.sync();
#ifdef Q_OS_MACOS
    return replaceBundle(path);
#else
    // Drift stays, still taking a second launch's connection, until the installer
    // closes it to replace its files. An installer that gives up instead (a
    // refused UAC prompt, Cancel) hands this launch back.
    auto *installer = new QProcess;
    installer->start(path, arguments());
    if (installer->waitForStarted()) {
        QEventLoop wait;
        connect(installer, &QProcess::finished, &wait, &QEventLoop::quit);
        wait.exec();
    }
    // Asked to quit for the installer, which a deleted QProcess would take along.
    if (installer->state() != QProcess::NotRunning)
        return true;
    delete installer;
    return false;
#endif
}

void Updater::check() {
    if (m_busy)
        return;
    m_busy = true;
    // Only for the length of a check: a manager kept around holds on to its connections.
    m_network = new QNetworkAccessManager(this);
    QNetworkReply *reply = m_network->get(request(QUrl(kLatest)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const Release release =
            reply->error() == QNetworkReply::NoError ? parseRelease(reply->readAll()) : Release();
        // Nothing newer, or the one already waiting for the button.
        if (!isNewer(release.version, QCoreApplication::applicationVersion())
            || (m_ready && release.version == m_release.version)) {
            done();
            return;
        }
        // Newer than the one on offer, which is withdrawn while this one downloads.
        if (m_ready) {
            m_ready = false;
            m_version.clear();
            emit changed();
        }
        m_release = release;
        if (!installed() || release.installer.isEmpty()) {
            offer(false);
            return;
        }
        m_path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                     .filePath(QString("Drift-%1-").arg(release.version) + kInstaller);
        // Downloaded and checked by an earlier run; install() checks it once more.
        const QSettings settings;
        if (settings.value("update/installer").toString() == m_path
            && settings.value("update/sha256").toByteArray() == release.sha256 && QFile::exists(m_path))
            offer(true);
        else
            download();
    });
}

void Updater::download() {
    // Written under a temporary name, so a download cut short leaves nothing at m_path.
    auto *file = new QSaveFile(m_path, this);
    if (!file->open(QIODevice::WriteOnly)) {
        delete file;
        offer(false);
        return;
    }
    // Hashed as it arrives: the installer is some 100 MB, a pause if read back in one go.
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkReply *reply = m_network->get(request(m_release.installer));
    auto save = [reply, file, hash] {
        const QByteArray data = reply->readAll();
        file->write(data);
        hash->addData(data);
    };
    connect(reply, &QNetworkReply::readyRead, file, save);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, hash, save] {
        reply->deleteLater();
        file->deleteLater();
        save();
        if (reply->error() != QNetworkReply::NoError) {
            // A dropped connection isn't worth sending the user to the release page.
            done();
            QTimer::singleShot(60 * 60 * 1000, this, &Updater::check);
            return;
        }
        if (hash->result().toHex() != m_release.sha256 || !file->commit()) {
            offer(false);
            return;
        }
        QSettings settings;
        // The installer of a version that was never installed.
        const QString previous = settings.value("update/installer").toString();
        if (previous != m_path)
            QFile::remove(previous);
        settings.setValue("update/version", m_release.version);
        settings.setValue("update/installer", m_path);
        settings.setValue("update/sha256", m_release.sha256);
        offer(true);
    });
}

// Without the installer in hand the release page is still worth pointing to;
// the next check tries the download again.
void Updater::offer(bool ready) {
    done();
    m_version = m_release.version;
    m_ready = ready;
    emit changed();
}

void Updater::done() {
    m_busy = false;
    if (QNetworkAccessManager *network = std::exchange(m_network, nullptr))
        network->deleteLater();
}

void Updater::install() {
    if (m_version.isEmpty())
        return;
    if (!m_ready) {
        QDesktopServices::openUrl(m_release.page);
        return;
    }
    // Checked here rather than when found on disk: reading some 100 MB would stall the window.
    if (hashOf(m_path) != m_release.sha256) {
        QFile::remove(m_path);
        offer(false);
        return;
    }
    QSettings().setValue("update/attempted", m_release.version);
#ifdef Q_OS_MACOS
    const bool started = replaceBundle(m_path);
#else
    const bool started = QProcess::startDetached(m_path, arguments());
#endif
    if (started)
        QCoreApplication::quit();
    else
        offer(false);
}
