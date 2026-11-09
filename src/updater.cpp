#include "updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVersionNumber>

#include <memory>

namespace {
const char kLatest[] = "https://api.github.com/repos/Gamawn/drift/releases/latest";
// Anything a release points to outside of this is ignored.
const char kReleases[] = "https://github.com/Gamawn/drift/releases/";
const char kInstaller[] = "Drift-setup-x64.exe";

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

// Only a copy the installer put there can be updated by running the installer again.
bool installed() {
#ifdef Q_OS_WIN
    return QFile::exists(QCoreApplication::applicationDirPath() + "/unins000.exe");
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
    return run(path, version);
}

void Updater::check() {
    if (m_busy || m_ready)
        return;
    m_busy = true;
    QNetworkReply *reply = m_network.get(request(QUrl(kLatest)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const Release release =
            reply->error() == QNetworkReply::NoError ? parseRelease(reply->readAll()) : Release();
        if (!isNewer(release.version, QCoreApplication::applicationVersion())) {
            m_busy = false;
            return;
        }
        m_release = release;
        if (!installed() || release.installer.isEmpty()) {
            offer(false);
            return;
        }
        m_path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                     .filePath(QString("Drift-setup-%1.exe").arg(release.version));
        // Still there from a run that ended before the update was installed.
        if (hashOf(m_path) == release.sha256)
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
    QNetworkReply *reply = m_network.get(request(m_release.installer));
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
        if (reply->error() != QNetworkReply::NoError || hash->result().toHex() != m_release.sha256 || !file->commit()) {
            offer(false);
            return;
        }
        QSettings settings;
        settings.setValue("update/version", m_release.version);
        settings.setValue("update/installer", m_path);
        settings.setValue("update/sha256", m_release.sha256);
        offer(true);
    });
}

// Without the installer in hand the release page is still worth pointing to;
// the next check tries the download again.
void Updater::offer(bool ready) {
    m_busy = false;
    m_version = m_release.version;
    m_ready = ready;
    emit changed();
}

void Updater::install() {
    if (m_version.isEmpty())
        return;
    if (!m_ready) {
        QDesktopServices::openUrl(m_release.page);
        return;
    }
    if (run(m_path, m_release.version))
        QCoreApplication::quit();
    else
        offer(false);
}

bool Updater::run(const QString &path, const QString &version) {
    QSettings().setValue("update/attempted", version);
    // /SILENT leaves only the progress window; /RELAUNCH is drift.iss's own and starts Drift again.
    return QProcess::startDetached(path, {"/SILENT", "/NORESTART", "/RELAUNCH=1"});
}
