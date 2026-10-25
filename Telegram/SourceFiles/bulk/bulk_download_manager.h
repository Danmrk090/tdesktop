/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "bulk/bulk_settings.h"

class DocumentData;
class HistoryItem;
class PeerData;
class PhotoData;

namespace Main {
class Session;
} // namespace Main

namespace Data {
class PhotoMedia;
} // namespace Data

namespace Storage {
enum class SharedMediaType : signed char;
} // namespace Storage

namespace Window {
class SessionController;
} // namespace Window

namespace Bulk {

enum class EntryState : uchar {
	Queued,
	Active,
	Done,
	Skipped,
	Failed,
};

struct Entry {
	FullMsgId itemId;
	uint64 mediaId = 0;
	TimeId date = 0;
	int64 size = 0;
	QString path; // absolute, chosen when the download starts
	QString error;
	crl::time retryAt = 0;
	int retries = 0;
	int waits = 0; // waiting for the chat or the message to load
	EntryState state = EntryState::Queued;
	bool photo = false;
};

enum class JobState : uchar {
	Running,
	Paused,
	Finished,
	Cancelled,
};

enum class PauseReason : uchar {
	None,
	User,
	LowDisk,
};

struct JobOptions {
	QString folder; // absolute, with a trailing slash
	QString chatName;
	QString nameTemplate;
	bool skipExisting = true;
};

struct ScanParams {
	PeerId peerId = 0;
	MsgId topicRootId = 0;
	int type = 0; // index in ScanTypes()
	MsgId offsetId = 0; // 0 means from the newest message
	Filters filters;
	int found = 0;
	int checked = 0;
	bool active = false;
	bool finished = false;
	QString error;
};

struct Job {
	uint64 id = 0;
	QString title;
	PeerId peerId = 0;
	JobOptions options;
	std::vector<Entry> entries;
	ScanParams scan;
	QString lastPath;
	TimeId created = 0;
	int nextIndex = 0; // entries before it are not queued
	JobState state = JobState::Running;
	PauseReason pauseReason = PauseReason::None;
	bool autoSave = false;
	bool checksumsWritten = false;
	bool notified = false;
};

struct JobProgress {
	int total = 0;
	int done = 0;
	int skipped = 0;
	int failed = 0;
	int active = 0;
	int queued = 0;
	int64 bytesTotal = 0;
	int64 bytesDone = 0;
};

struct SpeedInfo {
	int64 bytesPerSecond = 0;
	int connections = 0;
	int requests = 0;
	int64 bytesInFlight = 0;
	int queuedFiles = 0;
	int premiumDelays = 0;
	bool premium = false;
	bool scheduleWaiting = false;
};

struct AutoSaveRule {
	PeerId peerId = 0;
	QString title;
	QString folder;
	uint32 kinds = 0;
};

struct ScanType {
	Storage::SharedMediaType type;
	uint32 kinds = 0;
};
[[nodiscard]] const std::vector<ScanType> &ScanTypes();
[[nodiscard]] QString ScanTypeName(int index);

class Manager final : public base::has_weak_ptr {
public:
	explicit Manager(not_null<Main::Session*> session);
	~Manager();

	[[nodiscard]] not_null<Main::Session*> session() const {
		return _session;
	}

	uint64 addJob(
		QString title,
		PeerId peerId,
		JobOptions options,
		std::vector<Entry> entries);
	uint64 addScanJob(
		QString title,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		int type,
		Filters filters,
		JobOptions options);

	void pause(uint64 jobId);
	void resume(uint64 jobId);
	void cancel(uint64 jobId);
	void retryFailed(uint64 jobId);
	void remove(uint64 jobId);
	void clearFinished();

	[[nodiscard]] const std::vector<std::unique_ptr<Job>> &jobs() const {
		return _jobs;
	}
	[[nodiscard]] Job *job(uint64 jobId) const;
	[[nodiscard]] JobProgress progress(not_null<const Job*> job) const;
	[[nodiscard]] int64 jobSpeed(uint64 jobId) const;
	[[nodiscard]] SpeedInfo speed() const;
	[[nodiscard]] rpl::producer<> structureChanged() const {
		return _structureChanged.events();
	}
	[[nodiscard]] rpl::producer<> progressChanged() const {
		return _progressChanged.events();
	}

	[[nodiscard]] bool downloadedBefore(
		PhotoData *photo,
		DocumentData *document) const;
	[[nodiscard]] bool isManaged(not_null<DocumentData*> document) const;

	[[nodiscard]] const std::vector<AutoSaveRule> &autoSaveRules() const {
		return _autoSave;
	}
	[[nodiscard]] const AutoSaveRule *autoSaveRule(PeerId peerId) const;
	void setAutoSaveRule(AutoSaveRule rule); // no kinds removes the rule

