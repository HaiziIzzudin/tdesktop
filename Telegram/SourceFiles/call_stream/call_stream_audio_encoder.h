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
	using ChunkCallback = std::function<void(const QByteArray &data, qint64 ptsMs)>;

	AudioEncoder();
	~AudioEncoder();

	void setChunkCallback(ChunkCallback callback);
	void submitPiece(const AudioPiece &piece);
	void finish();

private:
	ChunkCallback _callback;
};

} // namespace CallStream