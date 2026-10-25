/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include "base/base_file_utilities.h"
#include "base/options.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_download_manager.h"
#include "data/data_file_click_handler.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h" // HistoryView::SelectedItem.
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "storage/storage_account.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Menu {
namespace {

constexpr auto kParallelPhotos = 8; // decoded photos are kept in memory
constexpr auto kParallelDocuments = 4; // each file gets the full speed

constexpr auto kPhotoCheckInterval = crl::time(1000);
constexpr auto kPhotoRetryTimeout = crl::time(4000);
constexpr auto kPhotoMaxRetries = 3;
constexpr auto kMaxNameAttempts = 10000;
constexpr auto kMaxFolderNameLength = 80;
constexpr auto kResultToastDuration = crl::time(8000);

base::options::toggle BulkDownloadSubfolder({
	.id = "bulk-download-subfolder",
	.name = "Bulk download: subfolder per chat",
	.description = "Save bulk downloads into a folder named after the chat.",
	.defaultValue = true,
});

base::options::toggle BulkDownloadSkipExisting({
	.id = "bulk-download-skip-existing",
	.name = "Bulk download: skip existing files",
	.description = "Don't download files that are already in the folder.",
	.defaultValue = true,
});

struct Entry {
	FullMsgId itemId;
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
	TimeId date = 0;
	int64 size = 0;
};

struct Collected {
	std::vector<Entry> entries;
	PeerData *peer = nullptr; // if all the messages are from one chat
	int64 totalSize = 0;
	int skipped = 0;
};

struct Target {
	QString path;
	bool skip = false;
};

[[nodiscard]] Collected Collect(
		not_null<Main::Session*> session,
		const MessageIdsList &ids) {
	auto result = Collected();
	auto photos = base::flat_set<not_null<PhotoData*>>();
	auto documents = base::flat_set<not_null<DocumentData*>>();
	auto multiplePeers = false;
	for (const auto &id : ids) {
		const auto item = session->data().message(id);
		if (!item || !CanBulkDownload(item)) {
			++result.skipped;
			continue;
		}
		const auto media = item->media();
		auto entry = Entry{ .itemId = item->fullId(), .date = item->date() };
		if (const auto photo = media->photo()) {
			if (!photos.emplace(photo).second) {
				continue;
			}
			entry.photo = photo;
			entry.size = photo->imageByteSize(Data::PhotoSize::Large);
		} else if (const auto document = media->document()) {
			if (!documents.emplace(document).second) {
				continue;
			}
			entry.document = document;
			entry.size = document->size;
		} else {
			++result.skipped;
			continue;
		}
		const auto peer = item->history()->peer.get();
		if (!multiplePeers) {
			if (!result.peer) {
				result.peer = peer;
			} else if (result.peer != peer) {
				result.peer = nullptr;
				multiplePeers = true;
			}
		}
		result.totalSize += std::max(entry.size, int64(0));
		result.entries.push_back(entry);
	}
	ranges::sort(result.entries, std::less<>(), &Entry::itemId);
	return result;
}

[[nodiscard]] QString WithSlash(QString path) {
	if (!path.isEmpty() && !path.endsWith('/')) {
		path += '/';
	}
	return path;
}

[[nodiscard]] QString DefaultFolder(not_null<Main::Session*> session) {
	const auto path = Core::App().settings().downloadPath();
	return WithSlash(path.isEmpty()
		? File::DefaultDownloadPath(session)
		: (path == FileDialog::Tmp())
		? session->local().tempDirectory()
		: path);
}

[[nodiscard]] QString FolderNameForPeer(not_null<PeerData*> peer) {
	auto name = peer->name().trimmed();
	if (name.size() > kMaxFolderNameLength) {
		name = name.mid(0, kMaxFolderNameLength).trimmed();
	}
	if (name.isEmpty()) {
		name = u"Chat %1"_q.arg(peer->id.value);
	}
	return base::FileNameFromUserString(name);
}

[[nodiscard]] QString MaskDangerousExtension(QString name) {
#ifdef Q_OS_WIN
	const auto lower = name.trimmed().toLower();
	for (const auto &extension : { u".lnk"_q, u".scf"_q }) {
		if (lower.endsWith(extension)) {
			return name + u".download"_q;
		}
	}
#endif // Q_OS_WIN
	return name;
}

[[nodiscard]] QString DateTag(TimeId date) {
	const auto when = date
		? base::unixtime::parse(date)
		: QDateTime::currentDateTime();
	return when.toString(u"yyyy-MM-dd_HH-mm-ss"_q);
}

[[nodiscard]] QString PhotoFileName(const Entry &entry, bool video) {
	return u"photo_%1_%2%3"_q
		.arg(DateTag(entry.date))
		.arg(entry.itemId.msg.bare)
		.arg(video ? u".mp4"_q : u".jpg"_q);
}

[[nodiscard]] QString DocumentFileName(const Entry &entry) {
	const auto document = entry.document;
	const auto original = document->filename().trimmed();
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
		.arg(DateTag(entry.date))
		.arg(entry.itemId.msg.bare)
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

class BulkDownload final : public base::has_weak_ptr {
public:
	BulkDownload(
		not_null<Window::SessionController*> window,
		std::vector<Entry> entries,
		QString folder,
		bool skipExisting);

	[[nodiscard]] not_null<Main::Session*> session() const {
		return _session;
	}
	void start();

private:
	struct ActivePhoto {
		Entry entry;
		std::shared_ptr<Data::PhotoMedia> view;
		crl::time requested = 0;
		int retries = 0;
	};

	void startDocumentsStep();
	void checkDocuments();
	[[nodiscard]] bool startDocument(const Entry &entry);
	void pumpPhotos();
	void checkPhotos();
	[[nodiscard]] bool savePhoto(const ActivePhoto &active);
	void checkFinished();
	void showResult();

	const not_null<Main::Session*> _session;
	const base::weak_ptr<Window::SessionController> _window;
	const QString _folder;
	const bool _skipExisting = false;

	std::vector<Entry> _documents;
	std::deque<Entry> _photos;
	std::vector<ActivePhoto> _activePhotos;
	std::vector<not_null<DocumentData*>> _activeDocuments;
	int _documentsDispatched = 0;
	base::flat_set<QString> _reserved;
	base::Timer _photosTimer;
	QString _lastPath;

	int _photosSaved = 0;
	int _documentsStarted = 0;
	int _skippedExisting = 0;
	int _failed = 0;
	bool _finished = false;

	rpl::lifetime _lifetime;

};

using BulkDownloads = std::vector<std::unique_ptr<BulkDownload>>;

[[nodiscard]] auto ActiveDownloads()
-> base::flat_map<not_null<Main::Session*>, BulkDownloads> & {
	static auto result = base::flat_map<
		not_null<Main::Session*>,
		BulkDownloads>();
	return result;
}

void Launch(std::unique_ptr<BulkDownload> download) {
	const auto session = download->session();
	auto &map = ActiveDownloads();
	auto i = map.find(session);
	if (i == end(map)) {
		i = map.emplace(session, BulkDownloads()).first;
		session->lifetime().add([=] {
			ActiveDownloads().remove(session);
		});
	}
	const auto raw = download.get();
	i->second.push_back(std::move(download));
	raw->start();
}

void Forget(not_null<Main::Session*> session, not_null<BulkDownload*> raw) {
	crl::on_main([=] {
		auto &map = ActiveDownloads();
		const auto i = map.find(session);
		if (i != end(map)) {
			auto &list = i->second;
			list.erase(
				ranges::remove_if(list, [&](const auto &download) {
					return (download.get() == raw.get());
				}),
				end(list));
		}
	});
}

BulkDownload::BulkDownload(
	not_null<Window::SessionController*> window,
	std::vector<Entry> entries,
	QString folder,
	bool skipExisting)
: _session(&window->session())
, _window(base::make_weak(window))
, _folder(WithSlash(std::move(folder)))
, _skipExisting(skipExisting)
, _photosTimer([=] { checkPhotos(); }) {
	for (auto &entry : entries) {
		if (entry.document) {
			_documents.push_back(entry);
		} else if (entry.photo) {
			_photos.push_back(entry);
		}
	}
}

void BulkDownload::start() {
	QDir().mkpath(_folder);

	_session->downloaderTaskFinished(
	) | rpl::on_next([=] {
		checkPhotos();
	}, _lifetime);
	_session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> document) {
		return !document->loading()
			&& ranges::contains(_activeDocuments, document);
	}) | rpl::on_next([=] {
		checkDocuments();
	}, _lifetime);
	_photosTimer.callEach(kPhotoCheckInterval);

