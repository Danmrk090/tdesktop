/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk/bulk_download_manager.h"

#include "apiwrap.h"
#include "base/base_file_utilities.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "bulk/bulk_metadata.h"
#include "bulk/bulk_upload.h"
#include "bulk/bulk_update_check.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_messages.h"
#include "data/data_search_controller.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "menu/menu_item_download_files.h"
#include "storage/download_manager_mtproto.h"
#include "storage/storage_shared_media.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "window/window_session_controller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStorageInfo>

namespace Bulk {
namespace {

constexpr auto kParallelPhotos = 6; // decoded photos are kept in memory
constexpr auto kPhotoCheckInterval = crl::time(1000);
constexpr auto kPhotoRetryTimeout = crl::time(5000);
constexpr auto kPhotoMaxRetries = 3;
constexpr auto kMaxRetries = 3;
constexpr auto kMaxWaits = 30;
constexpr auto kWaitTimeout = crl::time(10000);
constexpr auto kBusyTimeout = crl::time(2000);
constexpr auto kSaveDelay = crl::time(3000);
constexpr auto kSaveDelayLarge = crl::time(30000);
constexpr auto kLargeStateEntries = 5000;
constexpr auto kIndexSaveDelay = crl::time(60000);
constexpr auto kSpeedInterval = crl::time(1000);
constexpr auto kSpeedSamples = 5;
constexpr auto kDiskCheckInterval = crl::time(30000);
constexpr auto kFreeSpaceCacheTimeout = crl::time(10000);
constexpr auto kRestoreDelay = crl::time(4000);
constexpr auto kScanLimit = 100;
constexpr auto kMaxItemRequests = 100;
constexpr auto kScanDelay = crl::time(300);
constexpr auto kMaxNameAttempts = 10000;
constexpr auto kKeepFinishedJobs = 30;
constexpr auto kStateVersion = 1;

constexpr auto kRetryDelays = std::array<crl::time, kMaxRetries>{
	crl::time(5000),
	crl::time(30000),
	crl::time(120000),
};

struct Target {
	QString path;
	bool skip = false;
};

[[nodiscard]] auto Managers()
-> base::flat_map<not_null<Main::Session*>, std::unique_ptr<Manager>> & {
	static auto result = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<Manager>>();
	return result;
}

[[nodiscard]] base::flat_set<not_null<DocumentData*>> &ManagedDocuments() {
	static auto result = base::flat_set<not_null<DocumentData*>>();
	return result;
}

[[nodiscard]] QString DateTag(TimeId date) {
	const auto when = date
		? base::unixtime::parse(date)
		: QDateTime::currentDateTime();
	return when.toString(u"yyyy-MM-dd_HH-mm-ss"_q);
}

[[nodiscard]] QString RemoveBidiControls(QString name) {
	static const auto kControls = std::array<char16_t, 12>{
		0x200E, 0x200F, 0x202A, 0x202B, 0x202C, 0x202D, 0x202E,
		0x2066, 0x2067, 0x2068, 0x2069, 0x061C,
	};
	for (const auto ch : kControls) {
		name.remove(QChar(ch));
	}
	return name;
}

[[nodiscard]] QString MaskDangerousExtension(QString name) {
#ifdef Q_OS_WIN
	const auto lower = name.trimmed().toLower();
	for (const auto &extension : { u".lnk"_q, u".scf"_q, u".url"_q }) {
		if (lower.endsWith(extension)) {
			return name + u".download"_q;
		}
	}
#endif // Q_OS_WIN
	return name;
}

[[nodiscard]] QString PhotoFileName(
		FullMsgId itemId,
		TimeId date,
		bool video) {
	return u"photo_%1_%2%3"_q
		.arg(DateTag(date))
		.arg(itemId.msg.bare)
		.arg(video ? u".mp4"_q : u".jpg"_q);
}

[[nodiscard]] QString DocumentFileName(
		not_null<DocumentData*> document,
		FullMsgId itemId,
		TimeId date) {
	const auto original = RemoveBidiControls(document->filename()).trimmed();
	if (!original.isEmpty()) {
		return MaskDangerousExtension(
			base::FileNameFromUserString(original));
	}
	const auto mime = Core::MimeTypeForName(document->mimeString());
	const auto patterns = mime.globPatterns();
	auto extension = patterns.isEmpty()
		? QString()
		: QString(patterns.front()).replace('*', QString());
	auto prefix = u"file"_q;
	if (document->isVoiceMessage()) {
		prefix = u"voice"_q;
		extension = document->hasMimeType(u"audio/mp3"_q)
			? u".mp3"_q
			: u".ogg"_q;
	} else if (document->isVideoFile()
		|| document->isVideoMessage()
		|| document->isAnimation()) {
		prefix = u"video"_q;
		if (extension.isEmpty()) {
			extension = u".mp4"_q;
		}
	} else if (document->isAudioFile()) {
		prefix = u"audio"_q;
	}
	if (extension.isEmpty() || !extension.startsWith('.')) {
		extension = u".bin"_q;
	}
	return MaskDangerousExtension(u"%1_%2_%3%4"_q
		.arg(prefix)
		.arg(DateTag(date))
		.arg(itemId.msg.bare)
		.arg(extension));
}

// A same-named file of the same size is treated as already downloaded.
[[nodiscard]] Target ResolveTarget(
		const QString &folder,
		const QString &name,
		int64 expectedSize,
		bool skipExisting,
		base::flat_set<QString> &reserved) {
	const auto dot = name.lastIndexOf('.');
	const auto base = (dot > 0) ? name.mid(0, dot) : name;
	const auto extension = (dot > 0) ? name.mid(dot) : QString();
	for (auto i = 1; i != kMaxNameAttempts; ++i) {
		const auto candidate = folder + ((i == 1)
			? (base + extension)
			: (base + u" (%1)"_q.arg(i) + extension));
		const auto key = candidate.toLower();
		if (reserved.contains(key)) {
			continue;
		}
		const auto info = QFileInfo(candidate);
		if (info.exists()) {
			if (skipExisting
				&& expectedSize > 0
				&& info.isFile()
				&& info.size() == expectedSize) {
				return { .path = candidate, .skip = true };
			}
			continue;
		}
		reserved.emplace(key);
		return { .path = candidate };
	}
	return {};
}

void SetFileTimes(const QString &path, TimeId date) {
	if (date <= 0) {
		return;
	}
	auto file = QFile(path);
	if (file.open(QIODevice::ReadWrite)) {
		const auto when = base::unixtime::parse(date);
		file.setFileTime(when, QFileDevice::FileModificationTime);
		file.setFileTime(when, QFileDevice::FileAccessTime);
	}
}

void WriteCaption(const QString &path, not_null<HistoryItem*> item) {
	const auto text = item->originalText().text.trimmed();
	if (text.isEmpty()) {
		return;
	}
	auto file = QFile(path + u".txt"_q);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(text.toUtf8());
	}
}

[[nodiscard]] QString Str(uint64 value) {
	return QString::number(value);
}

[[nodiscard]] uint64 ReadU64(const QJsonValue &value) {
	return value.isString()
		? value.toString().toULongLong()
		: uint64(value.toDouble());
}

[[nodiscard]] QJsonObject SerializeFilters(const Filters &filters) {
	auto result = QJsonObject();
	result.insert(u"minSize"_q, Str(filters.minSize));
	result.insert(u"maxSize"_q, Str(filters.maxSize));
	result.insert(u"minDate"_q, qint64(filters.minDate));
	result.insert(u"maxDate"_q, qint64(filters.maxDate));
	result.insert(u"ext"_q, QJsonArray::fromStringList(filters.extensions));
	result.insert(u"kinds"_q, qint64(filters.kinds));
	result.insert(u"notDownloaded"_q, filters.onlyNotDownloaded);
	return result;
}

[[nodiscard]] Filters DeserializeFilters(const QJsonObject &object) {
	auto result = Filters();
	result.minSize = int64(ReadU64(object.value(u"minSize"_q)));
	result.maxSize = int64(ReadU64(object.value(u"maxSize"_q)));
	result.minDate = TimeId(object.value(u"minDate"_q).toInteger());
	result.maxDate = TimeId(object.value(u"maxDate"_q).toInteger());
	for (const auto &value : object.value(u"ext"_q).toArray()) {
		result.extensions.push_back(value.toString());
	}
	result.kinds = uint32(object.value(u"kinds"_q).toInteger(kAllKinds));
	result.onlyNotDownloaded = object.value(u"notDownloaded"_q).toBool();
	return result;
}

[[nodiscard]] bool IsTerminal(EntryState state) {
	return (state == EntryState::Done)
		|| (state == EntryState::Skipped)
		|| (state == EntryState::Failed);
}

[[nodiscard]] QString FolderOf(const QString &path) {
	const auto slash = path.lastIndexOf('/');
	return (slash >= 0) ? path.mid(0, slash + 1) : QString();
}

} // namespace