	void showWindowToast(const QString &text, const QString &path = {});

private:
	struct EntryRef {
		uint64 jobId = 0;
		int index = 0;
	};
	struct ActivePhoto {
		EntryRef ref;
		std::shared_ptr<Data::PhotoMedia> view;
		crl::time requested = 0;
		int retries = 0;
	};
	struct IndexValue {
		QString path;
		int64 size = 0;
	};
	enum class StartResult {
		Started,
		Waiting,
		Finished,
		Busy,
	};

	void restore();
	void subscribe();
	void pump();
	void pumpOnce();
	void schedulePump();
	[[nodiscard]] StartResult start(not_null<Job*> job, int index);
	[[nodiscard]] StartResult startDocument(
		not_null<Job*> job,
		int index,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document);
	[[nodiscard]] StartResult startPhoto(
		not_null<Job*> job,
		int index,
		not_null<HistoryItem*> item,
		not_null<PhotoData*> photo);
	[[nodiscard]] bool resolvePath(
		not_null<Job*> job,
		int index,
		not_null<HistoryItem*> item,
		const QString &name);
	[[nodiscard]] bool tryUseIndex(
		not_null<Job*> job,
		int index,
		not_null<HistoryItem*> item);
	void requestItem(not_null<Job*> job, int index);
	void documentProgress(not_null<DocumentData*> document);
	void finishDocument(EntryRef ref, not_null<DocumentData*> document);
	void checkPhotos();
	[[nodiscard]] bool savePhoto(const ActivePhoto &active);
	void complete(EntryRef ref);
	void fail(EntryRef ref, const QString &error, bool retry);
	void skip(EntryRef ref, const QString &error);
	void stopActive(not_null<Job*> job);
	void checkJobFinished(not_null<Job*> job);
	void writeChecksums(not_null<Job*> job);
	[[nodiscard]] bool checkDiskSpace(not_null<Job*> job);
	void checkLowDiskJobs();
	void updateSpeed();
	void scanNext(uint64 jobId);
	void scanDone(
		uint64 jobId,
		MsgId offsetId,
		MsgId smallest,
		const QVector<MsgId> &ids);
	void scanFailed(uint64 jobId, const QString &error);
	void newItem(not_null<HistoryItem*> item);
	[[nodiscard]] not_null<Job*> autoSaveJob(const AutoSaveRule &rule);

	[[nodiscard]] Entry *entry(EntryRef ref) const;
	[[nodiscard]] QString indexKey(const Entry &entry) const;
	[[nodiscard]] std::optional<IndexValue> lookupIndex(
		const QString &key) const;
	void addToIndex(const Entry &entry);

	void changed(bool structure = false);
	void saveLater();
	void saveNow();
	void saveIndexNow();
	[[nodiscard]] QString jobsPath() const;
	[[nodiscard]] QString indexPath() const;

	const not_null<Main::Session*> _session;
	std::vector<std::unique_ptr<Job>> _jobs;
	std::vector<AutoSaveRule> _autoSave;
	uint64 _lastJobId = 0;

	base::flat_map<not_null<DocumentData*>, EntryRef> _activeDocuments;
	std::vector<ActivePhoto> _activePhotos;
	base::flat_set<FullMsgId> _requestingItems;
	base::flat_set<QString> _reserved;
	base::flat_map<uint64, mtpRequestId> _scanRequests;
	base::flat_map<uint64, int> _scanWaits;
	base::flat_map<QString, IndexValue> _index;
	base::flat_map<uint64, int64> _jobBytes;
	base::flat_map<uint64, int64> _jobSpeeds;
	base::flat_map<QString, std::pair<crl::time, int64>> _freeSpace;

	std::deque<int64> _speedSamples;
	int64 _lastReceived = -1;
	int64 _speed = 0;

	base::Timer _pumpTimer;
	base::Timer _photosTimer;
	base::Timer _speedTimer;
	base::Timer _saveTimer;
	base::Timer _indexSaveTimer;
	base::Timer _diskTimer;
	bool _pumping = false;
	bool _pumpAgain = false;
	bool _restoring = false;
	bool _scheduleWaiting = false;
	bool _lowDiskNotified = false;

	rpl::event_stream<> _structureChanged;
	rpl::event_stream<> _progressChanged;
	rpl::lifetime _lifetime;

};

[[nodiscard]] Manager &Get(not_null<Main::Session*> session);
[[nodiscard]] Manager *Find(not_null<Main::Session*> session);

// Bulk downloads handle failures themselves, without a box per file.
[[nodiscard]] bool IsManagedDownload(not_null<DocumentData*> document);

[[nodiscard]] std::optional<Entry> EntryFromItem(
	not_null<HistoryItem*> item);
[[nodiscard]] bool EntryMatches(
	const Filters &filters,
	not_null<HistoryItem*> item,
	const Manager *manager);
[[nodiscard]] QString EntryFileName(not_null<HistoryItem*> item);
void SortEntries(std::vector<Entry> &entries, Order order);

// Called for every main window of a session.
void OnSessionWindow(not_null<Window::SessionController*> window);

} // namespace Bulk
