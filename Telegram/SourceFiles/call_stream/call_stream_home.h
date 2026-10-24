/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.
*/

#pragma once

#include "rpl/lifetime.h"
#include "rpl/producer.h"
#include "rpl/event_stream.h"

#include "call_stream/call_stream_types.h"
#include "call_stream/call_stream_link_service.h"

#include <QtCore/QObject>
#include <QImage>
#include <QSize>
#include <memory>
#include <vector>
#include <chrono>
#include <string>
#include <QString>

namespace Calls {
class Call;
class GroupCall;
} // namespace Calls

class PeerData;

namespace CallStream {

struct Feed {
	PeerData *peer = nullptr;
	QString slot;

	[[nodiscard]] QString name() const;
	[[nodiscard]] QString link() const;
};

class AudioEncoder;
class Mixer;
class StreamFeed;

class Home final : public QObject {
public:
	static void Start();
	static void Shutdown();
	[[nodiscard]] static Home *Instance();

	// Scripted seam: same entry point real observation uses.
	void announceCallChanged(Calls::Call *call, Calls::GroupCall *groupCall);

	[[nodiscard]] QString listing() const;
	[[nodiscard]] std::vector<Feed> feeds() const;

	void setStreamingEnabled(bool enabled);
	[[nodiscard]] bool streamingEnabled() const { return _enabled; }

	void submitVideoFrame(int feedIndex, const QImage &frame, const QSize &size);
	void submitFrameData(int feedIndex, const VideoFrameData &data);
	void submitAudioPiece(const AudioPiece &piece);
	void handlePlayoutTap(std::vector<int16_t> samples, int sampleRate, int channels, qint64 timestampMs);
	void handleCaptureTap(std::vector<int16_t> samples, int sampleRate, int channels);

	// Quality / pressure seams.
	void requestTopQualityForAll();
	void reportMissedDeadline(int feedIndex);
	void noteMissedDeadline(int feedIndex) { reportMissedDeadline(feedIndex); }
	[[nodiscard]] int globalMaxHeight() const { return _globalMaxHeight; }
	[[nodiscard]] int feedMaxHeight(int feedIndex) const;
	[[nodiscard]] int missedDeadlineCount() const;
	[[nodiscard]] int missedDeadlineCount(int feedIndex) const;
	void setFeedMaxHeight(int feedIndex, int height);

	[[nodiscard]] LinkServiceResponse serveFile(const QString &path) const;

	[[nodiscard]] rpl::producer<> listingChanged() const {
		return _listingChanged.events();
	}

private:
	struct Slot {
		Feed feed;
		std::unique_ptr<StreamFeed> instance;
		rpl::lifetime lifetime;
		QString folder;
		std::string endpointId;
		int maxHeight = 1080;
		int missedCount = 0;
		bool wired = false;
		bool ended = false;
	};

	Home();
	~Home() override;

	void observe();
	void rebuildFeeds();
	void clearFeeds();
	void dropSlots();
	void startFeed(Slot &slot, int index);
	void reviveFeed(Slot &slot, int index);
	void endFeed(Slot &slot);
	void wireVideoTracks();
	void applyFeedMaxHeights();
	void checkMaxHeightRecovery();

	[[nodiscard]] int findSlot(const Feed &feed) const;
	[[nodiscard]] bool sourceGone(const Feed &feed) const;
	[[nodiscard]] bool isLive(const Slot &slot) const;

	bool _enabled = true;
	std::vector<std::unique_ptr<Slot>> _slots;
	std::shared_ptr<AudioEncoder> _audioEncoder;
	std::unique_ptr<Mixer> _mixer;
	Calls::Call *_call = nullptr;
	Calls::GroupCall *_groupCall = nullptr;
	rpl::lifetime _lifetime;
	rpl::lifetime _callLifetime;
	rpl::event_stream<> _listingChanged;

	// Quality / pressure state.
	int _nextFeedFolder = 0;
	int _globalMaxHeight = 1080;
	std::chrono::steady_clock::time_point _lastShrinkTime{};
	bool _lastShrinkSet = false;
	std::chrono::steady_clock::time_point _lastMissTime{};
	std::chrono::steady_clock::time_point _lastRecoveryTime{};
	static constexpr int kShrinkIntervalMs = 3000;
	static constexpr int kSingleThreshold = 2;
	static constexpr int kAggregateThreshold = 2;
	static constexpr int kRecoveryCheckMs = 2000;
	static constexpr int kCalmBeforeRecoveryMs = 15000;

	static Home *_instance;
};

} // namespace CallStream
