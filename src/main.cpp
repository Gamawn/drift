// drift — endless YouTube Music radio from one of your playlists.

#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>

#include "backend.h"
#include "process.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
#ifdef Q_OS_LINUX
    // Omarchy sets QT_QPA_PLATFORMTHEME=gtk3, which loads all of GTK (6 threads, ~18 MB)
    // only to hand us a font; Material and the Omarchy accent replace the rest.
    qunsetenv("QT_QPA_PLATFORMTHEME");
#endif
    QGuiApplication app(argc, argv);
#ifdef Q_OS_LINUX
    // shortcut: fixed to GTK's default font, so a changed GTK font isn't followed; read gsettings if that matters.
    app.setFont(QFont(QStringList{"Adwaita Sans", "sans-serif"}, 11));
#endif
    app.setApplicationName("drift");
    app.setOrganizationName("drift");
    app.setApplicationVersion("0.2.0");

    // Matches the window to drift.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("drift");
    app.setWindowIcon(QIcon::fromTheme("drift"));

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

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
