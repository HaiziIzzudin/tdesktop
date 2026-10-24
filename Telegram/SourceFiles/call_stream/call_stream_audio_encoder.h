/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include "call_stream/call_stream_types.h"

#include <QtCore/QObject>
#include <functional>
#include <memory>
#include <vector>

namespace CallStream {

class AudioEncoder {
public:
	using ChunkCallback = std::function<void(
		const QByteArray &data,
		qint64 ptsSamples,
		qint64 durationSamples,
		qint64 startTimestampMs)>;

	AudioEncoder();
	~AudioEncoder();

	void setChunkCallback(ChunkCallback callback);
	void submitPiece(const AudioPiece &piece);
	void finish();

	[[nodiscard]] AudioParams params() const;

private:
	void encodeFrame(const int16_t *samples, int count);
	void drainPackets();

	ChunkCallback _callback;
	AudioParams _params;
	void *_ctx = nullptr;
	void *_frame = nullptr;
	void *_swr = nullptr;
	int _swrRate = 0;
	int _swrChannels = 0;
	QByteArray _pending;
	qint64 _pts = 0;
	qint64 _outputPts = 0;
	qint64 _startTimestampMs = 0;
	bool _startSet = false;
};

} // namespace CallStream