	pumpPhotos();
	startDocumentsStep();
}

void BulkDownload::startDocumentsStep() {
	// Few files at a time: each one gets the full speed and finishes
	// sooner, instead of all of them crawling together.
	while (_documentsDispatched < int(_documents.size())
		&& int(_activeDocuments.size()) < kParallelDocuments) {
		const auto &entry = _documents[_documentsDispatched++];
		if (startDocument(entry)) {
			_activeDocuments.push_back(entry.document);
		}
	}
	checkFinished();
}

void BulkDownload::checkDocuments() {
	if (_finished) {
		return;
	}
	const auto was = _activeDocuments.size();
	_activeDocuments.erase(
		ranges::remove_if(_activeDocuments, [](not_null<DocumentData*> d) {
			return !d->loading();
		}),
		end(_activeDocuments));
	if (_activeDocuments.size() != was) {
		startDocumentsStep();
	}
}

bool BulkDownload::startDocument(const Entry &entry) {
	const auto document = entry.document;
	if (document->isNull() || document->forbidsFileSave()) {
		++_failed;
		return false;
	} else if (document->loading()
		&& !document->loadingFilePath().isEmpty()) {
		++_documentsStarted;
		return true;
	}
	const auto target = ResolveTarget(
		_folder,
		DocumentFileName(entry),
		document->size,
		_skipExisting,
		_reserved);
	if (target.path.isEmpty()) {
		++_failed;
		return false;
	} else if (target.skip) {
		++_skippedExisting;
		return false;
	}
	document->save(entry.itemId, target.path);
	_lastPath = target.path;
	++_documentsStarted;
	if (document->loading() && !document->loadingFilePath().isEmpty()) {
		if (const auto item = _session->data().message(entry.itemId)) {
			Core::App().downloadManager().addLoading({
				.item = item,
				.document = document,
			});
		}
		return true;
	}
	return false;
}

