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

#include <QtCore/QDir>
#include <QtCore/QFile>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CallStream {

namespace {

constexpr auto kVideoTimeBase = AVRational{ 1, 30 };
constexpr auto kMaxPendingAudioMs = qint64(5000);
constexpr auto kMaxPendingAudioChunks = size_t(400);

void RemoveOldFiles(const QString &folder) {
	auto dir = QDir(folder);
	const auto names = dir.entryList(
		{ u"*.ts"_q, u"*.m3u8"_q, u"*.tmp"_q },
		QDir::Files);
	for (const auto &name : names) {
		dir.remove(name);
	}
}

void SetExtradata(AVCodecParameters *par, const QByteArray &extradata) {
	if (extradata.isEmpty()) {
		return;
	}
	par->extradata = static_cast<uint8_t*>(av_mallocz(
		extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
	if (!par->extradata) {
		return;
	}
	std::memcpy(par->extradata, extradata.constData(), extradata.size());
	par->extradata_size = extradata.size();
}

} // namespace

Packager::Packager(const QString &folder, const QString &prefix)
: _folder(folder)
, _prefix(prefix) {
	QDir().mkpath(folder);
}

Packager::~Packager() {
	finish();
	close();
}

void Packager::setVideoParams(
		int width,
		int height,
		const QByteArray &extradata) {
	std::lock_guard<std::mutex> lock(_mutex);
	_videoWidth = width;
	_videoHeight = height;
	_videoExtradata = extradata;
}

void Packager::setAudioParams(const AudioParams &params) {
	std::lock_guard<std::mutex> lock(_mutex);
	_audioParams = params;
}

bool Packager::ensureOpen(qint64 videoTimestampMs) {
	if (_opened) {
		return true;
	} else if (_openFailed) {
		return false;
	} else if (_videoWidth <= 0 || _videoHeight <= 0) {
		return false;
	} else if (videoTimestampMs <= 0) {
		return false;
	}
	RemoveOldFiles(_folder);

	_playlist = _folder + u"/playlist.m3u8"_q;
	const auto outPath = _playlist.toStdString();
	if (avformat_alloc_output_context2(
			&_fmt,
			nullptr,
			"hls",
			outPath.c_str()) < 0 || !_fmt) {
		const auto tsPath = _folder + u"/stream.ts"_q;
		if (avformat_alloc_output_context2(
				&_fmt,
				nullptr,
				"mpegts",
				tsPath.toStdString().c_str()) < 0 || !_fmt) {
			_openFailed = true;
			return false;
		}
		_fallback = true;
		QFile pl(_playlist);
		if (pl.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			pl.write("#EXTM3U\n"
				"#EXT-X-VERSION:3\n"
				"#EXT-X-TARGETDURATION:10\n"
				"#EXT-X-MEDIA-SEQUENCE:0\n"
				"#EXTINF:10.0,\n"
				"stream.ts\n");
			pl.close();
		}
	} else {
		av_opt_set(_fmt->priv_data, "hls_time", "2", 0);
		av_opt_set(_fmt->priv_data, "hls_list_size", "7", 0);
		av_opt_set(_fmt->priv_data, "hls_flags", "delete_segments", 0);
		const auto name = _prefix.isEmpty()
			? u"seg%03d.ts"_q
			: _prefix + u"-seg%03d.ts"_q;
		const auto pattern = (_folder + u"/"_q + name).toStdString();
		av_opt_set(
			_fmt->priv_data,
			"hls_segment_filename",
			pattern.c_str(),
			0);
	}

	_videoStream = avformat_new_stream(_fmt, nullptr);
	if (!_videoStream) {
		_openFailed = true;
		return false;
	}
	_videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	_videoStream->codecpar->codec_id = AV_CODEC_ID_H264;
	_videoStream->codecpar->width = _videoWidth;
	_videoStream->codecpar->height = _videoHeight;
	SetExtradata(_videoStream->codecpar, _videoExtradata);
	_videoStream->time_base = kVideoTimeBase;

	if (_audioParams.sampleRate > 0 && _audioParams.channels > 0) {
		_audioStream = avformat_new_stream(_fmt, nullptr);
		if (_audioStream) {
			auto par = _audioStream->codecpar;
			par->codec_type = AVMEDIA_TYPE_AUDIO;
			par->codec_id = AV_CODEC_ID_AAC;
			par->sample_rate = _audioParams.sampleRate;
			av_channel_layout_default(
				&par->ch_layout,
				_audioParams.channels);
			SetExtradata(par, _audioParams.extradata);
			_audioStream->time_base = AVRational{
				1,
				_audioParams.sampleRate,
			};
		}
	}

	if (!(_fmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&_fmt->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
			_openFailed = true;
			return false;
		}
	}
	if (avformat_write_header(_fmt, nullptr) < 0) {
		_openFailed = true;
		return false;
	}

	_startTimestampMs = videoTimestampMs;
	if (!_pendingAudio.empty()) {
		const auto first = _pendingAudio.front().startTimestampMs;
		if (first > 0) {
			_startTimestampMs = std::min(_startTimestampMs, first);
		}
	}
	_opened = true;

	for (const auto &audio : _pendingAudio) {
		writeAudioLocked(audio);
	}
	_pendingAudio.clear();
	return true;
}

void Packager::writeVideoPacket(
		const QByteArray &data,
		qint64 timestampMs,
		bool keyFrame) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (_ended) {
		return;
	}
	if (!ensureOpen(timestampMs)) {
		return;
	}
	const auto pts = qint64(std::llround(
		(timestampMs - _startTimestampMs) * 30.0 / 1000.0));
	if (pts <= _lastVideoPts) {
		return;
	}
	_lastVideoPts = pts;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		return;
	}
	if (av_new_packet(pkt, data.size()) < 0) {
		av_packet_free(&pkt);
		return;
	}
	std::memcpy(pkt->data, data.constData(), data.size());
	pkt->stream_index = _videoStream->index;
	pkt->pts = pkt->dts = pts;
	pkt->duration = 1;
	pkt->flags = keyFrame ? AV_PKT_FLAG_KEY : 0;
	av_packet_rescale_ts(pkt, kVideoTimeBase, _videoStream->time_base);
	av_interleaved_write_frame(_fmt, pkt);
	av_packet_free(&pkt);
}

