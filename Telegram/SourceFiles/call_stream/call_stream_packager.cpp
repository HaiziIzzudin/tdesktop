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
	bool useHls = true;
	const std::string outPath = _playlist.toStdString();
	if (avformat_alloc_output_context2(&_fmt, nullptr, "hls", outPath.c_str()) < 0 || !_fmt) {
		useHls = false;
		// Fallback: mpegts into stream.ts, and write playlist.m3u8 manually.
		const QString tsPath = _folder + u"/stream.ts"_q;
		if (avformat_alloc_output_context2(&_fmt, nullptr, "mpegts", tsPath.toStdString().c_str()) < 0 || !_fmt) {
			return false;
		}
		// Write a minimal playlist referencing stream.ts.
		QFile pl(_folder + u"/playlist.m3u8"_q);
		if (pl.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			pl.write("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:10.0,\nstream.ts\n");
			pl.close();
		}
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

	_audioStream = avformat_new_stream(_fmt, nullptr);
	if (_audioStream) {
		_audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
		_audioStream->codecpar->codec_id = AV_CODEC_ID_AAC;
		_audioStream->codecpar->sample_rate = 48000;
		av_channel_layout_from_string(&_audioStream->codecpar->ch_layout, "stereo");
		_audioStream->time_base = AVRational{ 1, 48000 };
	}

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

void Packager::writeAudioPacket(const QByteArray &data, qint64 ptsMs) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_fmt || !_headerWritten || _ended || !_audioStream) return;
	AVPacket *pkt = av_packet_alloc();
	if (!pkt) return;
	pkt->data = (uint8_t*)data.data();
	pkt->size = data.size();
	pkt->stream_index = _audioStream->index;
	pkt->pts = ptsMs * 48; // ms -> 48k samples
	pkt->dts = pkt->pts;
	av_packet_rescale_ts(pkt, AVRational{ 1, 48000 }, _audioStream->time_base);
	av_interleaved_write_frame(_fmt, pkt);
	av_packet_free(&pkt);

	// Ensure live playlist never carries ENDLIST (muxer may add it).
	QFile pl(_folder + u"/playlist.m3u8"_q);
	if (pl.exists()) {
		QString content;
		if (pl.open(QIODevice::ReadOnly)) {
			content = QString::fromUtf8(pl.readAll());
			pl.close();
		}
		content.remove(u"#EXT-X-ENDLIST\n"_q);
		content.remove(u"#EXT-X-ENDLIST"_q);
		if (pl.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			pl.write(content.toUtf8());
			pl.close();
		}
	}
}

void Packager::finish() {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_fmt || _ended) return;
	_ended = true;
	if (_headerWritten) {
		av_write_trailer(_fmt);
	}
	QFile pl(_folder + u"/playlist.m3u8"_q);
	if (pl.open(QIODevice::Append)) {
		pl.write("#EXT-X-ENDLIST\n");
		pl.close();
	}
}

} // namespace CallStream