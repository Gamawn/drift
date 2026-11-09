// drift — endless YouTube Music radio from one of your playlists.

#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include "backend.h"
#include "discord.h"
#ifdef Q_OS_LINUX
#include <malloc.h>
#include "mpris.h"
#endif
#include "process.h"
#ifdef Q_OS_WIN
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <qt_windows.h>
#include "smtc.h"
#endif
#include "theme.h"
#include "translator.h"
#include "updater.h"

int main(int argc, char *argv[]) {
#ifdef Q_OS_LINUX
    // Omarchy sets QT_QPA_PLATFORMTHEME=gtk3, which loads all of GTK (6 threads, ~18 MB)
    // only to hand us a font; Material and the Omarchy accent replace the rest.
    qunsetenv("QT_QPA_PLATFORMTHEME");
#endif
#ifdef Q_OS_WIN
    QApplication app(argc, argv); // the tray icon and its menu are Widgets
#else
    QGuiApplication app(argc, argv);
#endif
#ifdef Q_OS_LINUX
    // shortcut: fixed to GTK's default font, so a changed GTK font isn't followed; read gsettings if that matters.
    app.setFont(QFont(QStringList{"Adwaita Sans", "sans-serif"}, 11));
#endif
    app.setApplicationName("drift");
    app.setOrganizationName("drift");
    app.setApplicationVersion("0.3.2");

    // Matches the window to drift.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("drift");
    app.setWindowIcon(QIcon::fromTheme("drift"));

#ifdef Q_OS_WIN
    // drift.iss's SetupMutex: the installer is replacing Drift's files right now.
    // The copy it starts itself says so, as the installer is still there then.
    if (!app.arguments().contains("--from-setup")) {
        if (HANDLE setup = OpenMutexW(SYNCHRONIZE, FALSE, L"DriftSetup")) {
            CloseHandle(setup);
            return 0;
        }
    }
#endif

    // One Drift at a time: launching it again only brings the running one's window back.
    const QString instance = "drift-" + qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
    QLocalSocket running;
    running.connectToServer(instance);
    if (running.waitForConnected(500))
        return 0;
    QLocalServer::removeServer(instance); // left behind if Drift crashed
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.listen(instance);

    // An update downloaded last time goes in now, while nothing is playing.
    Updater updater;
    if (updater.installPending())
        return 0;

    QQuickStyle::setStyle("Material");
    // The UI is a few rectangles and text redrawn once a second; drawing it on the CPU
    // skips the GPU driver and its render thread, which halves drift's own memory.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend(findTool("yt-dlp"), findTool("mpv"));
    Discord discord(&backend);

    // Closing the window while music plays keeps it playing in the background;
    // the window and its QML engine are freed and built again when shown.
    app.setQuitOnLastWindowClosed(false);
    QQmlApplicationEngine *engine = nullptr;
    auto window = [&] { return engine ? qobject_cast<QWindow *>(engine->rootObjects().value(0)) : nullptr; };
    auto show = [&] {
        if (QWindow *existing = window()) {
            existing->show();
            existing->raise();
            existing->requestActivate();
            return;
        }
        delete engine;
        engine = new QQmlApplicationEngine;
        QObject::connect(&translator, &Translator::changed, engine, &QQmlEngine::retranslate);
        engine->rootContext()->setContextProperty("translator", &translator);
        engine->rootContext()->setContextProperty("theme", &theme);
        engine->rootContext()->setContextProperty("backend", &backend);
        engine->rootContext()->setContextProperty("discord", &discord);
        engine->rootContext()->setContextProperty("updater", &updater);
        engine->load(QUrl("qrc:/Main.qml"));
        if (QWindow *created = window())
            QObject::connect(created, &QWindow::visibleChanged, engine, [&](bool visible) {
                if (visible || !engine)
                    return;
                if (backend.current().isEmpty())
                    QCoreApplication::quit();
                else
                    std::exchange(engine, nullptr)->deleteLater();
#ifdef Q_OS_LINUX
                // glibc keeps freed memory for reuse; hand the UI's back to the system.
                QTimer::singleShot(0, [] { malloc_trim(0); });
#endif
            });
    };
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
        while (QLocalSocket *other = server.nextPendingConnection())
            other->deleteLater();
        show();
    });
#ifdef Q_OS_LINUX
    Mpris mpris(&backend, show, [] { QCoreApplication::quit(); });
#endif
#ifdef Q_OS_WIN
    Smtc smtc(&backend);

    // A closed window keeps the music going, so the tray is the way back to it, or out.
    // IDI_ICON1 is the icon qmake's RC_ICONS puts in drift.exe.
    QSystemTrayIcon tray(QIcon(QPixmap::fromImage(QImage::fromHICON(LoadIconW(GetModuleHandleW(nullptr), L"IDI_ICON1")))));
    QMenu menu;
    QAction *showAction = menu.addAction(QString(), show);
    QAction *pauseAction = menu.addAction(QString(), &backend, &Backend::togglePause);
    QAction *nextAction = menu.addAction(QString(), &backend, &Backend::next);
    QAction *updateAction = menu.addAction(QString(), &updater, &Updater::install);
    menu.addSeparator();
    QAction *quitAction = menu.addAction(QString(), &app, &QCoreApplication::quit);
    auto updateTray = [&] {
        const QVariantMap current = backend.current();
        showAction->setText(QCoreApplication::translate("main", "Show Drift"));
        pauseAction->setText(QCoreApplication::translate("main", backend.playing() ? "Pause" : "Play"));
        nextAction->setText(QCoreApplication::translate("main", "Next"));
        updateAction->setText(
            QCoreApplication::translate("main", updater.ready() ? "Restart and update" : "Download the update"));
        updateAction->setVisible(!updater.version().isEmpty());
        quitAction->setText(QCoreApplication::translate("main", "Quit"));
        pauseAction->setEnabled(!current.isEmpty());
        nextAction->setEnabled(!current.isEmpty());
        tray.setToolTip(current.isEmpty() ? QStringLiteral("Drift")
                                          : current.value("title").toString() + " — " + current.value("artist").toString());
    };
    updateTray();
    QObject::connect(&backend, &Backend::currentChanged, &tray, updateTray);
    QObject::connect(&backend, &Backend::playingChanged, &tray, updateTray);
    QObject::connect(&translator, &Translator::changed, &tray, updateTray);
    QObject::connect(&updater, &Updater::changed, &tray, updateTray);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &tray, [&](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger)
            show();
    });
    tray.setContextMenu(&menu);
    tray.show();
#endif

    show();
    if (!window())
        return 1;
    // Not in the way of the window coming up or the first track loading.
    QTimer::singleShot(5000, &updater, &Updater::check);
    const int code = app.exec();
    delete std::exchange(engine, nullptr);
    return code;
}