const std::vector<ScanType> &ScanTypes() {
	using Type = Storage::SharedMediaType;
	static const auto result = std::vector<ScanType>{
		{ Type::PhotoVideo, uint32(Kind::Photo) | uint32(Kind::Video) },
		{ Type::Photo, uint32(Kind::Photo) },
		{ Type::Video, uint32(Kind::Video) },
		{ Type::File, uint32(Kind::File) },
		{ Type::MusicFile, uint32(Kind::Music) },
		{ Type::RoundVoiceFile, uint32(Kind::Voice) | uint32(Kind::Round) },
		{ Type::GIF, uint32(Kind::Gif) },
	};
	return result;
}

QString ScanTypeName(int index) {
	switch (index) {
	case 0: return tr::lng_bulk_scan_photo_video(tr::now);
	case 1: return KindName(Kind::Photo);
	case 2: return KindName(Kind::Video);
	case 3: return KindName(Kind::File);
	case 4: return KindName(Kind::Music);
	case 5: return KindName(Kind::Voice);
	case 6: return KindName(Kind::Gif);
	}
	return QString();
}

std::optional<Entry> EntryFromItem(not_null<HistoryItem*> item) {
	if (!Menu::CanBulkDownload(item)) {
		return std::nullopt;
	}
	const auto media = item->media();
	if (const auto photo = media->photo()) {
		return Entry{
			.itemId = item->fullId(),
			.mediaId = photo->id,
			.date = item->date(),
			.size = int64(photo->imageByteSize(Data::PhotoSize::Large)),
			.photo = true,
		};
	} else if (const auto document = media->document()) {
		return Entry{
			.itemId = item->fullId(),
			.mediaId = document->id,
			.date = item->date(),
			.size = document->size,
		};
	}
	return std::nullopt;
}

QString EntryFileName(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return QString();
	} else if (media->photo()) {
		return PhotoFileName(item->fullId(), item->date(), false);
	} else if (const auto document = media->document()) {
		return DocumentFileName(document, item->fullId(), item->date());
	}
	return QString();
}

bool EntryMatches(
		const Filters &filters,
		not_null<HistoryItem*> item,
		const Manager *manager) {
	const auto media = item->media();
	if (!media) {
		return false;
	}
	const auto photo = media->photo();
	const auto document = photo ? nullptr : media->document();
	if (!photo && !document) {
		return false;
	}
	const auto kind = KindOf(photo, document);
	if (!(filters.kinds & uint32(kind))) {
		return false;
	}
	const auto size = photo
		? int64(photo->imageByteSize(Data::PhotoSize::Large))
		: document->size;
	if (filters.minSize > 0 && size < filters.minSize) {
		return false;
	} else if (filters.maxSize > 0 && size > filters.maxSize) {
		return false;
	}
	const auto date = item->date();
	if (filters.minDate > 0 && date < filters.minDate) {
		return false;
	} else if (filters.maxDate > 0 && date > filters.maxDate) {
		return false;
	}
	if (!filters.extensions.isEmpty()) {
		const auto extension = ExtensionOf(EntryFileName(item));
		if (!filters.extensions.contains(extension)) {
			return false;
		}
	}
	if (filters.onlyNotDownloaded
		&& manager
		&& manager->downloadedBefore(photo, document)) {
		return false;
	}
	return true;
}

void SortEntries(std::vector<Entry> &entries, Order order) {
	switch (order) {
	case Order::Oldest:
		ranges::stable_sort(entries, std::less<>(), &Entry::itemId);
		break;
	case Order::Newest:
		ranges::stable_sort(entries, std::greater<>(), &Entry::itemId);
		break;
	case Order::Smallest:
		ranges::stable_sort(entries, std::less<>(), &Entry::size);
		break;
	case Order::Largest:
		ranges::stable_sort(entries, std::greater<>(), &Entry::size);
		break;
	}
}

Manager::Manager(not_null<Main::Session*> session)
: _session(session)
, _pumpTimer([=] { pump(); })
, _photosTimer([=] { checkPhotos(); })
, _speedTimer([=] { updateSpeed(); })
, _saveTimer([=] { saveNow(); })
, _indexSaveTimer([=] { saveIndexNow(); })
, _diskTimer([=] { checkLowDiskJobs(); }) {
	restore();
	subscribe();
	_speedTimer.callEach(kSpeedInterval);
	_diskTimer.callEach(kDiskCheckInterval);
	_restoring = true;
	base::call_delayed(kRestoreDelay, this, [=] {
		_restoring = false;
		for (const auto &job : _jobs) {
			if (job->scan.active && !job->scan.finished) {
				scanNext(job->id);
			}
		}
		pump();
	});
}

Manager::~Manager() {
	for (const auto &[document, ref] : _activeDocuments) {
		ManagedDocuments().remove(document);
	}
	saveNow();
	saveIndexNow();
}

void Manager::subscribe() {
	_session->data().documentLoadProgress(
	) | rpl::on_next([=](not_null<DocumentData*> document) {
		documentProgress(document);
	}, _lifetime);

	_session->downloaderTaskFinished(
	) | rpl::on_next([=] {
		if (!_activePhotos.empty()) {
			checkPhotos();
		}
	}, _lifetime);

	_session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		newItem(item);
	}, _lifetime);

	Changes() | rpl::on_next([=] {
		pump();
	}, _lifetime);
}

Job *Manager::job(uint64 jobId) const {
	const auto i = ranges::find(_jobs, jobId, &Job::id);
	return (i != end(_jobs)) ? i->get() : nullptr;
}

Entry *Manager::entry(EntryRef ref) const {
	if (const auto job = this->job(ref.jobId)) {
		if (ref.index >= 0 && ref.index < int(job->entries.size())) {
			return &job->entries[ref.index];
		}
	}
	return nullptr;
}

uint64 Manager::addJob(
		QString title,
		PeerId peerId,
		JobOptions options,
		std::vector<Entry> entries) {
	auto job = std::make_unique<Job>();
	job->id = ++_lastJobId;
	job->title = std::move(title);
	job->peerId = peerId;
	job->options = std::move(options);
	job->entries = std::move(entries);
	job->created = base::unixtime::now();
	QDir().mkpath(job->options.folder);
	const auto id = job->id;
	_jobs.push_back(std::move(job));
	changed(true);
	pump();
	return id;
}

uint64 Manager::addScanJob(
		QString title,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		int type,
		Filters filters,
		JobOptions options) {
	const auto id = addJob(
		std::move(title),
		peer->id,
		std::move(options),
		{});
	if (const auto job = this->job(id)) {
		job->scan.peerId = peer->id;
		job->scan.topicRootId = topicRootId;
		job->scan.type = std::clamp(type, 0, int(ScanTypes().size()) - 1);
		job->scan.filters = std::move(filters);
		job->scan.active = true;
		changed(true);
		scanNext(id);
	}
	return id;
}

