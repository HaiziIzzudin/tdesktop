/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#include "call_stream/call_stream_home.h"

#include "call_stream/call_stream_audio_encoder.h"
#include "call_stream/call_stream_feed.h"
#include "call_stream/call_stream_mixer.h"
#include "call_stream/call_stream_link_service.h"

#include "calls/calls_call.h"

#include "webrtc/webrtc_create_adm.h"
#include <tgcalls/Instance.h>
#include "calls/calls_instance.h"
#include "calls/group/calls_group_call.h"
#include "calls/group/calls_group_common.h"
#include "webrtc/webrtc_video_track.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_group_call.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "data/data_user.h"

#include "call_stream/call_stream_link_service.h"

#include <rpl/rpl.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <cstring>

namespace CallStream {

Home *Home::_instance = nullptr;

Home::Home() : QObject(nullptr) {
}

Home::~Home() = default;

void Home::Start() {
	if (_instance) return;
	_instance = new Home();
	_instance->_enabled = Core::App().settings().callStreamingEnabled();
	_instance->observe();
	LinkService::Instance()->setAnswerProvider([]() {
		return _instance ? _instance->listing() : QString(u"no call right now"_q);
	});
	LinkService::Instance()->setFileProvider([](const QString &path) {
		return _instance ? _instance->serveFile(path) : LinkServiceResponse{};
	});
	Webrtc::SetPlayoutTapCallback([=](std::vector<int16_t> samples, int sampleRate, int channels, crl::time timestampMs) {
		crl::on_main(_instance, [=]() mutable {
			if (_instance) {
				_instance->handlePlayoutTap(std::move(samples), sampleRate, channels, timestampMs);
			}
		});
	});
	tgcalls::SetCaptureAudioTapCallback([=](std::vector<int16_t> samples, int sampleRate, int channels) {
		crl::on_main(_instance, [=]() mutable {
			if (_instance) {
				_instance->handleCaptureTap(std::move(samples), sampleRate, channels);
			}
		});
	});
}

void Home::Shutdown() {
	Webrtc::SetPlayoutTapCallback(nullptr);
	tgcalls::SetCaptureAudioTapCallback(nullptr);
	delete _instance;
	_instance = nullptr;
}

Home *Home::Instance() {
	return _instance;
}

void Home::setStreamingEnabled(bool enabled) {
	_enabled = enabled;
	Core::App().settings().setCallStreamingEnabled(enabled);
	Core::App().saveSettingsDelayed();
	if (!enabled) {
		clearFeeds();
	}
	_listingChanged.fire({});
}

void Home::observe() {
	auto &calls = Core::App().calls();
	rpl::combine(
		calls.currentCallValue(),
		calls.currentGroupCallValue()
	) | rpl::on_next([=](Calls::Call *c, Calls::GroupCall *g) {
		_callLifetime = rpl::lifetime();
		if (c != _call || g != _groupCall) {
			clearFeeds();
		}
		_call = c;
		_groupCall = g;

		if (g) {
			if (auto data = g->lookupReal()) {
				data->participantUpdated() | rpl::on_next([=](const Data::GroupCall::ParticipantUpdate &) {
					rebuildFeeds();
				}, _callLifetime);
				data->participantsReloaded() | rpl::on_next([=] {
					rebuildFeeds();
				}, _callLifetime);
				data->session().changes().realtimeNameUpdates() | rpl::on_next([=](const Data::NameUpdate &u) {
					for (const auto &f : _feeds) {
						if (f.peer == u.peer) { _listingChanged.fire({}); return; }
					}
				}, _callLifetime);
			}
			g->videoStreamActiveUpdates() | rpl::on_next([=](const Calls::VideoStateToggle &) {
				rebuildFeeds();
			}, _callLifetime);
		}
		if (c) {
			c->stateValue() | rpl::on_next([=](Calls::Call::State s) {
				if (s == Calls::Call::State::Established) rebuildFeeds();
			}, _callLifetime);
			if (auto u = c->user()) {
				u->session().changes().realtimeNameUpdates() | rpl::on_next([=](const Data::NameUpdate &nu) {
					if (nu.peer == u) _listingChanged.fire({});
				}, _callLifetime);
			}
		}

		rebuildFeeds();
	}, _lifetime);
}

void Home::announceCallChanged(Calls::Call *call, Calls::GroupCall *groupCall) {
	_callLifetime = rpl::lifetime();
	_call = call;
	_groupCall = groupCall;
	rebuildFeeds();
}

void Home::clearFeeds() {
	for (auto &f : _feedInstances) {
		if (f) f->finish();
	}
	_feedInstances.clear();
	_feedTrackLifetimes.clear();
	_feedWired.clear();
	_audioEncoder.reset();
	_mixer.reset();
	_feeds.clear();
	_perFeedMaxHeight.clear();
	_missedCounts.clear();
	_globalMaxHeight = 1080;
	_lastShrinkSet = false;
	_listingChanged.fire({});
}

void Home::rebuildFeeds() {
	if (!_call && !_groupCall) {
		clearFeeds();
		return;
	}
	if (!_enabled || !Core::App().settings().callStreamingEnabled()) {
		clearFeeds();
		return;
	}

	// Collect currently available sources (desired feeds).
	std::vector<Feed> desired;
	if (_groupCall) {
		if (auto data = _groupCall->lookupReal()) {
			auto self = data->session().user();
			for (const auto &p : data->participants()) {
				if (p.peer == self) continue;
				if (!p.cameraEndpoint().empty()) {
					Feed f; f.peer = p.peer; f.slot = u"camera"_q; desired.push_back(f);
				}
				if (!p.screenEndpoint().empty()) {
					Feed f; f.peer = p.peer; f.slot = u"screen"_q; desired.push_back(f);
				}
			}
			bool hasRtmp = _groupCall->rtmp();
			if (!hasRtmp) {
				for (const auto &kv : _groupCall->activeVideoTracks()) {
					if (kv.first.id == Data::RtmpEndpointId()) { hasRtmp = true; break; }
				}
			}
			if (hasRtmp) {
				if (auto data2 = _groupCall->lookupReal()) {
					Feed f; f.peer = data2->peer(); f.slot = u"rtmp"_q; desired.push_back(f);
				}
			}
		}
	} else if (_call) {
		if (auto u = _call->user()) {
			Feed f; f.peer = u; f.slot = u"camera"_q; desired.push_back(f);
		}
	}

	// Ensure shared audio encoder/mixer exists if we have or will have feeds.
	if (_audioEncoder == nullptr && (!_feeds.empty() || !desired.empty())) {
		_audioEncoder = std::make_shared<AudioEncoder>();
		_audioEncoder->setChunkCallback([this](const QByteArray &data, qint64 pts) {
			for (auto &f : _feedInstances) {
				if (f) f->submitAudioChunk(data, pts);
			}
		});
		_mixer = std::make_unique<Mixer>([this](const AudioPiece &piece) {
			if (_audioEncoder) {
				_audioEncoder->submitPiece(piece);
			}
		});
	}

	const auto tempRoot = QDir::tempPath() + u"/tdesktop-callstream"_q;
	QDir().mkpath(tempRoot);

	// Add any newly available feeds without disturbing existing ones.
	for (const auto &d : desired) {
		bool exists = false;
		for (const auto &e : _feeds) {
			if (e.peer == d.peer && e.slot == d.slot) { exists = true; break; }
		}
		if (exists) continue;
		_feeds.push_back(d);
		const int newIndex = static_cast<int>(_feedInstances.size());
		if (_audioEncoder) {
			const auto folder = tempRoot + u"/feed-"_q + QString::number(newIndex);
			QDir().mkpath(folder);
			auto instance = std::make_unique<StreamFeed>(newIndex, folder, _audioEncoder);
			const int captured = newIndex;
			instance->setOnMissedDeadline([this, captured]() {
				crl::on_main(this, [this, captured]() {
					if (captured >= 0 && captured < static_cast<int>(_feeds.size())) {
						reportMissedDeadline(captured);
					}
				});
			});
			_feedInstances.push_back(std::move(instance));
		} else {
			_feedInstances.push_back(nullptr);
		}
		_feedTrackLifetimes.push_back(rpl::lifetime());
		_feedWired.push_back(false);
		_perFeedMaxHeight.push_back(1080);
		_missedCounts.push_back(0);
	}

	// Keep vectors aligned (in case feeds exist but instances not).
	while (_feedInstances.size() < _feeds.size()) {
		_feedInstances.push_back(nullptr);
	}
	while (_feedTrackLifetimes.size() < _feeds.size()) {
		_feedTrackLifetimes.push_back(rpl::lifetime());
	}
	while (_feedWired.size() < _feeds.size()) {
		_feedWired.push_back(false);
	}
	while (_perFeedMaxHeight.size() < _feeds.size()) {
		_perFeedMaxHeight.push_back(1080);
	}
	while (_missedCounts.size() < _feeds.size()) {
		_missedCounts.push_back(0);
	}

	wireVideoTracks();
	requestTopQualityForAll();
	applyFeedMaxHeights();

	_listingChanged.fire({});
}

void Home::submitVideoFrame(int feedIndex, const QImage &frame, const QSize &size) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_feedInstances.size())) {
		return;
	}
	if (_feedInstances[feedIndex]) {
		_feedInstances[feedIndex]->submitVideoFrame(frame, size);
	}
}

