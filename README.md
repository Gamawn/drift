# Drift

![Drift playing radio from a playlist](screenshot.png)

Endless YouTube Music radio from one of your playlists. Paste a playlist link
and Drift takes each song from it in turn, plays it, then plays a few tracks
from YouTube Music's radio for that song, and keeps going forever. Playlist
songs come back after every pass; nothing else repeats.

Or type a song name instead of a link: Drift searches YouTube Music, and the
song you pick starts an endless radio of similar songs that keeps moving from
one related mix to the next.

Audio plays through `mpv` with video off and a small buffer, and `yt-dlp`
reads the playlist, so the whole thing stays around 200 MB.

## Shortcuts

| Key | Action |
| --- | --- |
| Space | Play or pause |
| N | Next track |
| ← → | Seek 10 seconds |
| ↑ ↓ | Volume |
| Enter | Search, or play the picked song |
| Ctrl+L | Search or change playlist |
| Esc | Close search results, leave the field |
| ? | Show shortcuts |
| Q | Quit |

## Build

```sh
bin/build      # build/drift
bin/test       # Qt Test, offscreen
bin/install    # makepkg -fsi, adds Drift to the app launcher
```

Needs `mpv` and `yt-dlp`. The playlist must be public or unlisted.
