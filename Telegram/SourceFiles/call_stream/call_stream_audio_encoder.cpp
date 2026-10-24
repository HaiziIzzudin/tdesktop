/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_audio_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <crl/crl_time.h>

#include <algorithm>

namespace CallStream {

namespace {

constexpr auto kSampleRate = 48000;
constexpr auto kChannels = 2;
constexpr auto kBytesPerFrame = kChannels * int(sizeof(int16_t));

AVCodecContext *OpenAac() {
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!codec) {
		return nullptr;
	}
	auto context = avcodec_alloc_context3(codec);
	if (!context) {
		return nullptr;
	}
	context->bit_rate = 96000;
	context->sample_rate = kSampleRate;
	av_channel_layout_default(&context->ch_layout, kChannels);
	context->sample_fmt = AV_SAMPLE_FMT_FLTP;
	context->time_base = AVRational{ 1, kSampleRate };
	if (avcodec_open2(context, codec, nullptr) < 0) {
		avcodec_free_context(&context);
		return nullptr;
	}
	return context;
}

QByteArray ConvertToStereo(
		const QByteArray &samples,
		int sampleRate,
		int channels) {
	if (sampleRate == kSampleRate && channels == kChannels) {
		return samples;
	}
	const auto count = int(samples.size() / (sizeof(int16_t) * channels));
	if (count <= 0) {
		return {};
	}
	auto output = QByteArray(count * kBytesPerFrame, Qt::Uninitialized);
	const auto source = reinterpret_cast<const int16_t*>(samples.constData());
	const auto target = reinterpret_cast<int16_t*>(output.data());
	for (auto i = 0; i != count; ++i) {
		const auto left = source[i * channels];
		const auto right = (channels > 1)
			? source[i * channels + 1]
			: left;
		target[i * kChannels] = left;
		target[i * kChannels + 1] = right;
	}
	return output;
}

} // namespace

AudioEncoder::AudioEncoder()
: _ctx(OpenAac()) {
	auto context = static_cast<AVCodecContext*>(_ctx);
	if (!context) {
		return;
	}
	auto parameters = avcodec_parameters_alloc();
	if (parameters) {
		if (avcodec_parameters_from_context(parameters, context) >= 0) {
			_params.sampleRate = parameters->sample_rate;
			_params.channels = parameters->ch_layout.nb_channels;
			if (parameters->extradata && parameters->extradata_size > 0) {
				_params.extradata = QByteArray(
					reinterpret_cast<const char*>(parameters->extradata),
					parameters->extradata_size);
			}
		}
		avcodec_parameters_free(&parameters);
	}
	auto frame = av_frame_alloc();
	if (frame) {
		frame->format = AV_SAMPLE_FMT_FLTP;
		frame->sample_rate = kSampleRate;
		av_channel_layout_default(&frame->ch_layout, kChannels);
		frame->nb_samples = context->frame_size;
		if (av_frame_get_buffer(frame, 0) < 0) {
			av_frame_free(&frame);
		}
	}
	_frame = frame;
}

AudioEncoder::~AudioEncoder() {
	if (_ctx) {
		auto context = static_cast<AVCodecContext*>(_ctx);
		avcodec_free_context(&context);
		_ctx = nullptr;
	}
	if (_frame) {
		auto frame = static_cast<AVFrame*>(_frame);
		av_frame_free(&frame);
		_frame = nullptr;
	}
	if (_swr) {
		auto resampler = static_cast<SwrContext*>(_swr);
		swr_free(&resampler);
		_swr = nullptr;
	}
}

AudioParams AudioEncoder::params() const {
	return _params;
}

void AudioEncoder::setChunkCallback(ChunkCallback callback) {
	_callback = std::move(callback);
}

