/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#ifdef _DEBUG

#include "test/test_ffmpeg_muxers.h"

#include "test/test_log.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace Test {
namespace {

[[nodiscard]] QStringList CollectMuxers() {
	auto result = QStringList();
	auto opaque = (void *)nullptr;
	const AVOutputFormat *format = nullptr;
	while ((format = av_muxer_iterate(&opaque))) {
		if (format->name) {
			result.push_back(QString::fromUtf8(format->name));
		}
	}
	return result;
}

} // namespace

QStringList ListFfmpegMuxers() {
	return CollectMuxers();
}

bool FfmpegHasMuxer(const QString &name) {
	return ListFfmpegMuxers().contains(name);
}

void CheckFfmpegMuxers() {
	const auto muxers = ListFfmpegMuxers();
	Note(u"FFMPEG_MUXERS: "_q + muxers.join(u", "_q));
	const auto required = QStringList{ u"hls"_q, u"mpegts"_q };
	for (const auto &name : required) {
		const auto found = muxers.contains(name);
		Check(
			found,
			u"ffmpeg muxer available: " + name,
			found ? QString() : u"missing ffmpeg muxer: " + name);
	}
}

void CheckFfmpegMuxerProbe() {
	CheckFfmpegMuxers();
}

void CheckLinkedFfmpegMuxers() {
	CheckFfmpegMuxers();
}

void CheckFfmpegMuxersAvailable() {
	CheckFfmpegMuxers();
}

void AppendFfmpegMuxersSelfTest(not_null<Runner *> runner) {
	runner->add({
		.name = u"ffmpeg_muxers"_q,
		.then = CheckFfmpegMuxers,
		.timeout = kDefaultStageTimeout,
	});
}

} // namespace Test

#endif // _DEBUG