void Home::submitFrameData(int feedIndex, const VideoFrameData &data) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_feedInstances.size())) {
		return;
	}
	if (_feedInstances[feedIndex]) {
		_feedInstances[feedIndex]->submitFrameData(data);
	}
}

void Home::submitAudioPiece(const AudioPiece &piece) {
	if (_audioEncoder) {
		_audioEncoder->submitPiece(piece);
	}
}

void Home::handlePlayoutTap(std::vector<int16_t> samples, int sampleRate, int channels, qint64 timestampMs) {
	if (_mixer) {
		_mixer->pushRemote(samples, sampleRate, channels, timestampMs);
	}
}

void Home::handleCaptureTap(std::vector<int16_t> samples, int sampleRate, int channels) {
	if (_mixer) {
		_mixer->pushMic(samples, sampleRate, channels);
	}
}

void Home::wireVideoTracks() {
	if (_groupCall) {
		if (auto data = _groupCall->lookupReal()) {
			const auto &tracks = _groupCall->activeVideoTracks();
			for (size_t i = 0; i < _feeds.size(); ++i) {
				if (i >= _feedWired.size()) _feedWired.push_back(false);
				if (_feedWired[i]) continue;
				const auto &feed = _feeds[i];
				std::string endpointId;
				for (const auto &p : data->participants()) {
					if (p.peer != feed.peer) continue;
					if (feed.slot == u"camera"_q) endpointId = p.cameraEndpoint();
					else if (feed.slot == u"screen"_q) endpointId = p.screenEndpoint();
					break;
				}
				if (feed.slot == u"rtmp"_q) {
					endpointId = Data::RtmpEndpointId();
				}
				Webrtc::VideoTrack *track = nullptr;
				Calls::VideoEndpoint qualityEndpoint;
				bool haveQualityEndpoint = false;
				for (const auto &kv : tracks) {
					if (kv.first.id == endpointId) {
						track = Calls::GroupCall::TrackPointer(kv.second).get();
						qualityEndpoint = kv.first;
						haveQualityEndpoint = true;
						break;
					}
				}
				if (!track) {
					_feedWired[i] = false;
					continue;
				}
				// Override tile-driven requests: always ask top tier.
				if (haveQualityEndpoint && _groupCall) {
					_groupCall->requestVideoQuality(qualityEndpoint, Calls::Group::VideoQuality::Full);
				}
				const int idx = static_cast<int>(i);
				_feedTrackLifetimes[i] = rpl::lifetime();
				_feedWired[i] = true;
				track->renderNextFrame() | rpl::on_next([this, track, idx]() {
					auto info = track->frameWithInfo(false);
					VideoFrameData frameData;
					if (info.yuv420) {
						frameData.hasYuv420 = true;
						frameData.yuvSize = info.yuv420->size;
						frameData.chromaSize = info.yuv420->chromaSize;
						frameData.yStride = info.yuv420->y.stride;
						frameData.uStride = info.yuv420->u.stride;
						frameData.vStride = info.yuv420->v.stride;
						frameData.y.resize((size_t)frameData.yStride * frameData.yuvSize.height());
						frameData.u.resize((size_t)frameData.uStride * frameData.chromaSize.height());
						frameData.v.resize((size_t)frameData.vStride * frameData.chromaSize.height());
						if (info.yuv420->y.data) std::memcpy(frameData.y.data(), info.yuv420->y.data, frameData.y.size());
						if (info.yuv420->u.data) std::memcpy(frameData.u.data(), info.yuv420->u.data, frameData.u.size());
						if (info.yuv420->v.data) std::memcpy(frameData.v.data(), info.yuv420->v.data, frameData.v.size());
					} else {
						frameData.image = info.original;
					}
					frameData.rotation = info.rotation;
					submitFrameData(idx, frameData);
				}, _feedTrackLifetimes[i]);
			}
		}
	} else if (_call) {
		if (!_feeds.empty() && !_feedInstances.empty()) {
			if (!_feedWired.empty() && _feedWired[0]) return;
			auto *track = _call->videoIncoming().get();
			_feedTrackLifetimes[0] = rpl::lifetime();
			if (_feedWired.empty()) _feedWired.push_back(true);
			else _feedWired[0] = true;
			const int idx = 0;
			track->renderNextFrame() | rpl::on_next([this, track, idx]() {
				auto info = track->frameWithInfo(false);
				VideoFrameData frameData;
				if (info.yuv420) {
					frameData.hasYuv420 = true;
					frameData.yuvSize = info.yuv420->size;
					frameData.chromaSize = info.yuv420->chromaSize;
					frameData.yStride = info.yuv420->y.stride;
					frameData.uStride = info.yuv420->u.stride;
					frameData.vStride = info.yuv420->v.stride;
					frameData.y.resize((size_t)frameData.yStride * frameData.yuvSize.height());
					frameData.u.resize((size_t)frameData.uStride * frameData.chromaSize.height());
					frameData.v.resize((size_t)frameData.vStride * frameData.chromaSize.height());
					if (info.yuv420->y.data) std::memcpy(frameData.y.data(), info.yuv420->y.data, frameData.y.size());
					if (info.yuv420->u.data) std::memcpy(frameData.u.data(), info.yuv420->u.data, frameData.u.size());
					if (info.yuv420->v.data) std::memcpy(frameData.v.data(), info.yuv420->v.data, frameData.v.size());
				} else {
					frameData.image = info.original;
				}
				frameData.rotation = info.rotation;
				submitFrameData(idx, frameData);
			}, _feedTrackLifetimes[0]);
		}
	}
}

