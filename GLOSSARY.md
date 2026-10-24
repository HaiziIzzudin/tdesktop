# Glossary

Vocabulary for work on Telegram Desktop calls: exposing a joined call as a local
media stream (the "call-stream" wayfinding effort), and decoding call pictures.
Other areas add their terms here as they crystallise.

## Language

**Feed**:
The live media of one video source in a joined call — one other person's
camera, or one shared screen — published as a standard network stream on the
user's own computer, carrying that picture and the whole call's sound.
_Avoid_: broadcast, relay, restream, output stream

**Stream link**:
The plain-text address handed back for one feed, the thing a player opens.
Fixed per person and source kind (a person with a camera and a shared screen
has two fixed links); the name lives in the feed listing, not in the address.
_Avoid_: watch URL, stream endpoint, playback address

**Feed listing**:
The call stream service's plain-text answer naming every live feed and its
stream link, one line each.
_Avoid_: feed index, channel list, roster

**Call stream service**:
The small local web service that answers a request with the feed listing and
the stream links.
_Avoid_: REST server, streaming gateway, media API

**Joined call**:
A call the user is participating in right now, one-on-one or group, whose
media is in scope for the feed.
_Avoid_: active call, current call, session

## Calls and their pictures

**Call**:
A live audio or video conversation the app joins, one-on-one or group. Not a
recorded voice message, not a video file, not a livestream the app merely watches.
_Avoid_: Meeting, session, conference

**Participant picture**:
The picture belonging to one other person in a call: either their camera or their
shared screen. It is what the call screen draws for that person, and it is not the
published feed.
_Avoid_: Feed, stream, tile content

**Shared screen**:
A participant picture that shows that person's whole screen instead of a camera.
_Avoid_: Screenshare, broadcast, screensharing

**Call screen**:
The part of the window that paints participant pictures during a call: the
one-on-one view, a group tile, or the enlarged single view.
_Avoid_: Viewport, renderer, canvas

**Call picture path**:
The whole route an incoming picture takes, from arrival over the network to being
painted by a call screen.
_Avoid_: Video pipeline, playback path (both already mean the app's video-file
player, which is a different thing)

## Decoding

**Hardware decoding**:
Decoding done by a fixed-function circuit inside the graphics chip instead of by
ordinary processor instructions.
_Avoid_: GPU acceleration, hardware acceleration (both are too broad: drawing,
capturing and encoding are also graphics work and are not what this means)

**Software decoding**:
Decoding done by ordinary processor instructions. This is what every call picture
gets today on Windows and Linux.
_Avoid_: CPU decoding, fallback decoding

**Decode device**:
The graphics device a decoder uses to decode. It is not necessarily the device the
window draws with.
_Avoid_: GPU, adapter, context

**Frame handoff**:
The step that moves a decoded picture from wherever decoding produced it into the
form a call screen accepts.
_Avoid_: Frame transfer, upload, copy (each names only one of the two possible
handoffs)

**Copy-back handoff**:
A frame handoff that brings the picture back into ordinary memory, so call screens
stay exactly as they are.
_Avoid_: Readback, download

**Direct-draw handoff**:
A frame handoff that leaves the picture in graphics memory and makes the call
screen draw it there. Not built yet.
_Avoid_: Zero-copy, texture path

**Software fallback**:
Serving a call picture with software decoding after hardware decoding turned out
to be unusable for that picture. It is the expected outcome, not an error.
_Avoid_: Degraded mode, rescue path

**Call acceleration switch**:
The user-facing on/off for hardware decoding of call pictures. It is deliberately
separate from the older setting that governs video files and stories.
_Avoid_: Video setting, hardware acceleration setting

## Verification

**Reference call**:
The fixed private group call used to compare the app's processor use before and
after a decoding change: same participant count, same quality tier, same duration.
_Avoid_: Test call, benchmark, baseline
