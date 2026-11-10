// C++/WinRT first: it must not see Qt's emit/signals/slots macros.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <qt_windows.h>

#include "smtc.h"
#include "backend.h"

using namespace winrt::Windows::Media;

namespace {
winrt::hstring wide(const QString &text) { return winrt::hstring(text.toStdWString()); }
}

// System-wide hotkeys: the media keys, since SMTC alone doesn't get them to Drift
// on every machine, and Alt combos by WASD for keyboards without them. A key
// another app already holds is left to it.
struct Hotkeys : QAbstractNativeEventFilter {
    struct Key {
        UINT modifiers, code;
        void (*action)(Backend *);
    };
    static constexpr Key keys[] = {
        {MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE, [](Backend *backend) { backend->togglePause(); }},
        {MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK, [](Backend *backend) { backend->next(); }},
        {MOD_NOREPEAT, VK_MEDIA_PREV_TRACK, [](Backend *backend) { backend->previous(); }},
        // CS2 leaves Alt unbound, so these don't fight the game; Alt+Z is NVIDIA's overlay.
        {MOD_ALT | MOD_NOREPEAT, 'X', [](Backend *backend) { backend->togglePause(); }},
        {MOD_ALT | MOD_NOREPEAT, 'C', [](Backend *backend) { backend->next(); }},
        // Volume repeats while held. The keyboard's own volume keys stay with Windows.
        {MOD_ALT, 'V', [](Backend *backend) { backend->changeVolume(-5); }},
        {MOD_ALT, 'B', [](Backend *backend) { backend->changeVolume(5); }},
    };
    Backend *backend;

    explicit Hotkeys(Backend *backend) : backend(backend) {
        for (int id = 0; id < int(std::size(keys)); ++id)
            if (!RegisterHotKey(nullptr, id, keys[id].modifiers, keys[id].code))
                qWarning("Drift couldn't take hotkey 0x%x+0x%x, another app holds it", keys[id].modifiers, keys[id].code);
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
    ~Hotkeys() override {
        for (int id = 0; id < int(std::size(keys)); ++id)
            UnregisterHotKey(nullptr, id);
    }
    // RegisterHotKey without a window posts WM_HOTKEY to this thread's queue, which Qt passes here.
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message != WM_HOTKEY || msg->wParam >= std::size(keys))
            return false;
        keys[msg->wParam].action(backend);
        return true;
    }
};

struct Smtc::Private {
    explicit Private(Backend *backend) : hotkeys(backend) {}
    Hotkeys hotkeys;
    // A MediaPlayer with its own playback switched off is the way a desktop app
    // without a window handle gets the controls; mpv does the playing.
    Playback::MediaPlayer player{nullptr};
    SystemMediaTransportControls controls{nullptr};
    SystemMediaTransportControls::ButtonPressed_revoker pressed;
};

Smtc::Smtc(Backend *backend, QObject *parent) : QObject(parent), d(std::make_unique<Private>(backend)) {
    try {
        d->player = Playback::MediaPlayer();
        d->player.CommandManager().IsEnabled(false);
        d->controls = d->player.SystemMediaTransportControls();
        d->controls.IsEnabled(true);
        d->controls.IsPlayEnabled(true);
        d->controls.IsPauseEnabled(true);
        d->controls.IsNextEnabled(true);
        d->controls.IsPreviousEnabled(true);
        d->pressed = d->controls.ButtonPressed(winrt::auto_revoke, [this, backend](const auto &, const auto &args) {
            const auto button = args.Button();
            // Windows calls this on its own thread; Backend lives on Qt's.
            QMetaObject::invokeMethod(this, [backend, button] {
                if (button == SystemMediaTransportControlsButton::Next)
                    backend->next();
                else if (button == SystemMediaTransportControlsButton::Previous)
                    backend->previous();
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
