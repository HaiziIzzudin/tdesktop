/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "call_stream/call_stream_link_service.h"

#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QHostAddress>

#include <crl/crl.h>

namespace CallStream {

namespace {
LinkService *InstancePtr = nullptr;
} // namespace

LinkService::LinkService() : QObject(nullptr) {
	_provider = []() {
		return u"no call right now"_q;
	};
	connect(&_server, &QTcpServer::newConnection, this, &LinkService::onNewConnection);
}

LinkService::~LinkService() {
	_server.close();
}

void LinkService::Start() {
	if (InstancePtr) {
		return;
	}
	auto *service = new LinkService();
	InstancePtr = service;
	const bool ok = service->_server.listen(QHostAddress::LocalHost, 8479);
	if (!ok) {
		qWarning("CallStream::LinkService: failed to listen on 127.0.0.1:8479");
	}
}

void LinkService::Shutdown() {
	if (!InstancePtr) {
		return;
	}
	delete InstancePtr;
	InstancePtr = nullptr;
}

LinkService *LinkService::Instance() {
	return InstancePtr;
}

void LinkService::setAnswerProvider(AnswerProvider provider) {
	if (provider) {
		_provider = std::move(provider);
	} else {
		_provider = []() {
			return u"no call right now"_q;
		};
	}
}

void LinkService::setFileProvider(FileProvider provider) {
	_fileProvider = std::move(provider);
}

void LinkService::onNewConnection() {
	while (auto *socket = _server.nextPendingConnection()) {
		connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
		handleConnection(socket);
	}
}

void LinkService::handleConnection(QTcpSocket *socket) {
	const QPointer<QTcpSocket> guard(socket);
	connect(socket, &QIODevice::readyRead, this, [this, guard]() {
		auto *socket = guard.data();
		if (!socket) {
			return;
		}
		if (socket->property("_handled").toBool()) {
			return;
		}
		const auto available = socket->bytesAvailable();
		const auto peeked = socket->peek(available > 0 ? available : 1);
		if (!peeked.contains("\r\n\r\n")) {
			return;
		}
		socket->setProperty("_handled", true);

		const auto requestLine = socket->readLine().simplified();
		const auto parts = requestLine.split(' ');
		// Consume remaining headers.
		while (true) {
			const auto line = socket->readLine();
			if (line == "\r\n" || line.isEmpty()) {
				break;
			}
		}

		const auto send = [&](int status, const QByteArray &statusText,
				const QByteArray &body, const QByteArray &contentType,
				const QByteArray &extraHeaders = QByteArray()) {
			socket->write("HTTP/1.1 " + QByteArray::number(status) + " " + statusText + "\r\n");
			socket->write("Content-Type: " + contentType + "\r\n");
			socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
			socket->write("Connection: close\r\n");
			socket->write("Cache-Control: no-store, no-cache\r\n");
			socket->write("Pragma: no-cache\r\n");
			if (!extraHeaders.isEmpty()) {
				socket->write(extraHeaders);
			}
			socket->write("\r\n");
			if (!body.isEmpty()) {
				socket->write(body);
			}
			socket->disconnectFromHost();
		};

		if (parts.size() < 2) {
			send(400, "Bad Request", QByteArray(), "text/plain; charset=utf-8");
			return;
		}
		const auto method = parts[0];
		const auto path = QString::fromUtf8(parts[1]);
		if (method != "GET") {
			send(405, "Method Not Allowed", QByteArray(), "text/plain; charset=utf-8", "Allow: GET\r\n");
			return;
		}

		QByteArray body;
		QByteArray contentType = "text/plain; charset=utf-8";
		if (_fileProvider) {
			const auto resp = _fileProvider(path);
			if (resp.isFile) {
				body = resp.body;
				if (!resp.contentType.isEmpty()) {
					contentType = resp.contentType;
				}
			} else if (path.lastIndexOf('.') > path.lastIndexOf('/')) {
				send(404, "Not Found", QByteArray(), "text/plain; charset=utf-8");
				return;
			} else if (_provider) {
				body = _provider().toUtf8();
			}
		} else if (_provider) {
			body = _provider().toUtf8();
		}
		if (body.isEmpty() && !_provider && !_fileProvider) {
			send(404, "Not Found", QByteArray(), "text/plain; charset=utf-8");
			return;
		}
		send(200, "OK", body, contentType);
	});
}

} // namespace CallStream