void Home::requestTopQualityForAll() {
	if (!_groupCall) return;
	for (const auto &kv : _groupCall->activeVideoTracks()) {
		_groupCall->requestVideoQuality(kv.first, Calls::Group::VideoQuality::Full);
	}
}

void Home::reportMissedDeadline(int feedIndex) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_feeds.size())) return;
	if (feedIndex >= static_cast<int>(_missedCounts.size())) return;
	_missedCounts[feedIndex]++;
	// Single-feed drop: only this feed misses.
	int feedsMissing = 0;
	for (int i = 0; i < static_cast<int>(_missedCounts.size()); ++i) {
		if (_missedCounts[i] > 0) ++feedsMissing;
	}
	if (feedsMissing == 1 && _missedCounts[feedIndex] >= kSingleThreshold) {
		if (feedIndex < static_cast<int>(_perFeedMaxHeight.size())) {
			_perFeedMaxHeight[feedIndex] = std::min(_perFeedMaxHeight[feedIndex], 720);
		}
		applyFeedMaxHeights();
		// reset this feed's count
		_missedCounts[feedIndex] = 0;
	}
	// Aggregate pressure: misses across feeds.
	int total = 0;
	for (int c : _missedCounts) total += c;
	if (total >= kAggregateThreshold && feedsMissing >= 2) {
		const auto now = std::chrono::steady_clock::now();
		bool allow = !_lastShrinkSet;
		if (_lastShrinkSet) {
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastShrinkTime).count();
			if (ms >= kShrinkIntervalMs) allow = true;
		}
		if (allow) {
			// step down global
			if (_globalMaxHeight > 360) {
				if (_globalMaxHeight >= 1080) _globalMaxHeight = 720;
				else if (_globalMaxHeight >= 720) _globalMaxHeight = 480;
				else if (_globalMaxHeight >= 480) _globalMaxHeight = 360;
			}
			_lastShrinkTime = now;
			_lastShrinkSet = true;
			// reset counts
			for (int &c : _missedCounts) c = 0;
			applyFeedMaxHeights();
		}
	}
}

