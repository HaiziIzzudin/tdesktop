/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "test/test_runner.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace Test {

// Lists the muxer names offered by the FFmpeg library linked into this build.
[[nodiscard]] QStringList ListFfmpegMuxers();

// True when the linked FFmpeg offers a muxer with this (short) name.
[[nodiscard]] bool FfmpegHasMuxer(const QString &name);

// Logs every linked muxer name, then fails (naming it) if either the
// "hls" or the "mpegts" muxer is missing.
void CheckFfmpegMuxers();

// Aliases kept for callers that name the check differently.
void CheckFfmpegMuxerProbe();
void CheckLinkedFfmpegMuxers();
void CheckFfmpegMuxersAvailable();

// Stage entry point for the task-test harness.
void AppendFfmpegMuxersSelfTest(not_null<Runner *> runner);

} // namespace Test
