/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include "call_stream/call_stream_types.h"

#include <QtCore/QByteArray>
#include <functional>
#include <vector>

namespace CallStream {

class Mixer {
public:
	using SubmitCallback = std::function<void(const AudioPiece &piece)>;

	explicit Mixer(SubmitCallback submit);
	~Mixer() = default;

	void pushRemote(const std::vector<int16_t> &samples, int sampleRate, int channels, qint64 timestampMs);
	void pushMic(const std::vector<int16_t> &samples, int sampleRate, int channels);

private:
	void tryMix();
	void mixMic(AudioPiece &out, const AudioPiece &mic);
	void trimQueue(std::vector<AudioPiece> &queue);

	SubmitCallback _submit;
	std::vector<AudioPiece> _remoteQueue;
	std::vector<AudioPiece> _micQueue;
};

} // namespace CallStream