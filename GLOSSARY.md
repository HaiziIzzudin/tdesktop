# Glossary

Vocabulary for work on exposing a joined Telegram Desktop call as a local,
standard media stream (the "call-stream" wayfinding effort). Other areas add
their terms here as they crystallise.

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
