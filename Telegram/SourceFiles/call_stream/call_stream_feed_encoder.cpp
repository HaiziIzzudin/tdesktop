/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_feed_encoder.h"

#include "ffmpeg/ffmpeg_utility.h"

#include <crl/crl_time.h>
#include <QtGui/QImage>

#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace CallStream {

namespace {

QSize ClampedSize(QSize size) {
	if (size.width() <= 0 || size.height() <= 0) {
		return QSize(2, 2);
	}
	int w = size.width();
	int h = size.height();
	if (w > 1920) {
		h = h * 1920 / w;
		w = 1920;
	}
	if (h > 1080) {
		w = w * 1080 / h;
		h = 1080;
	}
	if (w < 2) w = 2;
	if (h < 2) h = 2;
	w &= ~1;
	h &= ~1;
	if (w < 2) w = 2;
	if (h < 2) h = 2;
	return QSize(w, h);
}

} // namespace

FeedEncoder::FeedEncoder(const QString &folder, Packager *packager)
: _folder(folder)
, _packager(packager) {
	_thread = std::thread([this] { workerLoop(); });
}

FeedEncoder::~FeedEncoder() {
	finish();
	if (_thread.joinable()) {
		_thread.join();
	}
	closeEncoder();
}

void FeedEncoder::submitFrame(const QImage &frame) {
	VideoFrameData data;
	data.image = frame;
	data.timestampMs = crl::now();
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_queue.push(std::move(data));
	}
	_cv.notify_one();
}

void FeedEncoder::submitFrameData(const VideoFrameData &data) {
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_queue.push(data);
	}
	_cv.notify_one();
}

void FeedEncoder::submitAudioChunk(
		const QByteArray &data,
		qint64 ptsSamples,
		qint64 durationSamples,
		qint64 startTimestampMs) {
	if (_packager) {
		_packager->writeAudioPacket(
			data,
			ptsSamples,
			durationSamples,
			startTimestampMs);
	}
}

void FeedEncoder::recordMissedDeadline() {
	++_missedDeadlines;
	if (_onMissedDeadline) {
		_onMissedDeadline();
	}
}

void FeedEncoder::finish() {
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_finished = true;
	}
	_cv.notify_all();
	if (_thread.joinable() && std::this_thread::get_id() != _thread.get_id()) {
		_thread.join();
	}
}

void FeedEncoder::workerLoop() {
	while (true) {
		VideoFrameData data;
		bool hasData = false;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			if (_cv.wait_for(lock, std::chrono::milliseconds(33), [this] { return _finished || !_queue.empty(); })) {
				if (_queue.empty()) {
					if (_finished) break;
				} else {
					data = _queue.front();
					_queue.pop();
					hasData = true;
				}
			}
		}
		if (hasData) {
			// Backlog means a previous frame missed its deadline.
			{
				std::lock_guard<std::mutex> ql(_mutex);
				if (_queue.size() > 1) {
					recordMissedDeadline();
				}
			}
			const auto start = std::chrono::steady_clock::now();
			encodeOne(data);
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			if (elapsed > 33) {
				recordMissedDeadline();
			}
		} else if (!_finished && _codec) {
			// Hold the last picture through short gaps, then go black.
			const auto now = std::chrono::steady_clock::now();
			if (!_lastFrameTimeSet
				|| now - _lastFrameTime > std::chrono::milliseconds(500)) {
				if (_hasLastSource
					&& now - _lastSourceTime < std::chrono::seconds(3)) {
					auto repeat = _lastSource;
					repeat.timestampMs = 0;
					encodeOne(repeat, false);
				} else {
					VideoFrameData black;
					encodeOne(black, false);
				}
			}
		}
		if (_finished) {
			// drain remaining
			while (true) {
				VideoFrameData d;
				{
					std::lock_guard<std::mutex> lock(_mutex);
					if (_queue.empty()) break;
					d = _queue.front();
					_queue.pop();
				}
				encodeOne(d);
			}
			break;
		}
	}
}

void FeedEncoder::openEncoder(int width, int height) {
	if (_codec) return;
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!codec) return;
	_codec = avcodec_alloc_context3(codec);
	if (!_codec) return;
	auto *ctx = static_cast<AVCodecContext*>(_codec);
	ctx->width = width;
	ctx->height = height;
	ctx->time_base = AVRational{ 1, 30 };
	ctx->framerate = AVRational{ 30, 1 };
	ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	ctx->gop_size = 60;
	ctx->max_b_frames = 0;
	// High profile, level <= 4.1
	ctx->profile = AV_PROFILE_H264_HIGH;
	ctx->level = 41;
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
	av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		_codec = nullptr;
		return;
	}
	if (_packager) {
		_packager->setVideoParams(
			width,
			height,
			QByteArray(
				reinterpret_cast<const char*>(ctx->extradata),
				ctx->extradata ? ctx->extradata_size : 0));
	}
	_frame = av_frame_alloc();
	if (_frame) {
		auto *f = static_cast<AVFrame*>(_frame);
		f->format = AV_PIX_FMT_YUV420P;
		f->width = width;
		f->height = height;
		av_frame_get_buffer(f, 32);
	}
	_width = width;
	_height = height;
}

