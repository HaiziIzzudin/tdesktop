# Call pictures decode on the graphics chip through our own copy of the call engine, with frames copied back

On Windows we decode H.264 call pictures with the graphics chip, and we do it by
changing the decoder inside our own copy of the call engine rather than adding a
second decoder in the app; the decoded picture is then copied back into ordinary
memory so call screens stay untouched. Our own decode device is opened on the chip
that drives the screen instead of borrowing the window's device, and only H.264 is
accelerated, so nothing has to be rebuilt to ship it. Every one of these choices
picks the smallest change that can produce a measurable win, because the plumbing
already exists in the engine and in the app's file-playback path, and because the
alternative on each axis either duplicates maintenance or touches several call
screens before we know what the copy-back costs.

Status: accepted

## Considered options

- **A separate decoder in the app, handed to the engine.** Rejected: it duplicates
  the codec setup the engine's decoder already has, and leaves two places to keep
  working when the media library changes.
- **Direct-draw handoff from the start.** Rejected for the first version: it needs
  device sharing between the decoder and the window, a new picture form through the
  call video track, and a new drawing path in several call screens, all before we
  have a measured number saying the copy-back actually hurts.
- **Borrowing the window's graphics device.** Rejected for the first version: it
  ties decoder lifetime and threading to the UI's device, which makes the first
  version harder and buys nothing while frames are copied back anyway.
- **Rebuilding the media library to accelerate VP9 as well.** Rejected: it turns a
  contained change into a shared dependency rebuild, while H.264 is the common
  case for both call kinds and the only codec with a complete Windows hardware
  path in the library we already ship.

## Consequences

- The call engine library diverges from upstream, so its change lives in our
  dependency fork and must be committed and published there, not in this project.
- Copy-back is permanent until someone measures it and reopens the handoff
  decision; a hardware picture will therefore always pass through ordinary memory
  on its way to a call screen.
- Only H.264 streams can benefit on Windows. VP8 streams can never be accelerated
  there, and VP9 streams stay software until the media library is rebuilt.
- If the graphics chip refuses, calls fall back to software decoding, so the
  feature can only make calls cheaper, never break them.
