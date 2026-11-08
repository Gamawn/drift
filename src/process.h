#pragma once

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>

// Finds a helper program, preferring one shipped beside drift itself, as the
// Windows build bundles mpv and yt-dlp; otherwise the one on PATH.
inline QString findTool(const QString &name) {
    const QString bundled = QStandardPaths::findExecutable(name, {QCoreApplication::applicationDirPath()});
    if (!bundled.isEmpty())
        return bundled;
    const QString onPath = QStandardPaths::findExecutable(name);
    return onPath.isEmpty() ? name : onPath;
}

// yt-dlp is a console program, so without this Windows flashes a console
// window for every playlist, stream, and mix lookup.
inline void hideConsole(QProcess &process) {
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#else
    Q_UNUSED(process);
#endif
}
