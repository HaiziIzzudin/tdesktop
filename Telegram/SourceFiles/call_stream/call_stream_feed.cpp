/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_feed.h"

namespace CallStream {

StreamFeed::StreamFeed(
	int index,
	const QString &folder,
	const QString &slot,
	const std::shared_ptr<AudioEncoder> &audio)
: _index(index)
, _packager(folder, slot)
, _encoder(folder, &_packager)
, _audio(audio) {
	if (_audio) {
		_packager.setAudioParams(_audio->params());
	}
}

StreamFeed::~StreamFeed() = default;

void StreamFeed::submitVideoFrame(const QImage &frame, const QSize &) {
	_encoder.submitFrame(frame);
}

void StreamFeed::submitFrameData(const VideoFrameData &data) {
	_encoder.submitFrameData(data);
}

void StreamFeed::submitAudioChunk(
		const QByteArray &data,
		qint64 ptsSamples,
		qint64 durationSamples,
		qint64 startTimestampMs) {
	_encoder.submitAudioChunk(
		data,
		ptsSamples,
		durationSamples,
		startTimestampMs);
}

void StreamFeed::submitAudioPiece(const AudioPiece &piece) {
	if (_audio) {
		_audio->submitPiece(piece);
	}
}

void StreamFeed::finish() {
	_encoder.finish();
	_packager.finish();
}

} // namespace CallStream
