/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk/bulk_upload.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "bulk/bulk_settings.h"
#include "core/file_utilities.h"
#include "data/data_chat_participant_status.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/chat/attach/attach_send_files_way.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Bulk {
namespace {

constexpr auto kCheckInterval = crl::time(2000);
constexpr auto kNothingTimeout = crl::time(120000);
constexpr auto kNextGroupDelay = crl::time(1000);
constexpr auto kPeerWaitDelay = crl::time(5000);
constexpr auto kMaxPeerWaits = 12;

struct UploadState {
	PeerId peerId = 0;
	QString chat;
	QString folder;
	QStringList files;
	int next = 0; // first file not sent yet
	int skipped = 0;
	int failed = 0;
	bool asPhotos = true;
};

[[nodiscard]] QString StatePath(not_null<Main::Session*> session) {
	return StateFolder()
		+ u"upload_%1.json"_q.arg(session->userId().bare);
}

void SaveState(not_null<Main::Session*> session, const UploadState &state) {
	auto object = QJsonObject();
	object.insert(u"peer"_q, QString::number(state.peerId.value));
	object.insert(u"chat"_q, state.chat);
	object.insert(u"folder"_q, state.folder);
	object.insert(u"files"_q, QJsonArray::fromStringList(state.files));
	object.insert(u"next"_q, state.next);
	object.insert(u"skipped"_q, state.skipped);
	object.insert(u"failed"_q, state.failed);
	object.insert(u"asPhotos"_q, state.asPhotos);
	auto file = QSaveFile(StatePath(session));
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] std::optional<UploadState> LoadState(
		not_null<Main::Session*> session) {
	auto file = QFile(StatePath(session));
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	auto result = UploadState();
	result.peerId = PeerId(object.value(u"peer"_q).toString().toULongLong());
	result.chat = object.value(u"chat"_q).toString();
	result.folder = object.value(u"folder"_q).toString();
	for (const auto &value : object.value(u"files"_q).toArray()) {
		result.files.push_back(value.toString());
	}
	result.next = object.value(u"next"_q).toInt();
	result.skipped = object.value(u"skipped"_q).toInt();
	result.failed = object.value(u"failed"_q).toInt();
	result.asPhotos = object.value(u"asPhotos"_q).toBool(true);
	if (!result.peerId
		|| result.files.isEmpty()
		|| result.next >= result.files.size()) {
		return std::nullopt;
	}
	return result;
}

void RemoveState(not_null<Main::Session*> session) {
	QFile::remove(StatePath(session));
}

[[nodiscard]] QStringList CollectFiles(
		const QString &folder,
		bool subfolders) {
	auto result = QStringList();
	auto iterator = QDirIterator(
		folder,
		QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
		subfolders
			? QDirIterator::Subdirectories
			: QDirIterator::NoIteratorFlags);
	static const auto kSkip = QStringList{
		u"thumbs.db"_q,
		u"desktop.ini"_q,
		u".ds_store"_q,
		u"sha256sums.txt"_q,
	};
	while (iterator.hasNext()) {
		const auto path = iterator.next();
		const auto info = iterator.fileInfo();
		if (info.isHidden()
			|| info.size() <= 0
			|| kSkip.contains(info.fileName().toLower())) {
			continue;
		}
		result.push_back(path);
	}
	result.sort(Qt::CaseInsensitive);
	return result;
}

class Uploader final : public base::has_weak_ptr {
public:
	Uploader(not_null<Main::Session*> session, UploadState state);

	void start();
	[[nodiscard]] UploadProgress progress() const;

private:
	void sendNext();
	void check();
	void finish();

	const not_null<Main::Session*> _session;
	UploadState _state;
	base::flat_set<FullMsgId> _pending;
	base::Timer _timer;
	crl::time _sentAt = 0;
	int _groupCount = 0;
	int _expected = 0;
	int _seen = 0;
	int _peerWaits = 0;
	bool _waiting = false;
	rpl::lifetime _lifetime;

};

[[nodiscard]] auto Uploaders()
-> base::flat_map<not_null<Main::Session*>, std::unique_ptr<Uploader>> & {
	static auto result = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<Uploader>>();
	return result;
}

void Forget(not_null<Main::Session*> session) {
	crl::on_main([=] {
		Uploaders().remove(session);
	});
}

void Launch(not_null<Main::Session*> session, UploadState state) {
	auto &map = Uploaders();
	if (map.contains(session)) {
		return;
	}
	SaveState(session, state);
	const auto raw = map.emplace(
		session,
		std::make_unique<Uploader>(session, std::move(state))
	).first->second.get();
	session->lifetime().add([=] {
		Uploaders().remove(session);
	});
	raw->start();
}

Uploader::Uploader(not_null<Main::Session*> session, UploadState state)
: _session(session)
, _state(std::move(state))
, _timer([=] { check(); }) {
}

void Uploader::start() {
	_session->data().newItemAdded(
	) | rpl::filter([=](not_null<HistoryItem*> item) {
		return _waiting
			&& (item->history()->peer->id == _state.peerId)
			&& item->out()
			&& item->isSending()
			&& (item->media() != nullptr);
	}) | rpl::on_next([=](not_null<HistoryItem*> item) {
		_pending.emplace(item->fullId());
		++_seen;
	}, _lifetime);

	_session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		const auto old = FullMsgId(change.newId.peer, change.oldId);
		if (_pending.remove(old)) {
			check();
		}
	}, _lifetime);

	_session->data().itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		if (_pending.remove(item->fullId())) {
			check();
		}
	}, _lifetime);

	_timer.callEach(kCheckInterval);
	sendNext();
}

