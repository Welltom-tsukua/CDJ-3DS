# 3DS One Deck DJ Player — interaction specification

## Screens

- **Top screen — 400 x 240 (non-stereoscopic):** current deck by default.  It
  shows title/artist, BPM, elapsed and remaining time, play state, pitch,
  active loop, and the current top-menu selection.
- **Top screen — Browser mode:** a scrollable rekordbox/device-library track
  browser replaces the deck view.  Loading a highlighted track returns to the
  deck view; the bottom screen stays in its runtime waveform view throughout.
- **Bottom touch screen — 320 x 240:** always the live waveform and transport
  interaction surface.  It never changes into a library screen.

## Hardware controls

| Input | Action |
| --- | --- |
| `B` | Toggle play / pause. |
| `A` | Cue: return to the stored cue position; when paused, set the cue at the current position. |
| `Y` (held) | Scratch / jog-top mode.  Horizontal stylus movement controls scrub direction and speed. |
| `X` | Context confirmation.  It opens the highlighted top-menu item, confirms Browser selections, and controls the loop-mode state machine below. |
| D-pad | Move in top menus and Browser rows; left/right changes a top-menu item or loop length. |
| Stylus on waveform | Tap to seek.  Horizontal drag scrubs forward or backward; releasing seeks to the chosen position.  With `Y` held, audio follows the drag as a scratch. |

## Top menu

The top menu is always visible along the top edge of the upper screen.

`BROWSER | LOOP | …`

- **BROWSER:** `X` opens the track browser on the upper screen.  D-pad changes
  the highlighted row; `X` loads the selection into the one deck.
- **LOOP:** `X` opens the beat-length picker:
  `1/4 | 1/2 | 1 | 2 | 4` beats.  D-pad chooses a value and `X` creates a loop
  beginning at the current beat-aligned play position.

## Loop X-button state machine

1. In normal deck mode, select `LOOP` and press `X` to open the beat picker.
2. Select a beat length and press `X` to activate the loop.
3. The next `X` exits the active loop but keeps loop mode/picker open.
4. One more `X` closes loop mode and returns to normal deck operation.

## Transport and codecs

Audio is decoded from SD in a streaming ring buffer and queued to `ndsp`.
MP3 and M4A are required.  M4A handling must inspect the contained codec;
AAC and ALAC are separate decoder paths.  DRM-protected media is out of scope.

Forward seeks restart decode at a suitable compressed-frame boundary.  Reverse
and scratch playback use a rolling decoded PCM cache, so short reverse motion
is immediate.  Larger backward jumps first seek and refill the cache; this is
the necessary behaviour for compressed MP3/AAC/ALAC rather than pretending
those streams can be decoded backwards directly.
