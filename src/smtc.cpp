// C++/WinRT first: it must not see Qt's emit/signals/slots macros.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>

#include "smtc.h"
#include "backend.h"

using namespace winrt::Windows::Media;

namespace {
winrt::hstring wide(const QString &text) { return winrt::hstring(text.toStdWString()); }
}

struct Smtc::Private {
    // A MediaPlayer with its own playback switched off is the way a desktop app
    // without a window handle gets the controls; mpv does the playing.
    Playback::MediaPlayer player{nullptr};
    SystemMediaTransportControls controls{nullptr};
    SystemMediaTransportControls::ButtonPressed_revoker pressed;
};

Smtc::Smtc(Backend *backend, QObject *parent) : QObject(parent), d(std::make_unique<Private>()) {
    try {
        d->player = Playback::MediaPlayer();
        d->player.CommandManager().IsEnabled(false);
        d->controls = d->player.SystemMediaTransportControls();
        d->controls.IsEnabled(true);
        d->controls.IsPlayEnabled(true);
        d->controls.IsPauseEnabled(true);
        d->controls.IsNextEnabled(true);
        d->pressed = d->controls.ButtonPressed(winrt::auto_revoke, [this, backend](const auto &, const auto &args) {
            const auto button = args.Button();
            // Windows calls this on its own thread; Backend lives on Qt's.
            QMetaObject::invokeMethod(this, [backend, button] {
                if (button == SystemMediaTransportControlsButton::Next)
                    backend->next();
                else if (button == SystemMediaTransportControlsButton::Play && !backend->playing())
                    backend->togglePause();
                else if (button == SystemMediaTransportControlsButton::Pause && backend->playing())
                    backend->togglePause();
            }, Qt::QueuedConnection);
        });
    } catch (const winrt::hresult_error &error) {
        qWarning("Drift has no Windows media controls: %ls", error.message().c_str());
        d->controls = nullptr;
        return;
    }

    auto update = [this, backend] {
        try {
            const QVariantMap current = backend->current();
            d->controls.PlaybackStatus(current.isEmpty()   ? MediaPlaybackStatus::Stopped
                                       : backend->playing() ? MediaPlaybackStatus::Playing
                                                            : MediaPlaybackStatus::Paused);
            auto display = d->controls.DisplayUpdater();
            display.ClearAll();
            if (!current.isEmpty()) {
                display.Type(MediaPlaybackType::Music);
                display.MusicProperties().Title(wide(current.value("title").toString()));
                display.MusicProperties().Artist(wide(current.value("artist").toString()));
                display.Thumbnail(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(
                    winrt::Windows::Foundation::Uri(
                        wide("https://i.ytimg.com/vi/" + current.value("id").toString() + "/mqdefault.jpg"))));
            }
            display.Update();
        } catch (const winrt::hresult_error &) {
            // The flyout just shows stale details; playback is unaffected.
        }
    };
    connect(backend, &Backend::currentChanged, this, update);
    connect(backend, &Backend::playingChanged, this, update);
}

Smtc::~Smtc() = default;
