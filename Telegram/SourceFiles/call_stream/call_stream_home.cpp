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

#include <rpl/rpl.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <chrono>
#include <cstring>

namespace CallStream {

namespace {

std::string FeedEndpoint(Data::GroupCall *data, const Feed &feed) {
	if (feed.slot == u"rtmp"_q) {
		return Data::RtmpEndpointId();
	}
	for (const auto &p : data->participants()) {
		if (p.peer != feed.peer) {
			continue;
		}
		return (feed.slot == u"screen"_q)
			? p.screenEndpoint()
			: p.cameraEndpoint();
	}
	return std::string();
}

VideoFrameData FrameDataFrom(Webrtc::VideoTrack *track) {
	const auto info = track->frameWithInfo(false);
	auto result = VideoFrameData();
	result.timestampMs = crl::now();
	if (info.yuv420) {
		result.hasYuv420 = true;
		result.yuvSize = info.yuv420->size;
		result.chromaSize = info.yuv420->chromaSize;
		result.yStride = info.yuv420->y.stride;
		result.uStride = info.yuv420->u.stride;
		result.vStride = info.yuv420->v.stride;
		result.y.resize(size_t(result.yStride) * result.yuvSize.height());
		result.u.resize(size_t(result.uStride) * result.chromaSize.height());
		result.v.resize(size_t(result.vStride) * result.chromaSize.height());
		if (info.yuv420->y.data) {
			std::memcpy(result.y.data(), info.yuv420->y.data, result.y.size());
		}
		if (info.yuv420->u.data) {
			std::memcpy(result.u.data(), info.yuv420->u.data, result.u.size());
		}
		if (info.yuv420->v.data) {
			std::memcpy(result.v.data(), info.yuv420->v.data, result.v.size());
		}
	} else {
		result.image = info.original;
	}
	result.rotation = info.rotation;
	return result;
}

} // namespace

Home *Home::_instance = nullptr;

Home::Home() : QObject(nullptr) {
}

Home::~Home() {
	dropSlots();
}

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
					for (const auto &slot : _slots) {
						if (isLive(*slot) && slot->feed.peer == u.peer) {
							_listingChanged.fire({});
							return;
						}
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
	if (_groupCall) {
		_groupCall->clearForcedVideoQualities();
	}
	if (_audioEncoder) {
		_audioEncoder->finish();
	}
	dropSlots();
	_audioEncoder.reset();
	_mixer.reset();
	_globalMaxHeight = 1080;
	_lastShrinkSet = false;
	_lastMissTime = {};
	_lastRecoveryTime = {};
	_listingChanged.fire({});
}

void Home::dropSlots() {
	for (const auto &slot : _slots) {
		slot->lifetime = rpl::lifetime();
		if (slot->instance) {
			slot->instance->finish();
			slot->instance.reset();
		}
		if (!slot->folder.isEmpty()) {
			QDir(slot->folder).removeRecursively();
		}
	}
	_slots.clear();
}

bool Home::isLive(const Slot &slot) const {
	return (slot.instance != nullptr) && !slot.ended;
}

bool Home::sourceGone(const Feed &feed) const {
	if (!feed.peer) {
		return true;
	} else if (!_groupCall) {
		return false;
	}
	const auto data = _groupCall->lookupReal();
	if (!data) {
		return false;
	} else if (feed.slot == u"rtmp"_q) {
		return true;
	}
	const auto participant = data->participantByPeer(feed.peer);
	if (!participant) {
		// A partially loaded list says nothing about a missing participant.
		return data->participantsLoaded();
	}
	return (feed.slot == u"screen"_q)
		? participant->screenEndpoint().empty() || participant->screenPaused()
		: participant->cameraEndpoint().empty() || participant->cameraPaused();
}

int Home::findSlot(const Feed &feed) const {
	for (auto i = 0; i != int(_slots.size()); ++i) {
		if (_slots[i]->feed.peer == feed.peer
			&& _slots[i]->feed.slot == feed.slot) {
			return i;
		}
	}
	return -1;
}

void Home::startFeed(Slot &slot, int index) {
	slot.ended = false;
	slot.wired = false;
	slot.endpointId.clear();
	slot.maxHeight = 1080;
	slot.missedCount = 0;
	if (slot.folder.isEmpty()) {
		slot.folder = QDir::tempPath()
			+ u"/tdesktop-callstream/feed-"_q
			+ QString::number(_nextFeedFolder++);
	}
	QDir().mkpath(slot.folder);
	if (!_audioEncoder) {
		return;
	}
	auto instance = std::make_unique<StreamFeed>(
		index,
		slot.folder,
		slot.feed.slot,
		_audioEncoder);
	instance->setOnMissedDeadline([this, index] {
		crl::on_main(this, [this, index] {
			reportMissedDeadline(index);
		});
	});
	slot.instance = std::move(instance);
}

void Home::reviveFeed(Slot &slot, int index) {
	if (slot.instance) {
		slot.instance->finish();
		slot.instance.reset();
	}
	startFeed(slot, index);
}

void Home::endFeed(Slot &slot) {
	if (slot.ended) {
		return;
	}
	slot.ended = true;
	slot.wired = false;
	slot.endpointId.clear();
	slot.lifetime = rpl::lifetime();
	if (slot.instance) {
		slot.instance->finish();
	}
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
	auto desired = std::vector<Feed>();
	const auto addDesired = [&](PeerData *peer, const QString &slot) {
		Feed feed;
		feed.peer = peer;
		feed.slot = slot;
		desired.push_back(feed);
	};
	if (_groupCall) {
		if (auto data = _groupCall->lookupReal()) {
			auto self = data->session().user();
			for (const auto &p : data->participants()) {
				if (p.peer == self) continue;
				if (!p.cameraEndpoint().empty() && !p.cameraPaused()) {
					addDesired(p.peer, u"camera"_q);
				}
				if (!p.screenEndpoint().empty() && !p.screenPaused()) {
					addDesired(p.peer, u"screen"_q);
				}
			}
			auto hasRtmp = false;
			for (const auto &kv : _groupCall->activeVideoTracks()) {
				if (kv.first.id == Data::RtmpEndpointId()) {
					hasRtmp = true;
					break;
				}
			}
			if (hasRtmp) {
				addDesired(data->peer(), u"rtmp"_q);
			}
		}
	} else if (_call) {
		if (auto u = _call->user()) {
			addDesired(u, u"camera"_q);
		}
	}

	const auto wanted = [&](const Feed &feed) {
		for (const auto &d : desired) {
			if (d.peer == feed.peer && d.slot == feed.slot) {
				return true;
			}
		}
		return false;
	};

	// A source that is gone ends its stream and leaves the listing.
	for (const auto &slot : _slots) {
		if (slot->ended || wanted(slot->feed)) {
			continue;
		} else if (sourceGone(slot->feed)) {
			endFeed(*slot);
		}
	}

	// Ensure shared audio encoder/mixer exists if we have or will have feeds.
	if (_audioEncoder == nullptr && (!_slots.empty() || !desired.empty())) {
		_audioEncoder = std::make_shared<AudioEncoder>();
		_audioEncoder->setChunkCallback([this](
				const QByteArray &data,
				qint64 ptsSamples,
				qint64 durationSamples,
				qint64 startTimestampMs) {
			for (const auto &slot : _slots) {
				if (isLive(*slot)) {
					slot->instance->submitAudioChunk(
						data,
						ptsSamples,
						durationSamples,
						startTimestampMs);
				}
			}
		});
		_mixer = std::make_unique<Mixer>([this](const AudioPiece &piece) {
			if (_audioEncoder) {
				_audioEncoder->submitPiece(piece);
			}
		});
	}

	for (const auto &d : desired) {
		const auto index = findSlot(d);
		if (index < 0) {
			auto slot = std::make_unique<Slot>();
			slot->feed = d;
			_slots.push_back(std::move(slot));
			startFeed(*_slots.back(), int(_slots.size()) - 1);
		} else if (_slots[index]->ended) {
			reviveFeed(*_slots[index], index);
		} else if (!_slots[index]->instance) {
			startFeed(*_slots[index], index);
		}
	}

	wireVideoTracks();
	requestTopQualityForAll();
	applyFeedMaxHeights();

	_listingChanged.fire({});
}

void Home::submitVideoFrame(int feedIndex, const QImage &frame, const QSize &size) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_slots.size())) {
		return;
	}
	if (isLive(*_slots[feedIndex])) {
		_slots[feedIndex]->instance->submitVideoFrame(frame, size);
	}
}

