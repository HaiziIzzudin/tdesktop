/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_mixer.h"

#include <crl/crl_time.h>

#include <cstring>

namespace CallStream {

namespace {

constexpr auto kMaxQueuedPieces = size_t(200);

AudioPiece ToPiece(
		const std::vector<int16_t> &samples,
		int sampleRate,
		int channels,
		qint64 timestampMs) {
	AudioPiece piece;
	piece.sampleRate = sampleRate;
	piece.channels = channels;
	piece.timestampMs = timestampMs;
	piece.samples.resize(samples.size() * sizeof(int16_t));
	std::memcpy(piece.samples.data(), samples.data(), piece.samples.size());
	return piece;
}

} // namespace

Mixer::Mixer(SubmitCallback submit) : _submit(std::move(submit)) {
}

void Mixer::pushRemote(
		const std::vector<int16_t> &samples,
		int sampleRate,
		int channels,
		qint64 timestampMs) {
	_remoteQueue.push_back(ToPiece(
		samples,
		sampleRate,
		channels,
		timestampMs));
	trimQueue(_remoteQueue);
	tryMix();
}

void Mixer::pushMic(
		const std::vector<int16_t> &samples,
		int sampleRate,
		int channels) {
	_micQueue.push_back(ToPiece(
		samples,
		sampleRate,
		channels,
		crl::now()));
	trimQueue(_micQueue);
	tryMix();
}

void Mixer::trimQueue(std::vector<AudioPiece> &queue) {
	while (queue.size() > kMaxQueuedPieces) {
		queue.erase(queue.begin());
	}
}

void Mixer::mixMic(AudioPiece &out, const AudioPiece &mic) {
	const auto frames = int(
		out.samples.size() / (out.channels * sizeof(int16_t)));
	const auto micFrames = int(
		mic.samples.size() / (mic.channels * sizeof(int16_t)));
	auto mixed = reinterpret_cast<int16_t*>(out.samples.data());
	const auto micSamples = reinterpret_cast<const int16_t*>(
		mic.samples.constData());
	for (auto i = 0; i != frames; ++i) {
		auto micSample = int16_t(0);
		if (i < micFrames) {
			micSample = (mic.channels == 1)
				? micSamples[i]
				: micSamples[i * mic.channels];
		}
		for (auto c = 0; c != out.channels; ++c) {
			const auto index = i * out.channels + c;
			auto value = int(mixed[index]) + micSample;
			if (value > 32767) {
				value = 32767;
			} else if (value < -32768) {
				value = -32768;
			}
			mixed[index] = static_cast<int16_t>(value);
		}
	}
}

void Mixer::tryMix() {
	// Either side may be missing (muted call, no capture device).
	while (!_remoteQueue.empty() || !_micQueue.empty()) {
		auto out = AudioPiece();
		if (!_remoteQueue.empty()) {
			out = std::move(_remoteQueue.front());
			_remoteQueue.erase(_remoteQueue.begin());
			if (!_micQueue.empty()) {
				auto mic = std::move(_micQueue.front());
				_micQueue.erase(_micQueue.begin());
				mixMic(out, mic);
			}
		} else {
			out = std::move(_micQueue.front());
			_micQueue.erase(_micQueue.begin());
		}
		if (_submit) {
			_submit(out);
		}
	}
}

} // namespace CallStream