void BulkDownload::pumpPhotos() {
	while (!_photos.empty()
		&& int(_activePhotos.size()) < kParallelPhotos) {
		auto entry = _photos.front();
		_photos.pop_front();
		auto view = entry.photo->createMediaView();
		if (!view) {
			++_failed;
			continue;
		}
		view->wanted(Data::PhotoSize::Large, entry.itemId);
		_activePhotos.push_back({
			.entry = entry,
			.view = std::move(view),
			.requested = crl::now(),
		});
	}
}

void BulkDownload::checkPhotos() {
	if (_finished) {
		return;
	}
	constexpr auto large = Data::PhotoSize::Large;
	const auto now = crl::now();
	for (auto i = begin(_activePhotos); i != end(_activePhotos);) {
		const auto photo = i->entry.photo;
		auto done = false;
		if (i->view->loaded()) {
			if (savePhoto(*i)) {
				++_photosSaved;
			}
			done = true;
		} else if (photo->failed(large)) {
			++_failed;
			done = true;
		} else if (!photo->loading(large)
			&& (now - i->requested) >= kPhotoRetryTimeout) {
			if (++i->retries > kPhotoMaxRetries) {
				++_failed;
				done = true;
			} else {
				i->requested = now;
				i->view->wanted(large, i->entry.itemId);
			}
		}
		if (done) {
			i = _activePhotos.erase(i);
		} else {
			++i;
		}
	}
	pumpPhotos();
	checkDocuments();
	checkFinished();
}

