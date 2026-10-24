/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtCore/QSize>
#include <vector>
#include <cstdint>

namespace CallStream {

struct AudioPiece {
	QByteArray samples;
	qint64 timestampMs = 0;
	int sampleRate = 48000;
	int channels = 2;
};

struct AudioParams {
	int sampleRate = 0;
	int channels = 0;
	QByteArray extradata;
};

struct VideoFrameData {
	QImage image;
	qint64 timestampMs = 0;
	bool hasYuv420 = false;
	QSize yuvSize;
	QSize chromaSize;
	std::vector<uint8_t> y;
	std::vector<uint8_t> u;
	std::vector<uint8_t> v;
	int yStride = 0;
	int uStride = 0;
	int vStride = 0;
	int rotation = 0;
};

} // namespace CallStream