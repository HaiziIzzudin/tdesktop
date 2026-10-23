/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include "call_stream/call_stream_types.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QByteArray>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <functional>
namespace CallStream {

class FeedEncoder {
public:
	explicit FeedEncoder(const QString &folder);
	~FeedEncoder();

	void submitFrame(const QImage &frame);
	void submitFrameData(const VideoFrameData &data);
	void submitAudioChunk(const QByteArray &data, qint64 ptsMs);
	void finish();

	[[nodiscard]] const QString &folder() const { return _folder; }

	// Missed-deadline tracking (pressure signal).
	[[nodiscard]] int missedDeadlines() const { return _missedDeadlines.load(); }
	void recordMissedDeadline();
	void setOnMissedDeadline(std::function<void()> callback) { _onMissedDeadline = std::move(callback); }

	// Shrink ladder: max encoded height (1080/720/480/360). 0 = use delivered (1080 cap).
	void setMaxHeight(int height) { _maxHeight.store(height > 0 ? height : 1080); }
	[[nodiscard]] int maxHeight() const { return _maxHeight.load(); }

private:
	void workerLoop();
	void openEncoder(int width, int height);
	void encodeOne(const VideoFrameData &data);
	void closeEncoder();

	QString _folder;
	std::thread _thread;
	std::mutex _mutex;
	std::condition_variable _cv;
	std::queue<VideoFrameData> _queue;
	bool _finished = false;

	void *_codec = nullptr; // AVCodecContext*
	void *_frame = nullptr; // AVFrame*
	int _width = 0;
	int _height = 0;
	std::chrono::steady_clock::time_point _lastFrameTime{};
	bool _lastFrameTimeSet = false;

	std::atomic<int> _missedDeadlines{ 0 };
	std::atomic<int> _maxHeight{ 1080 };
	std::function<void()> _onMissedDeadline;
};

} // namespace CallStream