void Home::submitFrameData(int feedIndex, const VideoFrameData &data) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_slots.size())) {
		return;
	}
	if (isLive(*_slots[feedIndex])) {
		_slots[feedIndex]->instance->submitFrameData(data);
	}
	checkMaxHeightRecovery();
}

void Home::checkMaxHeightRecovery() {
	if (_globalMaxHeight >= 1080) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - _lastRecoveryTime < std::chrono::milliseconds(kRecoveryCheckMs)) {
		return;
	}
	_lastRecoveryTime = now;
	if (now - _lastMissTime < std::chrono::milliseconds(kCalmBeforeRecoveryMs)) {
		return;
	}
	if (_globalMaxHeight <= 360) {
		_globalMaxHeight = 480;
	} else if (_globalMaxHeight <= 480) {
		_globalMaxHeight = 720;
	} else {
		_globalMaxHeight = 1080;
	}
	for (const auto &slot : _slots) {
		slot->missedCount = 0;
	}
	applyFeedMaxHeights();
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
			for (auto index = 0; index != int(_slots.size()); ++index) {
				auto &slot = *_slots[index];
				if (!isLive(slot)) {
					continue;
				}
				const auto endpointId = FeedEndpoint(data, slot.feed);
				if (endpointId.empty()) {
					continue;
				}
				Webrtc::VideoTrack *track = nullptr;
				Calls::VideoEndpoint qualityEndpoint;
				for (const auto &kv : tracks) {
					if (kv.first.id == endpointId) {
						track = Calls::GroupCall::TrackPointer(kv.second).get();
						qualityEndpoint = kv.first;
						break;
					}
				}
				if (!track) {
					slot.wired = false;
					slot.endpointId.clear();
					continue;
				} else if (slot.wired && slot.endpointId == endpointId) {
					continue;
				}
				// Override tile-driven requests: always ask top tier.
				_groupCall->forceVideoQuality(
					qualityEndpoint,
					Calls::Group::VideoQuality::Full);
				slot.lifetime = rpl::lifetime();
				slot.wired = true;
				slot.endpointId = endpointId;
				track->renderNextFrame() | rpl::on_next([this, track, index] {
					submitFrameData(index, FrameDataFrom(track));
				}, slot.lifetime);
			}
		}
	} else if (_call) {
		if (_slots.empty()) {
			return;
		}
		auto &slot = *_slots[0];
		if (!isLive(slot) || slot.wired) {
			return;
		}
		auto track = _call->videoIncoming().get();
		slot.lifetime = rpl::lifetime();
		slot.wired = true;
		track->renderNextFrame() | rpl::on_next([this, track] {
			submitFrameData(0, FrameDataFrom(track));
		}, slot.lifetime);
	}
}

