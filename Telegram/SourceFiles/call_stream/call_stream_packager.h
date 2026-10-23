/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include <QtCore/QString>

namespace CallStream {

class Packager {
public:
	explicit Packager(const QString &folder);
	~Packager();

	void setEnded();
	[[nodiscard]] const QString &folder() const { return _folder; }

private:
	QString _folder;
	bool _ended = false;
};

} // namespace CallStream