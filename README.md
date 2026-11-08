# Drift

![Drift playing radio from a playlist](screenshot.png)

Endless YouTube Music radio from one of your playlists. Paste a playlist link
and Drift takes each song from it in turn, plays it, then plays a few tracks
from YouTube Music's radio for that song, and keeps going forever. Playlist
songs come back after every pass; nothing else repeats.

Audio plays through `mpv` with video off and a small buffer, and `yt-dlp`
reads the playlist, so the whole thing stays around 200 MB.

## Shortcuts

| Key | Action |
| --- | --- |
| Space | Play or pause |
| N | Next track |
| ← → | Seek 10 seconds |
| ↑ ↓ | Volume |
| Ctrl+L | Change playlist |
| Esc | Leave the link field |
| ? | Show shortcuts |
| Q | Quit |

## Build

```sh
bin/build      # build/drift
bin/test       # Qt Test, offscreen
bin/install    # makepkg -fsi, adds Drift to the app launcher
```

Needs `mpv` and `yt-dlp`. The playlist must be public or unlisted.