UploadProgress Uploader::progress() const {
	return {
		.chat = _state.chat,
		.folder = _state.folder,
		.sent = std::min(int(_state.files.size()), _state.next),
		.total = int(_state.files.size()),
		.skipped = _state.skipped,
		.active = true,
	};
}

void Uploader::sendNext() {
	if (_state.next >= _state.files.size()) {
		finish();
		return;
	}
	const auto peer = _session->data().peerLoaded(_state.peerId);
	if (!peer && ++_peerWaits <= kMaxPeerWaits) {
		// The chats list may still be loading after a restart.
		base::call_delayed(kPeerWaitDelay, this, [=] { sendNext(); });
		return;
	} else if (!peer || !CanUploadTo(peer)) {
		if (const auto window = _session->tryResolveWindow()) {
			window->showToast(tr::lng_bulk_upload_cant(tr::now));
		}
		_timer.cancel();
		Forget(_session); // keeps the state file to continue later
		return;
	}
	const auto chunk = _state.files.mid(_state.next, Ui::MaxAlbumItems());
	auto skippedNow = 0;
	auto list = Storage::PrepareMediaList(
		chunk,
		st::sendMediaPreviewSize,
		_session->premium(),
		[&](const Ui::PreparedList &) { ++skippedNow; });
	_state.skipped += skippedNow;
	_groupCount = int(chunk.size());
	if (list.files.empty()) {
		_state.next += _groupCount;
		SaveState(_session, _state);
		crl::on_main(this, [=] { sendNext(); });
		return;
	}
	auto way = Ui::SendFilesWay();
	way.setGroupFiles(true);
	way.setSendImagesAsPhotos(_state.asPhotos);
	const auto type = _state.asPhotos
		? SendMediaType::Photo
		: SendMediaType::File;
	auto action = Api::SendAction(_session->data().history(peer));
	action.clearDraft = false;
	auto groups = Ui::DivideByGroups(
		std::move(list),
		way,
		peer->slowmodeApplied());
	_expected = 0;
	for (const auto &group : groups) {
		_expected += int(group.list.files.size());
	}
	_seen = 0;
	_pending.clear();
	_waiting = true;
	_sentAt = crl::now();
	for (auto &group : groups) {
		const auto album = (group.type != Ui::AlbumType::None)
			? std::make_shared<SendingAlbum>()
			: nullptr;
		_session->api().sendFiles(
			std::move(group.list),
			type,
			album,
			action);
	}
}

void Uploader::check() {
	if (!_waiting) {
		return;
	}
	for (auto i = begin(_pending); i != end(_pending);) {
		const auto item = _session->data().message(*i);
		if (!item || !item->isSending()) {
			i = _pending.erase(i);
		} else if (item->hasFailed()) {
			++_state.failed;
			i = _pending.erase(i);
		} else {
			++i;
		}
	}
	const auto allSeen = (_seen >= _expected);
	const auto timedOut = !allSeen
		&& (crl::now() - _sentAt > kNothingTimeout)
		&& !_session->uploader().currentUploadId();
	if (_pending.empty() && (allSeen || timedOut)) {
		_waiting = false;
		_state.next += _groupCount;
		SaveState(_session, _state);
		base::call_delayed(kNextGroupDelay, this, [=] { sendNext(); });
	}
}

void Uploader::finish() {
	_timer.cancel();
	RemoveState(_session);
	if (const auto window = _session->tryResolveWindow()) {
		window->showToast(tr::lng_bulk_upload_done(
			tr::now,
			lt_count,
			int(_state.files.size()) - _state.skipped,
			lt_chat,
			_state.chat,
			lt_skipped,
			QString::number(_state.skipped + _state.failed)));
	}
	Forget(_session);
}

void UploadFolderBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer) {
	const auto session = &window->session();
	box->setTitle(tr::lng_bulk_upload_title());
	box->setWidth(st::boxWideWidth);

	struct State {
		QString folder;
		QStringList files;
		int64 size = 0;
		rpl::variable<QString> about;
	};
	const auto state = box->lifetime().make_state<State>();

	const auto subfolders = box->addRow(object_ptr<Ui::Checkbox>(
		box,
		tr::lng_bulk_upload_subfolders(tr::now),
		false,
		st::defaultBoxCheckbox));
	box->addSkip(st::boxLittleSkip);
	const auto asPhotos = box->addRow(object_ptr<Ui::Checkbox>(
		box,
		tr::lng_bulk_upload_compress(tr::now),
		true,
		st::defaultBoxCheckbox));
	box->addSkip(st::boxLittleSkip);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		state->about.value(),
		st::boxLabel));
	box->addSkip(st::boxLittleSkip);

	const auto refresh = [=] {
		if (state->folder.isEmpty()) {
			state->about = tr::lng_bulk_upload_choose_about(tr::now);
			return;
		}
		state->files = CollectFiles(state->folder, subfolders->checked());
		state->size = 0;
		for (const auto &path : state->files) {
			state->size += QFileInfo(path).size();
		}
		const auto count = int(state->files.size());
		const auto albums = (count + Ui::MaxAlbumItems() - 1)
			/ Ui::MaxAlbumItems();
		state->about = tr::lng_bulk_upload_about(
			tr::now,
			lt_count,
			count,
			lt_path,
			QDir::toNativeSeparators(state->folder),
			lt_size,
			Ui::FormatSizeText(state->size),
			lt_albums,
			QString::number(albums));
	};
	subfolders->checkedChanges(
	) | rpl::on_next([=] {
		refresh();
	}, subfolders->lifetime());

	const auto choose = box->addRow(object_ptr<Ui::LinkButton>(
		box,
		tr::lng_bulk_upload_choose(tr::now)));
	choose->setClickedCallback([=] {
		FileDialog::GetFolder(
			box.get(),
			tr::lng_bulk_upload_choose(tr::now),
			state->folder,
			crl::guard(box, [=](QString &&result) {
				if (!result.isEmpty()) {
					state->folder = result;
					refresh();
				}
			}));
	});
	refresh();

	const auto weak = base::make_weak(window);
	box->addButton(tr::lng_bulk_upload_start(), [=] {
		if (state->files.isEmpty()) {
			return;
		} else if (Uploaders().contains(session)) {
			box->showToast(tr::lng_bulk_upload_busy(tr::now));
			return;
		}
		auto upload = UploadState{
			.peerId = peer->id,
			.chat = peer->name(),
			.folder = state->folder,
			.files = state->files,
			.asPhotos = asPhotos->checked(),
		};
		box->closeBox();
		Launch(session, std::move(upload));
		if (const auto strong = weak.get()) {
			strong->showToast(tr::lng_bulk_upload_started(tr::now));
		}
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

} // namespace

bool CanUploadTo(not_null<PeerData*> peer) {
	return !peer->slowmodeApplied()
		&& Data::CanSendAnyOf(peer, Data::FilesSendRestrictions());
}

void ShowUploadFolderBox(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer) {
	if (!CanUploadTo(peer)) {
		window->showToast(tr::lng_bulk_upload_cant(tr::now));
		return;
	}
	window->show(Box(UploadFolderBox, window, peer));
}

void CheckUnfinishedUploads(not_null<Window::SessionController*> window) {
	const auto session = &window->session();
	static auto checked = base::flat_set<uint64>();
	if (!checked.emplace(session->uniqueId()).second
		|| Uploaders().contains(session)) {
		return;
	}
	const auto state = LoadState(session);
	if (!state) {
		RemoveState(session);
		return;
	}
	const auto copy = *state;
	window->show(Ui::MakeConfirmBox({
		.text = tr::lng_bulk_upload_unfinished(
			tr::now,
			lt_path,
			QDir::toNativeSeparators(copy.folder),
			lt_chat,
			copy.chat,
			lt_sent,
			QString::number(copy.next),
			lt_total,
			QString::number(copy.files.size())),
		.confirmed = [=](Fn<void()> close) {
			close();
			Launch(session, copy);
		},
		.cancelled = [=](Fn<void()> close) {
			RemoveState(session);
			close();
		},
		.confirmText = tr::lng_bulk_upload_continue(),
		.cancelText = tr::lng_bulk_upload_discard(),
		.strictCancel = true,
	}));
}

std::optional<UploadProgress> CurrentUpload(
		not_null<Main::Session*> session) {
	const auto i = Uploaders().find(session);
	if (i != end(Uploaders())) {
		return i->second->progress();
	} else if (const auto state = LoadState(session)) {
		return UploadProgress{
			.chat = state->chat,
			.folder = state->folder,
			.sent = state->next,
			.total = int(state->files.size()),
			.skipped = state->skipped,
			.paused = true,
		};
	}
	return std::nullopt;
}

void StopUpload(not_null<Main::Session*> session) {
	Uploaders().remove(session);
	RemoveState(session);
}

void ResumeUpload(not_null<Main::Session*> session) {
	if (Uploaders().contains(session)) {
		return;
	} else if (auto state = LoadState(session)) {
		Launch(session, std::move(*state));
	}
}

} // namespace Bulk
