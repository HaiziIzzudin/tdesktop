/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_audio_encoder.h"

namespace CallStream {

AudioEncoder::AudioEncoder() = default;

AudioEncoder::~AudioEncoder() = default;

void AudioEncoder::setChunkCallback(ChunkCallback callback) {
	_callback = std::move(callback);
}

void AudioEncoder::submitPiece(const AudioPiece &piece) {
	// Shared AAC-LC encode will be implemented here. For now the seam exists
	// so feeds can be wired without a parallel test-only path.
	if (_callback) {
		// Placeholder: forward the raw piece bytes as if encoded.
		_callback(piece.samples, piece.timestampMs);
	}
}

void AudioEncoder::finish() {
}

} // namespace CallStream