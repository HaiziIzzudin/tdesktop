/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_packager.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <QDir>
#include <QFile>

namespace CallStream {

Packager::Packager(const QString &folder) : _folder(folder) {
	QDir().mkpath(folder);
	open();
}

Packager::~Packager() {
	finish();
	close();
}

bool Packager::open() {
	_playlist = _folder + u"/playlist.m3u8"_q;
	const std::string outPath = _playlist.toStdString();
	if (avformat_alloc_output_context2(&_fmt, nullptr, "hls", outPath.c_str()) < 0 || !_fmt) {
		return false;
	}
	// hls options
	av_opt_set(_fmt->priv_data, "hls_time", "2", 0);
	av_opt_set(_fmt->priv_data, "hls_list_size", "7", 0);
	av_opt_set(_fmt->priv_data, "hls_flags", "delete_segments", 0);
	const std::string segPattern = (_folder + u"/seg%03d.ts"_q).toStdString();
	av_opt_set(_fmt->priv_data, "hls_segment_filename", segPattern.c_str(), 0);

	_videoStream = avformat_new_stream(_fmt, nullptr);
	if (!_videoStream) return false;
	_videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	_videoStream->codecpar->codec_id = AV_CODEC_ID_H264;
	_videoStream->codecpar->width = 640;
	_videoStream->codecpar->height = 480;
	_videoStream->time_base = AVRational{ 1, 30 };

	if (!(_fmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&_fmt->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
			return false;
		}
	}
	if (avformat_write_header(_fmt, nullptr) < 0) {
		return false;
	}
	_headerWritten = true;
	return true;
}

void Packager::close() {
	if (_fmt) {
		if (_fmt->pb) {
			avio_closep(&_fmt->pb);
		}
		avformat_free_context(_fmt);
		_fmt = nullptr;
	}
}

void Packager::writeVideoPacket(const QByteArray &data, qint64 pts, bool keyFrame) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_fmt || !_headerWritten || _ended) return;
	AVPacket *pkt = av_packet_alloc();
	if (!pkt) return;
	pkt->data = (uint8_t*)data.data();
	pkt->size = data.size();
	pkt->stream_index = _videoStream->index;
	pkt->pts = pts;
	pkt->dts = pts;
	pkt->flags = keyFrame ? AV_PKT_FLAG_KEY : 0;
	// rescale pts from 1/30 to stream time_base (same)
	av_packet_rescale_ts(pkt, AVRational{ 1, 30 }, _videoStream->time_base);
	av_interleaved_write_frame(_fmt, pkt);
	av_packet_free(&pkt);
}

void Packager::finish() {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_fmt || _ended) return;
	_ended = true;
	if (_headerWritten) {
		av_write_trailer(_fmt);
	}
}

} // namespace CallStream