void Home::applyFeedMaxHeights() {
	for (size_t i = 0; i < _feedInstances.size(); ++i) {
		if (!_feedInstances[i]) continue;
		int effective = std::min(_globalMaxHeight, (i < _perFeedMaxHeight.size()) ? _perFeedMaxHeight[i] : 1080);
		_feedInstances[i]->setMaxHeight(effective);
	}
}

void Home::setFeedMaxHeight(int feedIndex, int height) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_perFeedMaxHeight.size())) return;
	_perFeedMaxHeight[feedIndex] = height;
	applyFeedMaxHeights();
}

int Home::feedMaxHeight(int feedIndex) const {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_perFeedMaxHeight.size())) return _globalMaxHeight;
	return std::min(_globalMaxHeight, _perFeedMaxHeight[feedIndex]);
}

int Home::missedDeadlineCount() const {
	int total = 0;
	for (int c : _missedCounts) total += c;
	// include encoder counts too
	for (const auto &f : _feedInstances) {
		if (f) total += f->missedDeadlines();
	}
	return total;
}

int Home::missedDeadlineCount(int feedIndex) const {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_missedCounts.size())) return 0;
	int v = _missedCounts[feedIndex];
	if (feedIndex < static_cast<int>(_feedInstances.size()) && _feedInstances[feedIndex]) {
		v += _feedInstances[feedIndex]->missedDeadlines();
	}
	return v;
}

