/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_audio_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <cstring>

namespace CallStream {

namespace {
AVCodecContext *OpenAac() {
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!codec) return nullptr;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) return nullptr;
	ctx->bit_rate = 96000;
	ctx->sample_rate = 48000;
	av_channel_layout_default(&ctx->ch_layout, 2);
	ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	ctx->time_base = AVRational{ 1, 48000 };
	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return nullptr;
	}
	return ctx;
}
} // namespace

AudioEncoder::AudioEncoder() : _ctx(OpenAac()), _pts(0) {}

AudioEncoder::~AudioEncoder() {
	if (_ctx) {
		auto *c = static_cast<AVCodecContext*>(_ctx);
		avcodec_free_context(&c);
		_ctx = nullptr;
	}
}

void AudioEncoder::setChunkCallback(ChunkCallback callback) {
	_callback = std::move(callback);
}

void AudioEncoder::submitPiece(const AudioPiece &piece) {
	if (!_ctx || !_callback) return;
	auto *ctx = static_cast<AVCodecContext*>(_ctx);
	const int channels = piece.channels > 0 ? piece.channels : 2;
	const int sampleRate = piece.sampleRate > 0 ? piece.sampleRate : 48000;
	const int samplesPerFrame = ctx->frame_size > 0 ? ctx->frame_size : 1024;
	// Convert int16 PCM to float planar.
	const int16_t *pcm = (const int16_t*)piece.samples.data();
	const int totalSamples = piece.samples.size() / (sizeof(int16_t) * channels);
	int offset = 0;
	while (offset < totalSamples) {
		int n = std::min(samplesPerFrame, totalSamples - offset);
		AVFrame *frame = av_frame_alloc();
		frame->nb_samples = n;
		frame->format = AV_SAMPLE_FMT_FLTP;
		av_channel_layout_default(&frame->ch_layout, 2);
		frame->sample_rate = sampleRate;
		av_frame_get_buffer(frame, 0);
		for (int i = 0; i < n; ++i) {
			for (int ch = 0; ch < channels && ch < 2; ++ch) {
				float v = pcm[(offset + i) * channels + ch] / 32768.0f;
				((float*)frame->data[ch])[i] = v;
			}
		}
		frame->pts = _pts;
		_pts += n;
		AVPacket *pkt = av_packet_alloc();
		int ret = avcodec_send_frame(ctx, frame);
		if (ret >= 0) {
			while (ret >= 0) {
				ret = avcodec_receive_packet(ctx, pkt);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
				if (ret < 0) break;
				qint64 ptsMs = (pkt->pts * 1000LL) / sampleRate;
				_callback(QByteArray((const char*)pkt->data, pkt->size), ptsMs);
				av_packet_unref(pkt);
			}
		}
		av_frame_free(&frame);
		av_packet_free(&pkt);
		offset += n;
	}
}

void AudioEncoder::finish() {
	if (!_ctx) return;
	auto *ctx = static_cast<AVCodecContext*>(_ctx);
	avcodec_send_frame(ctx, nullptr);
	AVPacket *pkt = av_packet_alloc();
	int ret = 0;
	while (ret >= 0) {
		ret = avcodec_receive_packet(ctx, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
		if (ret < 0) break;
		qint64 ptsMs = (pkt->pts * 1000LL) / 48000;
		if (_callback) _callback(QByteArray((const char*)pkt->data, pkt->size), ptsMs);
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
}

} // namespace CallStream