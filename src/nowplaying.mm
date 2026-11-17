#include "nowplaying.h"
#include "backend.h"

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>

#include <functional>

namespace {
// Shown with the track it was fetched for; a strong global, as nothing else owns it.
MPMediaItemArtwork *artwork = nil;
QString artworkId;

void handle(MPRemoteCommand *command, Backend *backend, std::function<void(Backend *)> action) {
    [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *) {
        // Not promised to be Qt's thread, where Backend lives.
        QMetaObject::invokeMethod(backend, [=] { action(backend); });
        return MPRemoteCommandHandlerStatusSuccess;
    }];
}
}

NowPlaying::NowPlaying(Backend *backend, QObject *parent) : QObject(parent) {
    MPRemoteCommandCenter *commands = MPRemoteCommandCenter.sharedCommandCenter;
    handle(commands.togglePlayPauseCommand, backend, [](Backend *b) { b->togglePause(); });
    handle(commands.playCommand, backend, [](Backend *b) { if (!b->playing()) b->togglePause(); });
    handle(commands.pauseCommand, backend, [](Backend *b) { if (b->playing()) b->togglePause(); });
    handle(commands.nextTrackCommand, backend, [](Backend *b) { b->next(); });
    handle(commands.previousTrackCommand, backend, [](Backend *b) { b->previous(); });
    [commands.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        const int seconds = int(static_cast<MPChangePlaybackPositionCommandEvent *>(event).positionTime);
        QMetaObject::invokeMethod(backend, [=] { backend->seekTo(seconds); });
        return MPRemoteCommandHandlerStatusSuccess;
    }];

    auto update = [backend] {
        MPNowPlayingInfoCenter *center = MPNowPlayingInfoCenter.defaultCenter;
        const QVariantMap current = backend->current();
        if (current.isEmpty()) {
            center.nowPlayingInfo = nil;
            center.playbackState = MPNowPlayingPlaybackStateStopped;
            return;
        }
        const QString id = current.value("id").toString();
        NSMutableDictionary *info = [NSMutableDictionary dictionary];
        info[MPMediaItemPropertyTitle] = current.value("title").toString().toNSString();
        info[MPMediaItemPropertyArtist] = current.value("artist").toString().toNSString();
        info[MPMediaItemPropertyPlaybackDuration] = @(backend->duration());
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(backend->position());
        info[MPNowPlayingInfoPropertyPlaybackRate] = @(backend->playing() ? 1.0 : 0.0);
        if (artwork && artworkId == id)
            info[MPMediaItemPropertyArtwork] = artwork;
        center.nowPlayingInfo = info;
        // macOS takes this, not the rate, as what the play/pause key should do.
        center.playbackState = backend->playing() ? MPNowPlayingPlaybackStatePlaying : MPNowPlayingPlaybackStatePaused;
    };
    // The position too, once a second: there is no signal for a seek alone.
    for (auto signal : {&Backend::currentChanged, &Backend::playingChanged, &Backend::durationChanged,
                        &Backend::positionChanged})
        connect(backend, signal, this, update);

    connect(backend, &Backend::currentChanged, this, [backend, update] {
        const QString id = backend->current().value("id").toString();
        if (id.isEmpty() || id == artworkId)
            return;
        artworkId = id;
        artwork = nil;
        NSURL *url = [NSURL URLWithString:("https://i.ytimg.com/vi/" + id + "/mqdefault.jpg").toNSString()];
        [[NSURLSession.sharedSession dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *, NSError *) {
            NSImage *image = data ? [[NSImage alloc] initWithData:data] : nil;
            if (!image)
                return;
            QMetaObject::invokeMethod(backend, [=] {
                if (artworkId != id) // skipped on while it loaded
                    return;
                artwork = [[MPMediaItemArtwork alloc] initWithBoundsSize:image.size
                                                          requestHandler:^NSImage *(CGSize) { return image; }];
                update();
            });
        }] resume];
    });
}
