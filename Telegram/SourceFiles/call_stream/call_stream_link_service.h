/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QObject>
#include <QtCore/QByteArray>
#include <QtNetwork/QTcpServer>

#include <functional>
#include <memory>

namespace CallStream {

struct LinkServiceResponse {
	QByteArray body;
	QByteArray contentType;
	bool isFile = false;
};

class LinkService final : public QObject {
	Q_OBJECT
public:
	using AnswerProvider = std::function<QString()>;
	using FileProvider = std::function<LinkServiceResponse(const QString &path)>;

	static void Start();
	static void Shutdown();
	[[nodiscard]] static LinkService *Instance();

	void setAnswerProvider(AnswerProvider provider);
	void setFileProvider(FileProvider provider);

	~LinkService() override;

private:
	LinkService();

	void onNewConnection();
	void handleConnection(QTcpSocket *socket);

	QTcpServer _server;
	AnswerProvider _provider;
	FileProvider _fileProvider;
};

} // namespace CallStream