LinkServiceResponse Home::serveFile(const QString &path) const {
	LinkServiceResponse response;
	// Path like /<id>/<slot>.m3u8 or /<id>/<filename>.ts
	const auto trimmed = path.startsWith('/') ? path.mid(1) : path;
	const auto parts = trimmed.split('/');
	if (parts.size() < 2) {
		return response;
	}
	// Find feed by link prefix.
	const auto feedKey = parts[0] + u"/"_q + parts[1].section('.', 0, 0);
	QString folder;
	const auto fileName = parts.last();
	for (size_t i = 0; i < _feeds.size(); ++i) {
		if (_feeds[i].link().contains(parts[0]) && fileName.startsWith(_feeds[i].slot)) {
			if (i < _feedInstances.size() && _feedInstances[i]) {
				folder = _feedInstances[i]->folder();
			}
			break;
		}
	}
	if (folder.isEmpty()) {
		// Fallback: use index 0 folder if only one feed.
		if (!_feedInstances.empty() && _feedInstances[0]) {
			folder = _feedInstances[0]->folder();
		}
	}
	if (folder.isEmpty()) {
		return response;
	}
	QFile file(folder + u"/"_q + fileName);
	if (!file.open(QIODevice::ReadOnly)) {
		return response;
	}
	response.body = file.readAll();
	response.isFile = true;
	if (fileName.endsWith(u".m3u8"_q)) {
		response.contentType = "application/vnd.apple.mpegurl";
	} else if (fileName.endsWith(u".ts"_q)) {
		response.contentType = "video/MP2T";
	} else {
		response.contentType = "application/octet-stream";
	}
	return response;
}

QString Home::listing() const {
	if (!_enabled) return u"streaming is off"_q;
	if (!_call && !_groupCall) return u"no call right now"_q;
	if (_feeds.empty()) return u"no feeds right now"_q;
	QStringList lines;
	for (const auto &f : _feeds) {
		lines << (f.name() + u" — "_q + f.link());
	}
	return lines.join(u"\n"_q);
}

QString Feed::name() const {
	return peer ? peer->name() : QString();
}

QString Feed::link() const {
	const auto id = peer ? QString::number(peer->id.value) : QString(u"0"_q);
	return u"http://127.0.0.1:8479/"_q + id + u"/"_q + slot + u".m3u8"_q;
}

} // namespace CallStream