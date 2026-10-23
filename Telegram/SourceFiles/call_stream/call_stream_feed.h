/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include "call_stream/call_stream_audio_encoder.h"
#include "call_stream/call_stream_feed_encoder.h"
#include "call_stream/call_stream_types.h"

#include <QImage>
#include <QSize>
#include <QtCore/QString>
#include <memory>
#include <functional>

namespace CallStream {

class StreamFeed {
public:
	StreamFeed(int index, const QString &folder, const std::shared_ptr<AudioEncoder> &audio);
	~StreamFeed();

	void submitVideoFrame(const QImage &frame, const QSize &size);
	void submitFrameData(const VideoFrameData &data);
	void submitAudioChunk(const QByteArray &data, qint64 ptsMs);
	void submitAudioPiece(const AudioPiece &piece);
	void finish();

	[[nodiscard]] const QString &folder() const { return _encoder.folder(); }
	[[nodiscard]] int index() const { return _index; }

	void setMaxHeight(int height) { _encoder.setMaxHeight(height); }
	[[nodiscard]] int maxHeight() const { return _encoder.maxHeight(); }
	[[nodiscard]] int missedDeadlines() const { return _encoder.missedDeadlines(); }
	void recordMissedDeadline() { _encoder.recordMissedDeadline(); }
	void setOnMissedDeadline(std::function<void()> callback) { _encoder.setOnMissedDeadline(std::move(callback)); }

private:
	int _index = 0;
	FeedEncoder _encoder;
	std::shared_ptr<AudioEncoder> _audio;
};

} // namespace CallStream