void Manager::scanNext(uint64 jobId) {
	const auto job = this->job(jobId);
	if (!job
		|| !job->scan.active
		|| job->scan.finished
		|| job->state == JobState::Cancelled
		|| _scanRequests.contains(jobId)) {
		return;
	}
	const auto peer = _session->data().peerLoaded(job->scan.peerId);
	if (!peer) {
		// Wait for the chats list to load after a restart.
		if (++_scanWaits[jobId] > kMaxWaits) {
			scanFailed(jobId, tr::lng_bulk_error_chat(tr::now));
		} else {
			base::call_delayed(kWaitTimeout, this, [=] {
				scanNext(jobId);
			});
		}
		return;
	}
	_scanWaits.remove(jobId);
	const auto &types = ScanTypes();
	const auto type = types[std::clamp(job->scan.type, 0, int(types.size()) - 1)].type;
	const auto offsetId = job->scan.offsetId;
	const auto topicRootId = job->scan.topicRootId;
	const auto &filters = job->scan.filters;
	using Flag = MTPmessages_Search::Flag;
	const auto requestId = _session->api().request(MTPmessages_Search(
		MTP_flags(topicRootId ? Flag::f_top_msg_id : Flag(0)),
		peer->input(),
		MTP_string(QString()),
		MTP_inputPeerEmpty(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTP_int(topicRootId),
		Api::PrepareSearchFilter(type),
		MTP_int(std::max(filters.minDate, TimeId(0))),
		MTP_int(std::max(filters.maxDate, TimeId(0))),
		MTP_int(int(std::clamp(offsetId.bare, int64(0), int64(0x3FFFFFFF)))),
		MTP_int(0), // add_offset
		MTP_int(kScanLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		_scanRequests.remove(jobId);
		const auto job = this->job(jobId);
		const auto peer = _session->data().peerLoaded(
			job ? job->scan.peerId : PeerId());
		if (!job || !peer) {
			return;
		}
		// The next page starts below the smallest id the server sent.
		auto smallest = MsgId();
		result.match([](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &message : data.vmessages().v) {
				const auto id = IdFromMessage(message);
				if (id > 0 && (!smallest || id < smallest)) {
					smallest = id;
				}
			}
		});
		auto parsed = Api::ParseSearchResult(
			peer,
			type,
			offsetId,
			Data::LoadDirection::Before,
			result);
		auto ids = QVector<MsgId>();
		ids.reserve(parsed.messageIds.size());
		for (const auto id : parsed.messageIds) {
			ids.push_back(id);
		}
		scanDone(jobId, offsetId, smallest, ids);
	}).fail([=](const MTP::Error &error) {
		_scanRequests.remove(jobId);
		scanFailed(jobId, error.type());
	}).send();
	_scanRequests.emplace(jobId, requestId);
}

void Manager::scanDone(
		uint64 jobId,
		MsgId offsetId,
		MsgId smallest,
		const QVector<MsgId> &ids) {
	const auto job = this->job(jobId);
	if (!job) {
		return;
	}
	auto added = 0;
	auto known = base::flat_set<uint64>();
	for (const auto &entry : job->entries) {
		known.emplace(entry.mediaId);
	}
	for (const auto id : ids) {
		++job->scan.checked;
		const auto item = _session->data().message(job->scan.peerId, id);
		if (!item || !EntryMatches(job->scan.filters, item, this)) {
			continue;
		} else if (auto entry = EntryFromItem(item)) {
			if (known.emplace(entry->mediaId).second) {
				job->entries.push_back(std::move(*entry));
				++added;
			}
		}
	}
	job->scan.found += added;
	if (!smallest || (offsetId && smallest >= offsetId)) {
		job->scan.finished = true;
		job->scan.active = false;
	} else {
		job->scan.offsetId = smallest;
	}
	changed(true);
	if (added) {
		pump();
	}
	if (job->scan.finished) {
		checkJobFinished(job);
	} else {
		base::call_delayed(kScanDelay, this, [=] { scanNext(jobId); });
	}
}

void Manager::scanFailed(uint64 jobId, const QString &error) {
	const auto job = this->job(jobId);
	if (!job) {
		return;
	}
	job->scan.error = error;
	job->scan.finished = true;
	job->scan.active = false;
	changed(true);
	checkJobFinished(job);
}

void Manager::pause(uint64 jobId) {
	const auto job = this->job(jobId);
	if (!job || job->state != JobState::Running) {
		return;
	}
	stopActive(job);
	job->state = JobState::Paused;
	job->pauseReason = PauseReason::User;
	changed(true);
	pump();
}

void Manager::resume(uint64 jobId) {
	const auto job = this->job(jobId);
	if (!job
		|| (job->state != JobState::Paused
			&& job->state != JobState::Finished)) {
		return;
	}
	job->state = JobState::Running;
	job->pauseReason = PauseReason::None;
	job->nextIndex = 0;
	_lowDiskNotified = false;
	_freeSpace.clear();
	changed(true);
	if (job->scan.active && !job->scan.finished) {
		scanNext(jobId);
	}
	pump();
}

void Manager::cancel(uint64 jobId) {
	const auto job = this->job(jobId);
	if (!job) {
		return;
	}
	stopActive(job);
	if (const auto i = _scanRequests.find(jobId); i != end(_scanRequests)) {
		_session->api().request(i->second).cancel();
		_scanRequests.erase(i);
	}
	job->scan.active = false;
	job->state = JobState::Cancelled;
	job->pauseReason = PauseReason::None;
	changed(true);
	pump();
}

void Manager::retryFailed(uint64 jobId) {
	const auto job = this->job(jobId);
	if (!job) {
		return;
	}
	auto any = false;
	for (auto &entry : job->entries) {
		if (entry.state == EntryState::Failed) {
			entry.state = EntryState::Queued;
			entry.retries = 0;
			entry.waits = 0;
			entry.retryAt = 0;
			entry.error = QString();
			any = true;
		}
	}
	if (!any) {
		return;
	}
	job->nextIndex = 0;
	job->checksumsWritten = false;
	job->notified = false;
	if (job->state == JobState::Finished
		|| job->state == JobState::Cancelled) {
		job->state = JobState::Running;
	}
	changed(true);
	pump();
}

void Manager::remove(uint64 jobId) {
	if (const auto job = this->job(jobId)) {
		cancel(jobId);
		if (job->autoSave) {
			const auto peerId = job->peerId;
			_autoSave.erase(
				ranges::remove(_autoSave, peerId, &AutoSaveRule::peerId),
				end(_autoSave));
		}
	}
	_jobs.erase(
		ranges::remove(_jobs, jobId, &Job::id),
		end(_jobs));
	changed(true);
}

void Manager::clearFinished() {
	_jobs.erase(ranges::remove_if(_jobs, [](const auto &job) {
		return !job->autoSave
			&& (job->state == JobState::Finished
				|| job->state == JobState::Cancelled);
	}), end(_jobs));
	changed(true);
}

void Manager::stopActive(not_null<Job*> job) {
	const auto jobId = job->id;
	auto documents = std::vector<not_null<DocumentData*>>();
	for (const auto &[document, ref] : _activeDocuments) {
		if (ref.jobId == jobId) {
			documents.push_back(document);
		}
	}
	for (const auto document : documents) {
		const auto ref = _activeDocuments.take(document);
		ManagedDocuments().remove(document);
		if (const auto entry = this->entry(*ref)) {
			entry->state = EntryState::Queued;
		}
		document->cancel(); // removes the partially downloaded file
	}
	for (auto i = begin(_activePhotos); i != end(_activePhotos);) {
		if (i->ref.jobId == jobId) {
			if (const auto entry = this->entry(i->ref)) {
				entry->state = EntryState::Queued;
			}
			i = _activePhotos.erase(i);
		} else {
			++i;
		}
	}
	job->nextIndex = 0;
}

JobProgress Manager::progress(not_null<const Job*> job) const {
	auto result = JobProgress();
	result.total = int(job->entries.size());
	for (const auto &entry : job->entries) {
		const auto size = std::max(entry.size, int64(0));
		result.bytesTotal += size;
		switch (entry.state) {
		case EntryState::Queued: ++result.queued; break;
		case EntryState::Active: ++result.active; break;
		case EntryState::Done:
			++result.done;
			result.bytesDone += size;
			break;
		case EntryState::Skipped:
			++result.skipped;
			result.bytesDone += size;
			break;
		case EntryState::Failed:
			++result.failed;
			result.bytesDone += size;
			break;
		}
	}
	for (const auto &[document, ref] : _activeDocuments) {
		if (ref.jobId == job->id) {
			result.bytesDone += std::max(document->loadOffset(), int64(0));
		}
	}
	return result;
}

int64 Manager::jobSpeed(uint64 jobId) const {
	const auto i = _jobSpeeds.find(jobId);
	return (i != end(_jobSpeeds)) ? i->second : 0;
}

SpeedInfo Manager::speed() const {
	const auto stats = _session->downloader().stats();
	auto queued = 0;
	for (const auto &job : _jobs) {
		if (job->state != JobState::Running) {
			continue;
		}
		for (auto i = job->nextIndex; i < int(job->entries.size()); ++i) {
			if (job->entries[i].state == EntryState::Queued) {
				++queued;
			}
		}
	}
	return {
		.bytesPerSecond = _speed,
		.connections = stats.sessions,
		.requests = stats.requestsInFlight,
		.bytesInFlight = stats.bytesInFlight,
		.queuedFiles = queued,
		.premiumDelays = stats.premiumDelays,
		.premium = _session->premium(),
		.scheduleWaiting = _scheduleWaiting,
	};
}

void Manager::updateSpeed() {
	const auto received = _session->downloader().stats().receivedBytes;
	if (_lastReceived >= 0) {
		_speedSamples.push_back(std::max(received - _lastReceived, int64(0)));
		while (_speedSamples.size() > kSpeedSamples) {
			_speedSamples.pop_front();
		}
		auto sum = int64(0);
		for (const auto sample : _speedSamples) {
			sum += sample;
		}
		_speed = _speedSamples.empty()
			? 0
			: (sum * 1000 / (int64(_speedSamples.size()) * kSpeedInterval));
	}
	_lastReceived = received;

	auto anyRunning = false;
	for (const auto &job : _jobs) {
		if (job->state != JobState::Running) {
			_jobSpeeds.remove(job->id);
			_jobBytes.remove(job->id);
			continue;
		}
		anyRunning = true;
		const auto bytes = progress(job.get()).bytesDone;
		const auto i = _jobBytes.find(job->id);
		if (i != end(_jobBytes)) {
			const auto delta = std::max(bytes - i->second, int64(0));
			auto &speed = _jobSpeeds[job->id];
			speed = (speed * 3 + delta * 1000 / kSpeedInterval) / 4;
			i->second = bytes;
		} else {
			_jobBytes.emplace(job->id, bytes);
		}
	}
	if (anyRunning || !_activeDocuments.empty()) {
		_progressChanged.fire({});
	}
	if (!_restoring && !ScheduleAllowsNow() != _scheduleWaiting) {
		pump();
	}
}

void Manager::schedulePump() {
	if (!_pumpTimer.isActive()) {
		_pumpTimer.callOnce(kBusyTimeout);
	}
}

void Manager::pump() {
	if (_restoring) {
		return;
	} else if (_pumping) {
		_pumpAgain = true;
		return;
	}
	_pumping = true;
	do {
		_pumpAgain = false;
		pumpOnce();
	} while (_pumpAgain);
	_pumping = false;
}

void Manager::pumpOnce() {
	const auto now = crl::now();
	const auto scheduleOk = ScheduleAllowsNow();
	auto anyQueued = false;
	auto documentSlots = Current().parallel - int(_activeDocuments.size());
	auto photoSlots = kParallelPhotos - int(_activePhotos.size());
	auto nextRetry = crl::time(0);
	for (auto j = 0; j < int(_jobs.size()); ++j) {
		const auto job = _jobs[j].get();
		if (job->state != JobState::Running) {
			continue;
		}
		auto &entries = job->entries;
		while (job->nextIndex < int(entries.size())
			&& entries[job->nextIndex].state != EntryState::Queued) {
			++job->nextIndex;
		}
		if (job->nextIndex < int(entries.size())) {
			anyQueued = true;
		}
		if (!scheduleOk
			|| (documentSlots <= 0 && photoSlots <= 0)
			|| job->nextIndex >= int(entries.size())) {
			checkJobFinished(job);
			continue;
		} else if (!checkDiskSpace(job)) {
			continue;
		}
		for (auto i = job->nextIndex; i < int(entries.size()); ++i) {
			if (documentSlots <= 0 && photoSlots <= 0) {
				break;
			}
			auto &entry = entries[i];
			if (entry.state != EntryState::Queued) {
				continue;
			} else if (entry.retryAt > now) {
				if (!nextRetry || entry.retryAt < nextRetry) {
					nextRetry = entry.retryAt;
				}
				continue;
			} else if (entry.photo ? (photoSlots <= 0) : (documentSlots <= 0)) {
				continue;
			}
			const auto result = start(job, i);
			if (result == StartResult::Started) {
				if (entry.photo) {
					--photoSlots;
				} else {
					--documentSlots;
				}
			}
			if (job->state != JobState::Running) {
				break;
			}
		}
		checkJobFinished(job);
	}
	_scheduleWaiting = !scheduleOk && anyQueued;
	if (nextRetry) {
		const auto delay = std::max(nextRetry - now, crl::time(500));
		if (!_pumpTimer.isActive() || _pumpTimer.remainingTime() > delay) {
			_pumpTimer.callOnce(delay);
		}
	}
	if (!_activePhotos.empty() && !_photosTimer.isActive()) {
		_photosTimer.callEach(kPhotoCheckInterval);
	} else if (_activePhotos.empty() && _photosTimer.isActive()) {
		_photosTimer.cancel();
	}
}

auto Manager::start(not_null<Job*> job, int index) -> StartResult {
	auto &entry = job->entries[index];
	const auto item = _session->data().message(entry.itemId);
	if (!item) {
		requestItem(job, index);
		return StartResult::Waiting;
	}
	const auto ref = EntryRef{ job->id, index };
	if (!Menu::CanBulkDownload(item)) {
		fail(ref, tr::lng_bulk_error_unavailable(tr::now), false);
		return StartResult::Finished;
	}
	const auto media = item->media();
	if (entry.photo) {
		if (const auto photo = media->photo()) {
			return startPhoto(job, index, item, photo);
		}
	} else if (const auto document = media->document()) {
		return startDocument(job, index, item, document);
	}
	fail(ref, tr::lng_bulk_error_unavailable(tr::now), false);
	return StartResult::Finished;
}

void Manager::requestItem(not_null<Job*> job, int index) {
	auto &entry = job->entries[index];
	const auto itemId = entry.itemId;
	if (_requestingItems.contains(itemId)
		|| int(_requestingItems.size()) >= kMaxItemRequests) {
		return;
	}
	const auto ref = EntryRef{ job->id, index };
	const auto peer = _session->data().peerLoaded(itemId.peer);
	if (!peer) {
		if (++entry.waits > kMaxWaits) {
			fail(ref, tr::lng_bulk_error_chat(tr::now), false);
		} else {
			entry.retryAt = crl::now() + kWaitTimeout;
			schedulePump();
		}
		return;
	}
	_requestingItems.emplace(itemId);
	_session->api().requestMessageData(peer, itemId.msg, crl::guard(this, [=] {
		_requestingItems.remove(itemId);
		const auto entry = this->entry(ref);
		if (!entry || entry->itemId != itemId) {
			return;
		} else if (!_session->data().message(itemId)) {
			fail(ref, tr::lng_bulk_error_deleted(tr::now), false);
		}
		pump();
	}));
}

bool Manager::resolvePath(
		not_null<Job*> job,
		int index,
		not_null<HistoryItem*> item,
		const QString &name) {
	auto &entry = job->entries[index];
	const auto ref = EntryRef{ job->id, index };
	if (!entry.path.isEmpty()) {
		const auto info = QFileInfo(entry.path);
		if (info.exists()
			&& info.isFile()
			&& entry.size > 0
			&& info.size() == entry.size) {
			complete(ref);
			return false;
		} else if (info.exists() && info.isFile()) {
			QFile::remove(entry.path); // incomplete leftover
		}
		_reserved.emplace(entry.path.toLower());
		return true;
	}
	const auto relative = ApplyTemplate(job->options.nameTemplate, {
		.chat = job->options.chatName,
		.type = KindName(KindOf(
			entry.photo ? item->media()->photo() : nullptr,
			entry.photo ? nullptr : item->media()->document())),
		.name = name,
		.date = entry.date,
		.msgId = entry.itemId.msg,
	});
	const auto slash = relative.lastIndexOf('/');
	const auto folder = job->options.folder
		+ ((slash >= 0) ? relative.mid(0, slash + 1) : QString());
	const auto fileName = (slash >= 0) ? relative.mid(slash + 1) : relative;
	const auto target = ResolveTarget(
		folder,
		fileName,
		entry.size,
		job->options.skipExisting,
		_reserved);
	if (target.path.isEmpty()) {
		fail(ref, tr::lng_bulk_error_name(tr::now), false);
		return false;
	}
	entry.path = target.path;
	if (target.skip) {
		addToIndex(entry);
		skip(ref, tr::lng_bulk_error_exists(tr::now));
		return false;
	}
	QDir().mkpath(folder);
	return true;
}

bool Manager::tryUseIndex(
		not_null<Job*> job,
		int index,
		not_null<HistoryItem*> item) {
	const auto &settings = Current();
	if (!settings.skipDownloadedElsewhere
		&& !settings.copyDownloadedElsewhere) {
		return false;
	}
	auto &entry = job->entries[index];
	const auto known = lookupIndex(indexKey(entry));
	if (!known
		|| (!entry.path.isEmpty()
			&& QFileInfo(known->path) == QFileInfo(entry.path))) {
		return false;
	}
	const auto ref = EntryRef{ job->id, index };
	if (settings.skipDownloadedElsewhere) {
		if (entry.path.isEmpty()) {
			entry.path = known->path;
		}
		skip(ref, tr::lng_bulk_error_downloaded(
			tr::now,
			lt_path,
			QDir::toNativeSeparators(known->path)));
		return true;
	}
	const auto name = QFileInfo(known->path).fileName();
	if (!resolvePath(job, index, item, name)) {
		return true;
	}
	if (QFile::copy(known->path, entry.path)) {
		complete(ref);
	} else {
		QFile::remove(entry.path);
		return false;
	}
	return true;
}

auto Manager::startDocument(
	not_null<Job*> job,
	int index,
	not_null<HistoryItem*> item,
	not_null<DocumentData*> document)
-> StartResult {
	auto &entry = job->entries[index];
	const auto ref = EntryRef{ job->id, index };
	if (document->isNull() || document->forbidsFileSave()) {
		fail(ref, tr::lng_bulk_error_unavailable(tr::now), false);
		return StartResult::Finished;
	} else if (_activeDocuments.contains(document)) {
		// The same file in another job, wait for it to finish.
		entry.retryAt = crl::now() + kBusyTimeout;
		schedulePump();
		return StartResult::Busy;
	}
	entry.size = document->size;
	if (tryUseIndex(job, index, item)) {
		return StartResult::Finished;
	} else if (!resolvePath(
			job,
			index,
			item,
			DocumentFileName(document, entry.itemId, entry.date))) {
		return StartResult::Finished;
	}
	entry.state = EntryState::Active;
	entry.error = QString();
	document->save(entry.itemId, entry.path);
	if (!document->loading()) {
		// Copied from the cache or from an already downloaded file.
		finishDocument(ref, document);
		return StartResult::Finished;
	}
	_activeDocuments.emplace(document, ref);
	ManagedDocuments().emplace(document);
	if (!document->loadingFilePath().isEmpty()) {
		Core::App().downloadManager().addLoading({
			.item = item,
			.document = document,
		});
	}
	changed();
	return StartResult::Started;
}

auto Manager::startPhoto(
	not_null<Job*> job,
	int index,
	not_null<HistoryItem*> item,
	not_null<PhotoData*> photo)
-> StartResult {
	auto &entry = job->entries[index];
	const auto ref = EntryRef{ job->id, index };
	const auto itemId = entry.itemId;
	const auto busy = ranges::any_of(
		_activePhotos,
		[&](const ActivePhoto &active) {
			const auto other = this->entry(active.ref);
			return other && (other->itemId == itemId);
		});
	if (busy) {
		entry.retryAt = crl::now() + kBusyTimeout;
		schedulePump();
		return StartResult::Busy;
	}
	if (tryUseIndex(job, index, item)) {
		return StartResult::Finished;
	} else if (!resolvePath(
			job,
			index,
			item,
			PhotoFileName(entry.itemId, entry.date, false))) {
		return StartResult::Finished;
	}
	auto view = photo->createMediaView();
	if (!view) {
		fail(ref, tr::lng_bulk_error_unavailable(tr::now), true);
		return StartResult::Finished;
	}
	view->wanted(Data::PhotoSize::Large, entry.itemId);
	entry.state = EntryState::Active;
	entry.error = QString();
	_activePhotos.push_back({
		.ref = ref,
		.view = std::move(view),
		.requested = crl::now(),
	});
	changed();
	return StartResult::Started;
}

void Manager::documentProgress(not_null<DocumentData*> document) {
	if (document->loading()) {
		return;
	}
	const auto i = _activeDocuments.find(document);
	if (i == end(_activeDocuments)) {
		return;
	}
	const auto ref = i->second;
	_activeDocuments.erase(i);
	ManagedDocuments().remove(document);
	finishDocument(ref, document);
	pump();
}

void Manager::finishDocument(EntryRef ref, not_null<DocumentData*> document) {
	const auto entry = this->entry(ref);
	if (!entry) {
		return;
	}
	if (document->status == FileDownloadFailed) {
		document->status = FileReady;
		QFile::remove(entry->path);
		fail(ref, tr::lng_bulk_error_failed(tr::now), true);
		return;
	} else if (document->cancelled()) {
		skip(ref, tr::lng_bulk_error_cancelled(tr::now));
		return;
	}
	const auto info = QFileInfo(entry->path);
	if (!info.exists() || !info.isFile()) {
		fail(ref, tr::lng_bulk_error_failed(tr::now), true);
		return;
	} else if (entry->size > 0 && info.size() != entry->size) {
		QFile::remove(entry->path);
		fail(ref, tr::lng_bulk_error_size(
			tr::now,
			lt_ready,
			Ui::FormatSizeText(info.size()),
			lt_total,
			Ui::FormatSizeText(entry->size)), true);
		return;
	}
	complete(ref);
}

void Manager::checkPhotos() {
	constexpr auto large = Data::PhotoSize::Large;
	const auto now = crl::now();
	auto finished = std::vector<std::pair<EntryRef, int>>(); // 0 ok, 1 fail
	for (auto i = begin(_activePhotos); i != end(_activePhotos);) {
		const auto entry = this->entry(i->ref);
		if (!entry) {
			i = _activePhotos.erase(i);
			continue;
		}
		const auto item = _session->data().message(entry->itemId);
		const auto photo = (item && item->media())
			? item->media()->photo()
			: nullptr;
		auto done = false;
		if (!photo) {
			finished.emplace_back(i->ref, 1);
			done = true;
		} else if (i->view->loaded()) {
			finished.emplace_back(i->ref, savePhoto(*i) ? 0 : 1);
			done = true;
		} else if (photo->failed(large)) {
			finished.emplace_back(i->ref, 1);
			done = true;
		} else if (!photo->loading(large)
			&& (now - i->requested) >= kPhotoRetryTimeout) {
			if (++i->retries > kPhotoMaxRetries) {
				finished.emplace_back(i->ref, 1);
				done = true;
			} else {
				i->requested = now;
				i->view->wanted(large, entry->itemId);
			}
		}
		if (done) {
			i = _activePhotos.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &[ref, result] : finished) {
		if (result == 0) {
			complete(ref);
		} else if (const auto entry = this->entry(ref)
			; entry && entry->state == EntryState::Active) {
			fail(ref, tr::lng_bulk_error_failed(tr::now), true);
		}
	}
	if (!finished.empty()) {
		pump();
	}
}

bool Manager::savePhoto(const ActivePhoto &active) {
	constexpr auto large = Data::PhotoSize::Large;
	const auto entry = this->entry(active.ref);
	if (!entry || entry->path.isEmpty()) {
		return false;
	}
	auto bytes = active.view->videoContent(large);
	const auto video = !bytes.isEmpty();
	if (video && entry->path.endsWith(u".jpg"_q)) {
		_reserved.remove(entry->path.toLower());
		const auto folder = FolderOf(entry->path);
		auto name = entry->path.mid(folder.size());
		name = name.mid(0, name.size() - 4) + u".mp4"_q;
		const auto target = ResolveTarget(
			folder,
			name,
			-1,
			false,
			_reserved);
		if (target.path.isEmpty()) {
			return false;
		}
		entry->path = target.path;
	}
	if (!video) {
		bytes = active.view->imageBytes(large);
	}
	auto ok = false;
	if (!bytes.isEmpty()) {
		auto file = QFile(entry->path);
		ok = file.open(QIODevice::WriteOnly)
			&& (file.write(bytes) == bytes.size());
		if (ok) {
			entry->size = bytes.size();
		}
	} else {
		ok = active.view->saveToFile(entry->path);
		if (ok) {
			entry->size = QFileInfo(entry->path).size();
		}
	}
	if (!ok) {
		QFile::remove(entry->path);
		return false;
	}
	return true;
}

void Manager::complete(EntryRef ref) {
	const auto job = this->job(ref.jobId);
	const auto entry = this->entry(ref);
	if (!job || !entry) {
		return;
	}
	entry->state = EntryState::Done;
	entry->error = QString();
	const auto &settings = Current();
	if (settings.messageDateAsFileDate) {
		SetFileTimes(entry->path, entry->date);
	}
	if (settings.captionFiles) {
		if (const auto item = _session->data().message(entry->itemId)) {
			WriteCaption(entry->path, item);
		}
	}
	addToIndex(*entry);
	job->lastPath = entry->path;
	changed();
}

void Manager::fail(EntryRef ref, const QString &error, bool retry) {
	const auto entry = this->entry(ref);
	if (!entry) {
		return;
	}
	entry->error = error;
	if (retry && entry->retries < kMaxRetries) {
		entry->retryAt = crl::now() + kRetryDelays[entry->retries];
		++entry->retries;
		entry->state = EntryState::Queued;
		if (const auto job = this->job(ref.jobId)) {
			job->nextIndex = std::min(job->nextIndex, ref.index);
		}
		schedulePump();
	} else {
		entry->state = EntryState::Failed;
	}
	changed();
}

void Manager::skip(EntryRef ref, const QString &error) {
	if (const auto entry = this->entry(ref)) {
		entry->state = EntryState::Skipped;
		entry->error = error;
		changed();
	}
}

void Manager::checkJobFinished(not_null<Job*> job) {
	if (job->state != JobState::Running
		|| job->autoSave
		|| (job->scan.active && !job->scan.finished)) {
		return;
	}
	for (const auto &entry : job->entries) {
		if (!IsTerminal(entry.state)) {
			return;
		}
	}
	job->state = JobState::Finished;
	changed(true);
	if (Current().checksums && !job->checksumsWritten) {
		writeChecksums(job);
	}
	if (!job->notified) {
		job->notified = true;
		const auto progress = this->progress(job);
		showWindowToast(tr::lng_bulk_job_finished(
			tr::now,
			lt_title,
			job->title,
			lt_done,
			QString::number(progress.done),
			lt_skipped,
			QString::number(progress.skipped),
			lt_failed,
			QString::number(progress.failed)), job->lastPath);
	}
}

void Manager::writeChecksums(not_null<Job*> job) {
	job->checksumsWritten = true;
	auto files = std::vector<QString>();
	for (const auto &entry : job->entries) {
		if (entry.state == EntryState::Done && !entry.path.isEmpty()) {
			files.push_back(entry.path);
		}
	}
	if (files.empty()) {
		return;
	}
	const auto folder = job->options.folder;
	crl::async([=] {
		auto lines = QByteArray();
		for (const auto &path : files) {
			auto file = QFile(path);
			if (!file.open(QIODevice::ReadOnly)) {
				continue;
			}
			auto hash = QCryptographicHash(QCryptographicHash::Sha256);
			if (!hash.addData(&file)) {
				continue;
			}
			const auto relative = path.startsWith(folder)
				? path.mid(folder.size())
				: path;
			lines += hash.result().toHex();
			lines += " *";
			lines += relative.toUtf8();
			lines += '\n';
		}
		auto out = QFile(folder + u"SHA256SUMS.txt"_q);
		if (out.open(QIODevice::Append)) {
			out.write(lines);
		}
	});
}

bool Manager::checkDiskSpace(not_null<Job*> job) {
	const auto minimum = int64(Current().minFreeSpaceMB) * 1024 * 1024;
	if (minimum <= 0) {
		return true;
	}
	const auto folder = job->options.folder;
	const auto now = crl::now();
	auto available = int64(-1);
	const auto i = _freeSpace.find(folder);
	if (i != end(_freeSpace) && now - i->second.first < kFreeSpaceCacheTimeout) {
		available = i->second.second;
	} else {
		QDir().mkpath(folder);
		const auto info = QStorageInfo(folder);
		available = info.isValid() ? int64(info.bytesAvailable()) : -1;
		_freeSpace[folder] = { now, available };
	}
	auto next = int64(0);
	for (auto i = job->nextIndex; i < int(job->entries.size()); ++i) {
		if (job->entries[i].state == EntryState::Queued) {
			next = std::max(job->entries[i].size, int64(0));
			break;
		}
	}
	if (available < 0 || available >= minimum + next) {
		return true;
	}
	stopActive(job);
	job->state = JobState::Paused;
	job->pauseReason = PauseReason::LowDisk;
	changed(true);
	if (!_lowDiskNotified) {
		_lowDiskNotified = true;
		showWindowToast(tr::lng_bulk_low_disk(
			tr::now,
			lt_size,
			Ui::FormatSizeText(std::max(available, int64(0)))));
	}
	return false;
}

void Manager::checkLowDiskJobs() {
	auto any = false;
	_freeSpace.clear();
	for (const auto &job : _jobs) {
		if (job->state == JobState::Paused
			&& job->pauseReason == PauseReason::LowDisk) {
			const auto info = QStorageInfo(job->options.folder);
			const auto minimum = int64(Current().minFreeSpaceMB)
				* 1024
				* 1024;
			if (info.isValid()
				&& int64(info.bytesAvailable()) >= minimum * 2) {
				job->state = JobState::Running;
				job->pauseReason = PauseReason::None;
				job->nextIndex = 0;
				any = true;
			}
		}
	}
	if (any) {
		_lowDiskNotified = false;
		changed(true);
		pump();
	}
}

QString Manager::indexKey(const Entry &entry) const {
	return (entry.photo ? u"p"_q : u"d"_q) + QString::number(entry.mediaId);
}

auto Manager::lookupIndex(const QString &key) const
-> std::optional<IndexValue> {
	const auto i = _index.find(key);
	if (i == end(_index)) {
		return std::nullopt;
	}
	const auto info = QFileInfo(i->second.path);
	if (!info.exists()
		|| !info.isFile()
		|| (i->second.size > 0 && info.size() != i->second.size)) {
		return std::nullopt;
	}
	return i->second;
}

void Manager::addToIndex(const Entry &entry) {
	if (!entry.mediaId || entry.path.isEmpty()) {
		return;
	}
	_index[indexKey(entry)] = IndexValue{
		.path = entry.path,
		.size = QFileInfo(entry.path).size(),
	};
	if (!_indexSaveTimer.isActive()) {
		_indexSaveTimer.callOnce(kIndexSaveDelay);
	}
}

bool Manager::downloadedBefore(
		PhotoData *photo,
		DocumentData *document) const {
	if (photo) {
		return lookupIndex(u"p"_q + QString::number(photo->id)).has_value();
	} else if (document) {
		return lookupIndex(u"d"_q + QString::number(document->id))
			|| !document->filepath(true).isEmpty();
	}
	return false;
}

bool Manager::isManaged(not_null<DocumentData*> document) const {
	return _activeDocuments.contains(document);
}

const AutoSaveRule *Manager::autoSaveRule(PeerId peerId) const {
	const auto i = ranges::find(_autoSave, peerId, &AutoSaveRule::peerId);
	return (i != end(_autoSave)) ? &*i : nullptr;
}

void Manager::setAutoSaveRule(AutoSaveRule rule) {
	const auto peerId = rule.peerId;
	_autoSave.erase(
		ranges::remove(_autoSave, peerId, &AutoSaveRule::peerId),
		end(_autoSave));
	if (rule.kinds) {
		if (!rule.folder.endsWith('/')) {
			rule.folder += '/';
		}
		_autoSave.push_back(rule);
		const auto job = autoSaveJob(rule);
		job->options.folder = rule.folder;
		job->title = tr::lng_bulk_autosave_title(
			tr::now,
			lt_chat,
			rule.title);
	} else {
		for (const auto &job : _jobs) {
			if (job->autoSave && job->peerId == peerId) {
				job->autoSave = false;
				job->state = JobState::Running;
				checkJobFinished(job.get());
			}
		}
	}
	changed(true);
}

not_null<Job*> Manager::autoSaveJob(const AutoSaveRule &rule) {
	for (const auto &job : _jobs) {
		if (job->autoSave && job->peerId == rule.peerId) {
			return job.get();
		}
	}
	auto job = std::make_unique<Job>();
	job->id = ++_lastJobId;
	job->title = tr::lng_bulk_autosave_title(tr::now, lt_chat, rule.title);
	job->peerId = rule.peerId;
	job->options.folder = rule.folder;
	job->options.chatName = rule.title;
	job->options.nameTemplate = Current().nameTemplate;
	job->options.skipExisting = true;
	job->autoSave = true;
	job->created = base::unixtime::now();
	const auto result = job.get();
	_jobs.push_back(std::move(job));
	return result;
}

void Manager::newItem(not_null<HistoryItem*> item) {
	if (_autoSave.empty() || !IsServerMsgId(item->id)) {
		return;
	}
	const auto rule = autoSaveRule(item->history()->peer->id);
	if (!rule) {
		return;
	}
	auto filters = Filters();
	filters.kinds = rule->kinds;
	if (!EntryMatches(filters, item, this)) {
		return;
	} else if (auto entry = EntryFromItem(item)) {
		const auto job = autoSaveJob(*rule);
		if (job->state == JobState::Finished
			|| job->state == JobState::Cancelled) {
			job->state = JobState::Running;
		}
		job->entries.push_back(std::move(*entry));
		changed(true);
		pump();
	}
}

void Manager::showWindowToast(const QString &text, const QString &path) {
	const auto window = _session->tryResolveWindow();
	if (!window) {
		return;
	}
	auto marked = TextWithEntities{ text };
	if (!path.isEmpty()) {
		marked.append('\n').append(tr::link(
			tr::lng_bulk_download_open_folder(tr::now),
			u"internal:bulk_open_folder"_q));
	}
	window->showToast({
		.text = std::move(marked),
		.filter = [=](const auto &...) {
			if (!path.isEmpty()) {
				File::ShowInFolder(path);
			}
			return false;
		},
		.duration = crl::time(8000),
	});
}

void Manager::changed(bool structure) {
	if (structure) {
		_structureChanged.fire({});
	}
	saveLater();
}

void Manager::saveLater() {
	if (_saveTimer.isActive()) {
		return;
	}
	// Big lists take a while to write, don't do that too often.
	auto entries = 0;
	for (const auto &job : _jobs) {
		entries += int(job->entries.size());
	}
	_saveTimer.callOnce((entries > kLargeStateEntries)
		? kSaveDelayLarge
		: kSaveDelay);
}

QString Manager::jobsPath() const {
	return StateFolder()
		+ u"jobs_%1.json"_q.arg(_session->userId().bare);
}

QString Manager::indexPath() const {
	return StateFolder()
		+ u"index_%1.json"_q.arg(_session->userId().bare);
}

void Manager::saveNow() {
	_saveTimer.cancel();

	// Keep only the latest finished jobs.
	auto finished = 0;
	for (auto i = _jobs.size(); i != 0;) {
		const auto &job = _jobs[--i];
		if (!job->autoSave
			&& (job->state == JobState::Finished
				|| job->state == JobState::Cancelled)
			&& ++finished > kKeepFinishedJobs) {
			_jobs.erase(_jobs.begin() + i);
		}
	}

	auto jobs = QJsonArray();
	for (const auto &job : _jobs) {
		auto object = QJsonObject();
		object.insert(u"id"_q, Str(job->id));
		object.insert(u"title"_q, job->title);
		object.insert(u"peer"_q, Str(job->peerId.value));
		object.insert(u"folder"_q, job->options.folder);
		object.insert(u"chat"_q, job->options.chatName);
		object.insert(u"template"_q, job->options.nameTemplate);
		object.insert(u"skipExisting"_q, job->options.skipExisting);
		object.insert(u"state"_q, int(job->state));
		object.insert(u"pause"_q, int(job->pauseReason));
		object.insert(u"autoSave"_q, job->autoSave);
		object.insert(u"created"_q, qint64(job->created));
		object.insert(u"lastPath"_q, job->lastPath);
		object.insert(u"checksums"_q, job->checksumsWritten);
		object.insert(u"notified"_q, job->notified);
		auto scan = QJsonObject();
		scan.insert(u"peer"_q, Str(job->scan.peerId.value));
		scan.insert(u"topic"_q, qint64(job->scan.topicRootId.bare));
		scan.insert(u"type"_q, job->scan.type);
		scan.insert(u"offset"_q, qint64(job->scan.offsetId.bare));
		scan.insert(u"found"_q, job->scan.found);
		scan.insert(u"checked"_q, job->scan.checked);
		scan.insert(u"active"_q, job->scan.active);
		scan.insert(u"finished"_q, job->scan.finished);
		scan.insert(u"error"_q, job->scan.error);
		scan.insert(u"filters"_q, SerializeFilters(job->scan.filters));
		object.insert(u"scan"_q, scan);
		auto entries = QJsonArray();
		for (const auto &entry : job->entries) {
			auto value = QJsonArray();
			value.push_back(Str(entry.itemId.peer.value));
			value.push_back(qint64(entry.itemId.msg.bare));
			value.push_back(Str(entry.mediaId));
			value.push_back(entry.photo ? 1 : 0);
			value.push_back(qint64(entry.date));
			value.push_back(qint64(entry.size));
			value.push_back(int(entry.state == EntryState::Active
				? EntryState::Queued
				: entry.state));
			value.push_back(entry.retries);
			value.push_back(entry.path);
			value.push_back(entry.error);
			entries.push_back(value);
		}
		object.insert(u"entries"_q, entries);
		jobs.push_back(object);
	}
	auto rules = QJsonArray();
	for (const auto &rule : _autoSave) {
		auto object = QJsonObject();
		object.insert(u"peer"_q, Str(rule.peerId.value));
		object.insert(u"title"_q, rule.title);
		object.insert(u"folder"_q, rule.folder);
		object.insert(u"kinds"_q, qint64(rule.kinds));
		rules.push_back(object);
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, kStateVersion);
	root.insert(u"lastJobId"_q, Str(_lastJobId));
	root.insert(u"jobs"_q, jobs);
	root.insert(u"autoSave"_q, rules);

	auto file = QSaveFile(jobsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

void Manager::saveIndexNow() {
	_indexSaveTimer.cancel();
	auto root = QJsonObject();
	for (const auto &[key, value] : _index) {
		auto pair = QJsonArray();
		pair.push_back(value.path);
		pair.push_back(qint64(value.size));
		root.insert(key, pair);
	}
	auto file = QSaveFile(indexPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

void Manager::restore() {
	{
		auto file = QFile(indexPath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto document = QJsonDocument::fromJson(file.readAll());
			const auto root = document.object();
			for (auto i = root.begin(); i != root.end(); ++i) {
				const auto pair = i.value().toArray();
				if (pair.size() >= 2) {
					_index.emplace(i.key(), IndexValue{
						.path = pair[0].toString(),
						.size = pair[1].toInteger(),
					});
				}
			}
		}
	}
	auto file = QFile(jobsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	const auto root = document.object();
	if (root.value(u"version"_q).toInt() != kStateVersion) {
		return;
	}
	_lastJobId = ReadU64(root.value(u"lastJobId"_q));
	for (const auto &value : root.value(u"autoSave"_q).toArray()) {
		const auto object = value.toObject();
		auto rule = AutoSaveRule{
			.peerId = PeerId(ReadU64(object.value(u"peer"_q))),
			.title = object.value(u"title"_q).toString(),
			.folder = object.value(u"folder"_q).toString(),
			.kinds = uint32(object.value(u"kinds"_q).toInteger()),
		};
		if (rule.peerId && rule.kinds && !rule.folder.isEmpty()) {
			_autoSave.push_back(std::move(rule));
		}
	}
	for (const auto &value : root.value(u"jobs"_q).toArray()) {
		const auto object = value.toObject();
		auto job = std::make_unique<Job>();
		job->id = ReadU64(object.value(u"id"_q));
		job->title = object.value(u"title"_q).toString();
		job->peerId = PeerId(ReadU64(object.value(u"peer"_q)));
		job->options.folder = object.value(u"folder"_q).toString();
		job->options.chatName = object.value(u"chat"_q).toString();
		job->options.nameTemplate = object.value(u"template"_q).toString();
		job->options.skipExisting = object.value(u"skipExisting"_q).toBool(
			true);
		job->state = JobState(std::clamp(
			object.value(u"state"_q).toInt(),
			0,
			int(JobState::Cancelled)));
		job->pauseReason = PauseReason(std::clamp(
			object.value(u"pause"_q).toInt(),
			0,
			int(PauseReason::LowDisk)));
		job->autoSave = object.value(u"autoSave"_q).toBool();
		job->created = TimeId(object.value(u"created"_q).toInteger());
		job->lastPath = object.value(u"lastPath"_q).toString();
		job->checksumsWritten = object.value(u"checksums"_q).toBool();
		job->notified = object.value(u"notified"_q).toBool();
		const auto scan = object.value(u"scan"_q).toObject();
		job->scan.peerId = PeerId(ReadU64(scan.value(u"peer"_q)));
		job->scan.topicRootId = MsgId(scan.value(u"topic"_q).toInteger());
		job->scan.type = scan.value(u"type"_q).toInt();
		job->scan.offsetId = MsgId(scan.value(u"offset"_q).toInteger());
		job->scan.found = scan.value(u"found"_q).toInt();
		job->scan.checked = scan.value(u"checked"_q).toInt();
		job->scan.active = scan.value(u"active"_q).toBool();
		job->scan.finished = scan.value(u"finished"_q).toBool();
		job->scan.error = scan.value(u"error"_q).toString();
		job->scan.filters = DeserializeFilters(
			scan.value(u"filters"_q).toObject());
		if (!job->options.folder.isEmpty()
			&& !job->options.folder.endsWith('/')) {
			job->options.folder += '/';
		}
		for (const auto &value : object.value(u"entries"_q).toArray()) {
			const auto list = value.toArray();
			if (list.size() < 10) {
				continue;
			}
			auto entry = Entry{
				.itemId = FullMsgId(
					PeerId(ReadU64(list[0])),
					MsgId(list[1].toInteger())),
				.mediaId = ReadU64(list[2]),
				.date = TimeId(list[4].toInteger()),
				.size = list[5].toInteger(),
				.path = list[8].toString(),
				.error = list[9].toString(),
				.retries = list[7].toInt(),
				.state = EntryState(std::clamp(
					list[6].toInt(),
					0,
					int(EntryState::Failed))),
				.photo = (list[3].toInt() != 0),
			};
			if (entry.state == EntryState::Active) {
				entry.state = EntryState::Queued;
			}
			if (entry.state == EntryState::Queued && !entry.path.isEmpty()) {
				_reserved.emplace(entry.path.toLower());
			}
			job->entries.push_back(std::move(entry));
		}
		if (job->id) {
			_lastJobId = std::max(_lastJobId, job->id);
			_jobs.push_back(std::move(job));
		}
	}
	for (const auto &rule : _autoSave) {
		const auto exists = ranges::any_of(_jobs, [&](const auto &job) {
			return job->autoSave && job->peerId == rule.peerId;
		});
		if (!exists) {
			autoSaveJob(rule);
		}
	}
}

Manager &Get(not_null<Main::Session*> session) {
	auto &map = Managers();
	const auto i = map.find(session);
	if (i != end(map)) {
		return *i->second;
	}
	const auto result = map.emplace(
		session,
		std::make_unique<Manager>(session)).first->second.get();
	session->lifetime().add([=] {
		Managers().remove(session);
	});
	return *result;
}

Manager *Find(not_null<Main::Session*> session) {
	auto &map = Managers();
	const auto i = map.find(session);
	return (i != end(map)) ? i->second.get() : nullptr;
}

bool IsManagedDownload(not_null<DocumentData*> document) {
	return ManagedDocuments().contains(document);
}

void OnSessionWindow(not_null<Window::SessionController*> window) {
	if (!window->isPrimary()) {
		return;
	}
	const auto session = &window->session();
	const auto &manager = Get(session);
	const auto running = ranges::count_if(manager.jobs(), [](const auto &job) {
		return !job->autoSave && job->state == JobState::Running;
	});
	if (running > 0) {
		window->showToast(tr::lng_bulk_resuming(
			tr::now,
			lt_count,
			int(running)));
	}
	CheckUnfinishedUploads(window);
	CheckForUpdates(window);
	static auto cleaned = false;
	if (!cleaned) {
		cleaned = true;
		CleanStrippedCopies();
	}
}

} // namespace Bulk
