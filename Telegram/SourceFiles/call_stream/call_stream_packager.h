/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include "call_stream/call_stream_types.h"

#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <mutex>
#include <vector>

struct AVFormatContext;
struct AVStream;
struct AVPacket;

namespace CallStream {

class Packager {
public:
	explicit Packager(const QString &folder, const QString &prefix = QString());
	~Packager();

	void setVideoParams(int width, int height, const QByteArray &extradata);
	void setAudioParams(const AudioParams &params);

	void writeVideoPacket(
		const QByteArray &data,
		qint64 timestampMs,
		bool keyFrame);
	void writeAudioPacket(
		const QByteArray &data,
		qint64 ptsSamples,
		qint64 durationSamples,
		qint64 startTimestampMs);
	void finish();
	[[nodiscard]] const QString &folder() const { return _folder; }

private:
	struct PendingAudio {
		QByteArray data;
		qint64 ptsSamples = 0;
		qint64 durationSamples = 0;
		qint64 startTimestampMs = 0;
	};

	bool ensureOpen(qint64 videoTimestampMs);
	void writeAudioLocked(const PendingAudio &audio);
	void close();

	QString _folder;
	QString _prefix;
	QString _playlist;
	AVFormatContext *_fmt = nullptr;
	AVStream *_videoStream = nullptr;
	AVStream *_audioStream = nullptr;
	AudioParams _audioParams;
	int _videoWidth = 0;
	int _videoHeight = 0;
	QByteArray _videoExtradata;
	qint64 _startTimestampMs = 0;
	qint64 _lastVideoPts = -1;
	qint64 _lastAudioPts = -1;
	bool _opened = false;
	bool _openFailed = false;
	bool _ended = false;
	std::vector<PendingAudio> _pendingAudio;
	std::mutex _mutex;
};

} // namespace CallStream
