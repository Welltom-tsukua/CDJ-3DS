# 3DS One Deck

Initial executable UI foundation for the one-deck rekordbox player.

This build implements the two-screen interaction model and state machine only:

- top 400x240 deck / Browser / LOOP controls
- bottom 320x240 runtime waveform and touch scrub surface
- `B` play/pause, `A` cue, `Y` scratch modifier, `X` contextual confirmation,
  D-pad navigation
- loop selections of 1/4, 1/2, 1, 2 and 4 beats

The next source layer is the streaming audio and rekordbox parser:

- `sdmc:/PIONEER/rekordbox/export.pdb` for Device Library rows
- `sdmc:/PIONEER/USBANLZ` for beat grids and overview waveform data
- MP3 and AAC-in-M4A streaming decode to ndsp

Build from a devkitPro MSYS shell with `make`.
