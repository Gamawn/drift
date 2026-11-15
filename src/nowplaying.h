#pragma once

#include <QObject>

class Backend;

// macOS' Now Playing: the media keys and Control Center's tile play, pause,
// skip and seek Drift and show its track, without switching to it.
// Only this file's .mm sees Objective-C.
class NowPlaying : public QObject {
public:
    explicit NowPlaying(Backend *backend, QObject *parent = nullptr);
};