void Home::requestTopQualityForAll() {
	if (!_groupCall) return;
	for (const auto &kv : _groupCall->activeVideoTracks()) {
		_groupCall->forceVideoQuality(kv.first, Calls::Group::VideoQuality::Full);
	}
}

void Home::reportMissedDeadline(int feedIndex) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_slots.size())) return;
	auto &slot = *_slots[feedIndex];
	if (!isLive(slot)) return;
	_lastMissTime = std::chrono::steady_clock::now();
	slot.missedCount++;
	// Single-feed drop: only this feed misses.
	int feedsMissing = 0;
	for (const auto &s : _slots) {
		if (s->missedCount > 0) ++feedsMissing;
	}
	if (feedsMissing == 1 && slot.missedCount >= kSingleThreshold) {
		slot.maxHeight = std::min(slot.maxHeight, 720);
		applyFeedMaxHeights();
		// reset this feed's count
		slot.missedCount = 0;
	}
	// Aggregate pressure: misses across feeds.
	int total = 0;
	for (const auto &s : _slots) total += s->missedCount;
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
			for (const auto &s : _slots) s->missedCount = 0;
			applyFeedMaxHeights();
		}
	}
}

void Home::applyFeedMaxHeights() {
	for (const auto &slot : _slots) {
		if (isLive(*slot)) {
			slot->instance->setMaxHeight(std::min(_globalMaxHeight, slot->maxHeight));
		}
	}
}