void AudioEncoder::submitPiece(const AudioPiece &piece) {
	auto context = static_cast<AVCodecContext*>(_ctx);
	auto frame = static_cast<AVFrame*>(_frame);
	if (!context || !frame || !_callback || piece.samples.isEmpty()) {
		return;
	}
	if (!_startSet) {
		_startTimestampMs = (piece.timestampMs > 0)
			? piece.timestampMs
			: crl::now();
		_startSet = true;
	}
	const auto pieceRate = (piece.sampleRate > 0)
		? piece.sampleRate
		: kSampleRate;
	const auto pieceChannels = (piece.channels > 0) ? piece.channels : 1;

	QByteArray converted;
	if (pieceRate == kSampleRate) {
		converted = ConvertToStereo(
			piece.samples,
			pieceRate,
			pieceChannels);
	} else {
		auto resampler = static_cast<SwrContext*>(_swr);
		if (!resampler
			|| _swrRate != pieceRate
			|| _swrChannels != pieceChannels) {
			swr_free(&resampler);
			auto input = AVChannelLayout{};
			auto output = AVChannelLayout{};
			av_channel_layout_default(&input, pieceChannels);
			av_channel_layout_default(&output, kChannels);
			if (swr_alloc_set_opts2(
					&resampler,
					&output,
					AV_SAMPLE_FMT_S16,
					kSampleRate,
					&input,
					AV_SAMPLE_FMT_S16,
					pieceRate,
					0,
					nullptr) < 0
				|| !resampler
				|| swr_init(resampler) < 0) {
				swr_free(&resampler);
				_swr = nullptr;
				return;
			}
			_swr = resampler;
			_swrRate = pieceRate;
			_swrChannels = pieceChannels;
		}
		const auto count = int(
			piece.samples.size() / (sizeof(int16_t) * pieceChannels));
		const auto capacity = int(av_rescale_rnd(
			swr_get_delay(resampler, pieceRate) + count,
			kSampleRate,
			pieceRate,
			AV_ROUND_UP));
		if (capacity <= 0) {
			return;
		}
		converted.resize(capacity * kBytesPerFrame);
		auto target = reinterpret_cast<uint8_t*>(converted.data());
		const auto source = reinterpret_cast<const uint8_t*>(
			piece.samples.constData());
		const auto written = swr_convert(
			resampler,
			&target,
			capacity,
			&source,
			count);
		if (written <= 0) {
			return;
		}
		converted.resize(written * kBytesPerFrame);
	}
	if (converted.isEmpty()) {
		return;
	}
	_pending.append(converted);

	const auto frameSize = int(context->frame_size);
	auto consumed = 0;
	while (_pending.size() - consumed >= frameSize * kBytesPerFrame) {
		const auto data = reinterpret_cast<const int16_t*>(
			_pending.constData() + consumed);
		encodeFrame(data, frameSize);
		consumed += frameSize * kBytesPerFrame;
	}
	if (consumed > 0) {
		_pending.remove(0, consumed);
	}
}

void AudioEncoder::encodeFrame(const int16_t *samples, int count) {
	auto context = static_cast<AVCodecContext*>(_ctx);
	auto frame = static_cast<AVFrame*>(_frame);
	if (!context || !frame) {
		return;
	}
	if (av_frame_make_writable(frame) < 0) {
		return;
	}
	frame->nb_samples = count;
	frame->pts = _pts;
	_pts += count;
	for (auto channel = 0; channel != kChannels; ++channel) {
		auto target = reinterpret_cast<float*>(frame->data[channel]);
		for (auto i = 0; i != count; ++i) {
			target[i] = samples[i * kChannels + channel] / 32768.0f;
		}
	}
	if (avcodec_send_frame(context, frame) >= 0) {
		drainPackets();
	}
}

void AudioEncoder::drainPackets() {
	auto context = static_cast<AVCodecContext*>(_ctx);
	if (!context) {
		return;
	}
	auto packet = av_packet_alloc();
	if (!packet) {
		return;
	}
	while (true) {
		const auto result = avcodec_receive_packet(context, packet);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			break;
		} else if (result < 0) {
			break;
		}
		if (_callback) {
			auto pts = packet->pts;
			if (pts == AV_NOPTS_VALUE) {
				pts = _outputPts;
			} else {
				pts += context->initial_padding;
			}
			pts = std::max<qint64>(pts, _outputPts);
			_outputPts = pts;
			_callback(
				QByteArray(
					reinterpret_cast<const char*>(packet->data),
					packet->size),
				pts,
				(packet->duration > 0) ? packet->duration : 1024,
				_startTimestampMs);
		}
		av_packet_unref(packet);
	}
	av_packet_free(&packet);
}

void AudioEncoder::finish() {
	auto context = static_cast<AVCodecContext*>(_ctx);
	if (!context) {
		return;
	}
	const auto rest = int(_pending.size() / kBytesPerFrame);
	if (rest > 0) {
		encodeFrame(
			reinterpret_cast<const int16_t*>(_pending.constData()),
			rest);
		_pending.clear();
	}
	avcodec_send_frame(context, nullptr);
	drainPackets();
}

} // namespace CallStream
