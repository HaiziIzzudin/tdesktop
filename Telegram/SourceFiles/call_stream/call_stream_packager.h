/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <mutex>

struct AVFormatContext;
struct AVStream;
struct AVPacket;

namespace CallStream {

class Packager {
public:
	explicit Packager(const QString &folder);
	~Packager();

	void writeVideoPacket(const QByteArray &data, qint64 pts, bool keyFrame);
	void finish();
	[[nodiscard]] const QString &folder() const { return _folder; }

private:
	bool open();
	void close();

	QString _folder;
	QString _playlist;
	AVFormatContext *_fmt = nullptr;
	AVStream *_videoStream = nullptr;
	bool _headerWritten = false;
	bool _ended = false;
	std::mutex _mutex;
};

} // namespace CallStream