void Home::setFeedMaxHeight(int feedIndex, int height) {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_slots.size())) return;
	_slots[feedIndex]->maxHeight = height;
	applyFeedMaxHeights();
}

int Home::feedMaxHeight(int feedIndex) const {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_slots.size())) return _globalMaxHeight;
	return std::min(_globalMaxHeight, _slots[feedIndex]->maxHeight);
}

int Home::missedDeadlineCount() const {
	int total = 0;
	for (const auto &slot : _slots) {
		total += slot->missedCount;
		if (isLive(*slot)) {
			total += slot->instance->missedDeadlines();
		}
	}
	return total;
}

int Home::missedDeadlineCount(int feedIndex) const {
	if (feedIndex < 0 || feedIndex >= static_cast<int>(_slots.size())) return 0;
	int v = _slots[feedIndex]->missedCount;
	if (isLive(*_slots[feedIndex])) {
		v += _slots[feedIndex]->instance->missedDeadlines();
	}
	return v;
}

LinkServiceResponse Home::serveFile(const QString &path) const {
	LinkServiceResponse response;
	// Path like /<id>/<slot>.m3u8 or /<id>/<slot>-seg<nnn>.ts.
	const auto trimmed = path.startsWith('/') ? path.mid(1) : path;
	const auto parts = trimmed.split('/');
	if (parts.size() < 2) {
		return response;
	}
	const auto feedId = parts[0];
	const auto fileName = parts.last();
	QString folder;
	for (const auto &slot : _slots) {
		const auto &feed = slot->feed;
		if (!feed.peer || QString::number(feed.peer->id.value) != feedId) {
			continue;
		} else if (!fileName.startsWith(feed.slot)) {
			continue;
		}
		folder = slot->folder;
		break;
	}
	if (folder.isEmpty()
		&& _slots.size() == 1
		&& !_slots[0]->folder.isEmpty()
		&& fileName.endsWith(u".m3u8"_q)) {
		folder = _slots[0]->folder;
	}
	if (folder.isEmpty()) {
		return response;
	}
	auto actualName = fileName;
	auto fullPath = folder + u"/"_q + actualName;
	if (!QFile::exists(fullPath) && fileName.endsWith(u".m3u8"_q)) {
		actualName = u"playlist.m3u8"_q;
		fullPath = folder + u"/"_q + actualName;
	}
	QFile file(fullPath);
	if (!file.open(QIODevice::ReadOnly)) {
		return response;
	}
	response.body = file.readAll();
	if (response.body.isEmpty()) {
		// The muxer may be in the middle of rewriting the playlist.
		file.seek(0);
		response.body = file.readAll();
	}
	response.isFile = true;
	response.contentType = actualName.endsWith(u".m3u8"_q)
		? "application/vnd.apple.mpegurl"
		: actualName.endsWith(u".ts"_q)
		? "video/MP2T"
		: "application/octet-stream";
	return response;
}

QString Home::listing() const {
	if (!_enabled) return u"streaming is off"_q;
	if (!_call && !_groupCall) return u"no call right now"_q;
	// JSON notation: list of { "username", "stream" }.
	QStringList entries;
	for (const auto &slot : _slots) {
		if (!isLive(*slot)) {
			continue;
		}
		const auto name = slot->feed.name();
		const auto link = slot->feed.link();
		entries << (u"{\"username\":\""_q + name + u"\",\"stream\":\""_q + link + u"\"}"_q);
	}
	if (entries.isEmpty()) return u"no feeds right now"_q;
	return u"["_q + entries.join(u","_q) + u"]"_q;
}

std::vector<Feed> Home::feeds() const {
	auto result = std::vector<Feed>();
	result.reserve(_slots.size());
	for (const auto &slot : _slots) {
		if (isLive(*slot)) {
			result.push_back(slot->feed);
		}
	}
	return result;
}

QString Feed::name() const {
	return peer ? peer->name() : QString();
}

QString Feed::link() const {
	const auto id = peer ? QString::number(peer->id.value) : QString(u"0"_q);
	return u"http://127.0.0.1:8479/"_q + id + u"/"_q + slot + u".m3u8"_q;
}

} // namespace CallStream