bool BulkDownload::savePhoto(const ActivePhoto &active) {
	constexpr auto large = Data::PhotoSize::Large;
	auto bytes = active.view->videoContent(large);
	const auto video = !bytes.isEmpty();
	if (!video) {
		bytes = active.view->imageBytes(large);
	}
	const auto target = ResolveTarget(
		_folder,
		PhotoFileName(active.entry, video),
		bytes.isEmpty() ? int64(-1) : int64(bytes.size()),
		_skipExisting,
		_reserved);
	if (target.path.isEmpty()) {
		++_failed;
		return false;
	} else if (target.skip) {
		++_skippedExisting;
		return false;
	}
	auto ok = false;
	if (!bytes.isEmpty()) {
		auto file = QFile(target.path);
		ok = file.open(QIODevice::WriteOnly)
			&& (file.write(bytes) == bytes.size());
	} else {
		ok = active.view->saveToFile(target.path);
	}
	if (!ok) {
		QFile::remove(target.path);
		++_failed;
		return false;
	}
	SetFileTimes(target.path, active.entry.date);
	_lastPath = target.path;
	return true;
}

void BulkDownload::checkFinished() {
	if (_finished
		|| _documentsDispatched < int(_documents.size())
		|| !_activeDocuments.empty()
		|| !_photos.empty()
		|| !_activePhotos.empty()) {
		return;
	}
	_finished = true;
	_photosTimer.cancel();
	showResult();
	Forget(_session, this); // destroys us later, we may be in a callback
}

void BulkDownload::showResult() {
	const auto window = _window.get();
	if (!window) {
		return;
	}
	auto text = tr::marked(tr::lng_bulk_download_result(
		tr::now,
		lt_photos,
		QString::number(_photosSaved),
		lt_files,
		QString::number(_documentsStarted),
		lt_skipped,
		QString::number(_skippedExisting),
		lt_failed,
		QString::number(_failed)));
	const auto path = _lastPath;
	if (!path.isEmpty()) {
		text.append('\n').append(tr::link(
			tr::lng_bulk_download_open_folder(tr::now),
			u"internal:bulk_download_open_folder"_q));
	}
	window->showToast({
		.text = std::move(text),
		.filter = [=](const auto &...) {
			if (!path.isEmpty()) {
				File::ShowInFolder(path);
			}
			return false;
		},
		.duration = kResultToastDuration,
	});
}

void StartDownload(
		not_null<Window::SessionController*> window,
		std::vector<Entry> entries,
		QString folder,
		bool skipExisting) {
	const auto count = int(entries.size());
	window->showToast(tr::lng_bulk_download_started(
		tr::now,
		lt_count,
		count));
	Launch(std::make_unique<BulkDownload>(
		window,
		std::move(entries),
		std::move(folder),
		skipExisting));
}

void BulkDownloadBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		std::shared_ptr<Collected> collected,
		Fn<void()> started) {
	const auto session = &window->session();
	const auto weak = base::make_weak(window);
	const auto count = int(collected->entries.size());

	box->setTitle(tr::lng_bulk_download_title());
	box->setWidth(st::boxWideWidth);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_bulk_download_about(
				tr::now,
				lt_count,
				count,
				lt_size,
				Ui::FormatSizeText(collected->totalSize)),
			st::boxLabel));
	if (collected->skipped > 0) {
		box->addSkip(st::boxLittleSkip);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_bulk_download_skipped(
					tr::now,
					lt_count,
					collected->skipped),
				st::boxDividerLabel));
	}
	box->addSkip(st::boxLittleSkip);

	const auto folder = box->lifetime().make_state<rpl::variable<QString>>(
		DefaultFolder(session));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			folder->value() | rpl::map([](const QString &path) {
				return tr::lng_bulk_download_folder(
					tr::now,
					lt_path,
					QDir::toNativeSeparators(path));
			}),
			st::boxLabel));
	const auto change = box->addRow(
		object_ptr<Ui::LinkButton>(
			box,
			tr::lng_bulk_download_change(tr::now)));
	change->setClickedCallback([=] {
		const auto current = folder->current();
		FileDialog::GetFolder(
			box.get(),
			tr::lng_download_path_choose(tr::now),
			current.endsWith('/') ? current.mid(0, current.size() - 1) : current,
			[=](QString &&result) {
				if (!result.isEmpty()) {
					*folder = WithSlash(result);
				}
			});
	});
	box->addSkip(st::boxLittleSkip);

	const auto peer = collected->peer;
	const auto subfolderName = peer ? FolderNameForPeer(peer) : QString();
	const auto subfolder = peer
		? box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				tr::lng_bulk_download_subfolder(
					tr::now,
					lt_chat,
					subfolderName),
				BulkDownloadSubfolder.value(),
				st::defaultBoxCheckbox))
		: nullptr;
	if (subfolder) {
		box->addSkip(st::boxLittleSkip);
	}
	const auto skipExisting = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			tr::lng_bulk_download_skip_existing(tr::now),
			BulkDownloadSkipExisting.value(),
			st::defaultBoxCheckbox));

	box->addButton(tr::lng_bulk_download_start(), [=] {
		const auto window = weak.get();
		if (!window) {
			box->closeBox();
			return;
		}
		const auto useSubfolder = subfolder && subfolder->checked();
		const auto skip = skipExisting->checked();
		if (subfolder) {
			BulkDownloadSubfolder.set(useSubfolder);
		}
		BulkDownloadSkipExisting.set(skip);

		auto path = folder->current();
		if (useSubfolder && !subfolderName.isEmpty()) {
			path = WithSlash(path + subfolderName);
		}
		auto entries = std::move(collected->entries);
		box->closeBox();
		StartDownload(window, std::move(entries), path, skip);
		if (started) {
			started();
		}
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

[[nodiscard]] bool OnlyPhotos(
		not_null<Main::Session*> session,
		const MessageIdsList &ids) {
	for (const auto &id : ids) {
		if (const auto item = session->data().message(id)) {
			if (CanBulkDownload(item) && !item->media()->photo()) {
				return false;
			}
		}
	}
	return true;
}

void AddAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		MessageIdsList ids,
		Fn<void()> started) {
	const auto onlyPhotos = OnlyPhotos(&window->session(), ids);
	const auto text = onlyPhotos
		? tr::lng_context_save_images_selected(tr::now)
		: tr::lng_context_save_documents_selected(tr::now);
	const auto icon = onlyPhotos
		? &st::menuIconSaveImage
		: &st::menuIconDownload;
	const auto weak = base::make_weak(window);
	menu->addAction(text, [=] {
		if (const auto strong = weak.get()) {
			DownloadFiles(strong, ids, started);
		}
	}, icon);
}

} // namespace

bool CanBulkDownload(not_null<const HistoryItem*> item) {
	if (item->forbidsSaving()
		|| !item->history()->peer->allowsForwarding()) {
		return false;
	}
	const auto media = item->media();
	if (!media || media->webpage()) {
		return false;
	} else if (media->photo()) {
		return true;
	} else if (const auto document = media->document()) {
		return !document->isNull();
	}
	return false;
}

void DownloadFiles(
		not_null<Window::SessionController*> window,
		const MessageIdsList &ids,
		Fn<void()> started) {
	auto collected = std::make_shared<Collected>(
		Collect(&window->session(), ids));
	if (collected->entries.empty()) {
		window->showToast(tr::lng_bulk_download_nothing(tr::now));
		return;
	}
	window->show(Box(BulkDownloadBox, window, collected, started));
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<HistoryView::SelectedItem> &selectedItems,
		not_null<HistoryView::ListWidget*> list) {
	auto ids = MessageIdsList();
	ids.reserve(selectedItems.size());
	auto any = false;
	for (const auto &selectedItem : selectedItems) {
		const auto item = window->session().data().message(
			selectedItem.msgId);
		if (item && CanBulkDownload(item)) {
			any = true;
		}
		ids.push_back(selectedItem.msgId);
	}
	if (!any) {
		return;
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->cancelSelection();
		}
	};
	AddAction(menu, window, std::move(ids), done);
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<HistoryInner*> list) {
	auto ids = MessageIdsList();
	ids.reserve(items.size());
	auto any = false;
	for (const auto &item : items) {
		if (CanBulkDownload(item)) {
			any = true;
		}
		ids.push_back(item->fullId());
	}
	if (!any) {
		return;
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->clearSelected();
		}
	};
	AddAction(menu, window, std::move(ids), done);
}

} // namespace Menu