void FeedEncoder::encodeOne(const VideoFrameData &data, bool fromSource) {
	QSize srcSize;
	bool black = false;
	if (data.hasYuv420) {
		srcSize = data.yuvSize;
	} else if (!data.image.isNull()) {
		srcSize = data.image.size();
	} else {
		if (_width > 0 && _height > 0) {
			srcSize = QSize(_width, _height);
			black = true;
		} else {
			return;
		}
	}
	const QSize dstSize = ClampedSize(srcSize);
	// Apply shrink ladder (never below delivered size: only shrink when target < src).
	int maxH = _maxHeight.load();
	if (maxH <= 0) maxH = 1080;
	QSize finalSize = dstSize;
	if (maxH < finalSize.height()) {
		int h = maxH;
		int w = finalSize.width() * h / finalSize.height();
		if (w < 2) w = 2;
		if (h < 2) h = 2;
		w &= ~1;
		h &= ~1;
		if (w < 2) w = 2;
		if (h < 2) h = 2;
		finalSize = QSize(w, h);
	}
	if (_codec && (_width != finalSize.width() || _height != finalSize.height())) {
		closeEncoder();
	}
	if (!_codec) {
		openEncoder(finalSize.width(), finalSize.height());
		if (!_codec) return;
	}

	auto *ctx = static_cast<AVCodecContext*>(_codec);
	auto *frame = static_cast<AVFrame*>(_frame);
	if (!frame) return;

	const auto timestampMs = (data.timestampMs > 0)
		? data.timestampMs
		: crl::now();
	if (!_epochSet) {
		_epochMs = timestampMs;
		_epochSet = true;
	}
	const auto pts = qint64(std::llround(
		(timestampMs - _epochMs) * 30.0 / 1000.0));
	if (pts <= _lastPts) {
		return;
	}
	_lastPts = pts;

	if (fromSource) {
		_lastSource = data;
		_lastSourceTime = std::chrono::steady_clock::now();
		_hasLastSource = true;
	}

	if (av_frame_make_writable(frame) < 0) {
		return;
	}

	AVFrame *srcFrame = av_frame_alloc();
	if (!srcFrame) {
		return;
	}
	if (data.hasYuv420) {
		srcFrame->format = AV_PIX_FMT_YUV420P;
		srcFrame->width = data.yuvSize.width();
		srcFrame->height = data.yuvSize.height();
		srcFrame->data[0] = const_cast<uint8_t*>(data.y.data());
		srcFrame->data[1] = const_cast<uint8_t*>(data.u.data());
		srcFrame->data[2] = const_cast<uint8_t*>(data.v.data());
		srcFrame->linesize[0] = data.yStride;
		srcFrame->linesize[1] = data.uStride;
		srcFrame->linesize[2] = data.vStride;
	} else {
		srcFrame->format = AV_PIX_FMT_BGRA;
		srcFrame->width = data.image.width();
		srcFrame->height = data.image.height();
		srcFrame->data[0] = const_cast<uchar*>(data.image.bits());
		srcFrame->linesize[0] = data.image.bytesPerLine();
	}

	if (black) {
		if (frame->data[0]) {
			memset(frame->data[0], 16, (size_t)frame->linesize[0] * ctx->height);
		}
		if (frame->data[1]) {
			memset(frame->data[1], 128, (size_t)frame->linesize[1] * (ctx->height / 2));
		}
		if (frame->data[2]) {
			memset(frame->data[2], 128, (size_t)frame->linesize[2] * (ctx->height / 2));
		}
	} else {
		if (!_sws
			|| _swsWidth != srcFrame->width
			|| _swsHeight != srcFrame->height
			|| _swsFormat != srcFrame->format) {
			sws_freeContext(static_cast<SwsContext*>(_sws));
			_sws = sws_getContext(
				srcFrame->width, srcFrame->height,
				(AVPixelFormat)srcFrame->format,
				ctx->width, ctx->height, ctx->pix_fmt,
				SWS_BILINEAR, nullptr, nullptr, nullptr);
			_swsWidth = srcFrame->width;
			_swsHeight = srcFrame->height;
			_swsFormat = srcFrame->format;
		}
		if (auto sws = static_cast<SwsContext*>(_sws)) {
			sws_scale(
				sws,
				srcFrame->data,
				srcFrame->linesize,
				0,
				srcFrame->height,
				frame->data,
				frame->linesize);
		}
	}

	// If srcFrame planes were attached, detach to avoid free
	if (data.hasYuv420) {
		srcFrame->data[0] = nullptr;
		srcFrame->data[1] = nullptr;
		srcFrame->data[2] = nullptr;
	}
	av_frame_free(&srcFrame);

	frame->pts = pts;

	AVPacket *pkt = av_packet_alloc();
	int ret = avcodec_send_frame(ctx, frame);
	if (ret >= 0) {
		while (ret >= 0) {
			ret = avcodec_receive_packet(ctx, pkt);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
			if (ret < 0) break;
			pkt->pts = pts;
			pkt->dts = pts;
			if (_packager) {
				_packager->writeVideoPacket(
					QByteArray(
						reinterpret_cast<const char*>(pkt->data),
						pkt->size),
					timestampMs,
					(pkt->flags & AV_PKT_FLAG_KEY) != 0);
			}
			av_packet_unref(pkt);
		}
	}
	av_packet_free(&pkt);

	_lastFrameTime = std::chrono::steady_clock::now();
	_lastFrameTimeSet = true;
}

void FeedEncoder::closeEncoder() {
	if (_codec) {
		auto *ctx = static_cast<AVCodecContext*>(_codec);
		avcodec_free_context(&ctx);
		_codec = nullptr;
	}
	if (_frame) {
		auto *f = static_cast<AVFrame*>(_frame);
		av_frame_free(&f);
		_frame = nullptr;
	}
	if (_sws) {
		sws_freeContext(static_cast<SwsContext*>(_sws));
		_sws = nullptr;
	}
	_swsWidth = 0;
	_swsHeight = 0;
	_swsFormat = 0;
}

} // namespace CallStream