void Packager::writeAudioPacket(
		const QByteArray &data,
		qint64 ptsSamples,
		qint64 durationSamples,
		qint64 startTimestampMs) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (_ended) {
		return;
	}
	PendingAudio audio;
	audio.data = data;
	audio.ptsSamples = ptsSamples;
	audio.durationSamples = durationSamples;
	audio.startTimestampMs = startTimestampMs;
	if (!_opened) {
		_pendingAudio.push_back(std::move(audio));
		while (_pendingAudio.size() > kMaxPendingAudioChunks
			|| (_pendingAudio.size() > 2
				&& _pendingAudio.back().startTimestampMs
					- _pendingAudio.front().startTimestampMs
					> kMaxPendingAudioMs)) {
			_pendingAudio.erase(_pendingAudio.begin());
		}
		return;
	}
	writeAudioLocked(audio);
}

void Packager::writeAudioLocked(const PendingAudio &audio) {
	if (!_audioStream) {
		return;
	}
	const auto rate = _audioParams.sampleRate;
	auto pts = audio.ptsSamples;
	if (audio.startTimestampMs > 0) {
		pts += (audio.startTimestampMs - _startTimestampMs) * rate / 1000;
	}
	pts = std::max<qint64>(pts, 0);
	pts = std::max(pts, _lastAudioPts + 1);
	_lastAudioPts = pts;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		return;
	}
	if (av_new_packet(pkt, audio.data.size()) < 0) {
		av_packet_free(&pkt);
		return;
	}
	std::memcpy(pkt->data, audio.data.constData(), audio.data.size());
	pkt->stream_index = _audioStream->index;
	pkt->pts = pkt->dts = pts;
	pkt->duration = audio.durationSamples;
	av_packet_rescale_ts(
		pkt,
		AVRational{ 1, rate },
		_audioStream->time_base);
	av_interleaved_write_frame(_fmt, pkt);
	av_packet_free(&pkt);
}

void Packager::finish() {
	std::lock_guard<std::mutex> lock(_mutex);
	if (_ended) {
		return;
	}
	_ended = true;
	if (!_opened) {
		return;
	}
	av_write_trailer(_fmt);
	if (_fallback) {
		QFile pl(_playlist);
		if (pl.open(QIODevice::Append)) {
			pl.write("#EXT-X-ENDLIST\n");
			pl.close();
		}
	}
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

} // namespace CallStream
