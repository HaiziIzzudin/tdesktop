/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_mixer.h"

#include <crl/crl_time.h>

#include <cstring>

namespace CallStream {

Mixer::Mixer(SubmitCallback submit) : _submit(std::move(submit)) {
}

void Mixer::pushRemote(const std::vector<int16_t> &samples, int sampleRate, int channels, qint64 timestampMs) {
	AudioPiece piece;
	piece.sampleRate = sampleRate;
	piece.channels = channels;
	piece.timestampMs = timestampMs;
	piece.samples.resize(samples.size() * sizeof(int16_t));
	std::memcpy(piece.samples.data(), samples.data(), piece.samples.size());
	_remoteQueue.push_back(std::move(piece));
	tryMix();
}

void Mixer::pushMic(const std::vector<int16_t> &samples, int sampleRate, int channels) {
	AudioPiece piece;
	piece.sampleRate = sampleRate;
	piece.channels = channels;
	piece.timestampMs = crl::now();
	piece.samples.resize(samples.size() * sizeof(int16_t));
	std::memcpy(piece.samples.data(), samples.data(), piece.samples.size());
	_micQueue.push_back(std::move(piece));
	tryMix();
}

void Mixer::tryMix() {
	while (!_remoteQueue.empty() && !_micQueue.empty()) {
		auto remote = _remoteQueue.front();
		_remoteQueue.erase(_remoteQueue.begin());
		auto mic = _micQueue.front();
		_micQueue.erase(_micQueue.begin());

		// Upmix mic to stereo if needed and add to remote.
		const int frames = remote.samples.size() / (remote.channels * sizeof(int16_t));
		QByteArray mixed = remote.samples;
		auto *mixedPtr = reinterpret_cast<int16_t *>(mixed.data());
		const auto *micPtr = reinterpret_cast<const int16_t *>(mic.samples.data());

		for (int i = 0; i < frames; ++i) {
			int16_t micSample = 0;
			if (i < static_cast<int>(mic.samples.size() / (mic.channels * sizeof(int16_t)))) {
				if (mic.channels == 1) {
					micSample = micPtr[i];
				} else if (mic.channels >= 2) {
					micSample = micPtr[i * mic.channels];
				}
			}
			for (int c = 0; c < remote.channels; ++c) {
				int idx = i * remote.channels + c;
				int v = mixedPtr[idx] + micSample;
				if (v > 32767) v = 32767;
				if (v < -32768) v = -32768;
				mixedPtr[idx] = static_cast<int16_t>(v);
			}
		}

		AudioPiece out;
		out.samples = mixed;
		out.timestampMs = remote.timestampMs;
		out.sampleRate = remote.sampleRate;
		out.channels = remote.channels;
		if (_submit) {
			_submit(out);
		}
	}
}

} // namespace CallStream