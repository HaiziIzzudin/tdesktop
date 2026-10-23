/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_packager.h"

namespace CallStream {

Packager::Packager(const QString &folder) : _folder(folder) {
}

Packager::~Packager() = default;

void Packager::setEnded() {
	_ended = true;
}

} // namespace CallStream