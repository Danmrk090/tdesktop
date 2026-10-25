/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk/bulk_boxes.h"

#include "apiwrap.h"
#include "base/base_file_utilities.h"
#include "base/options.h"
#include "base/unixtime.h"
#include "bulk/bulk_download_manager.h"
#include "bulk/bulk_update_check.h"
#include "bulk/bulk_upload.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "menu/menu_item_download_files.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/storage_account.h"
#include "ui/boxes/calendar_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStorageInfo>

namespace Bulk {
namespace {

constexpr auto kMB = int64(1024 * 1024);
constexpr auto kMaxFailedShown = 300;
constexpr auto kMaxFolderNameLength = 80;

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

[[nodiscard]] QString WithSlash(QString path) {
	if (!path.isEmpty() && !path.endsWith('/')) {
		path += '/';
	}
	return path;
}

[[nodiscard]] QString DefaultFolder(not_null<Main::Session*> session) {
	const auto last = Current().lastFolder;
	if (!last.isEmpty() && QFileInfo(last).isDir()) {
		return WithSlash(last);
	}
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

[[nodiscard]] QString FormatDate(TimeId date) {
	return date
		? QLocale().toString(
			base::unixtime::parse(date).date(),
			QLocale::ShortFormat)
		: tr::lng_bulk_date_any(tr::now);
}

[[nodiscard]] int64 ParseMegabytes(const QString &text) {
	auto ok = false;
	const auto value = text.trimmed().replace(',', '.').toDouble(&ok);
	return (ok && value > 0) ? int64(value * kMB) : 0;
}

[[nodiscard]] QString FormatMegabytes(int64 bytes) {
	return (bytes > 0)
		? QString::number(double(bytes) / kMB, 'f', 0)
		: QString();
}

void AddDivider(not_null<Ui::GenericBox*> box) {
	box->addSkip(st::boxLittleSkip);
}

not_null<Ui::FlatLabel*> AddSubtitle(
		not_null<Ui::GenericBox*> box,
		const QString &text) {
	return box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(tr::semibold(text)),
		st::boxLabel));
}

not_null<Ui::FlatLabel*> AddAbout(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> text) {
	return box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		std::move(text),
		st::boxDividerLabel));
}

struct FolderControls {
	Fn<QString()> folder; // with the chat subfolder, if chosen
	Fn<QString()> baseFolder;
	Fn<bool()> skipExisting;
	rpl::producer<QString> folderValue;
};

FolderControls AddFolderControls(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		PeerData *peer,
		bool withSkipExisting = true,
		QString initial = QString()) {
	AddSubtitle(box, tr::lng_bulk_section_folder(tr::now));
	const auto folder = box->lifetime().make_state<rpl::variable<QString>>(
		initial.isEmpty() ? DefaultFolder(session) : WithSlash(initial));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		folder->value() | rpl::map([](const QString &path) {
			return tr::lng_bulk_download_folder(
				tr::now,
				lt_path,
				QDir::toNativeSeparators(path));
		}),
		st::boxLabel));
	const auto change = box->addRow(object_ptr<Ui::LinkButton>(
		box,
		tr::lng_bulk_download_change(tr::now)));
	change->setClickedCallback([=] {
		const auto current = folder->current();
		FileDialog::GetFolder(
			box.get(),
			tr::lng_download_path_choose(tr::now),
			current.endsWith('/') ? current.mid(0, current.size() - 1) : current,
			crl::guard(box, [=](QString &&result) {
				if (!result.isEmpty()) {
					*folder = WithSlash(result);
				}
			}));
	});
	box->addSkip(st::boxLittleSkip);

	const auto subfolderName = peer ? FolderNameForPeer(peer) : QString();
	const auto subfolder = peer
		? box->addRow(object_ptr<Ui::Checkbox>(
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
	const auto skipExisting = withSkipExisting
		? box->addRow(object_ptr<Ui::Checkbox>(
			box,
			tr::lng_bulk_download_skip_existing(tr::now),
			BulkDownloadSkipExisting.value(),
			st::defaultBoxCheckbox))
		: nullptr;
	if (skipExisting) {
		box->addSkip(st::boxLittleSkip);
	}

	auto result = FolderControls();
	result.baseFolder = [=] {
		return folder->current();
	};
	result.folder = [=] {
		auto path = folder->current();
		const auto use = subfolder && subfolder->checked();
		if (subfolder) {
			BulkDownloadSubfolder.set(use);
		}
		Update([&](Settings &settings) {
			settings.lastFolder = path;
		});
		if (use && !subfolderName.isEmpty()) {
			path = WithSlash(path + subfolderName);
		}
		return path;
	};
	result.skipExisting = [=] {
		const auto skip = !skipExisting || skipExisting->checked();
		if (skipExisting) {
			BulkDownloadSkipExisting.set(skip);
		}
		return skip;
	};
	result.folderValue = folder->value();
	return result;
}

void AddDiskSpaceLabel(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> folder,
		int64 needed) {
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		std::move(folder) | rpl::map([=](const QString &path) {
			QDir().mkpath(path);
			const auto info = QStorageInfo(path);
			if (!info.isValid()) {
				return QString();
			}
			const auto available = int64(info.bytesAvailable());
			auto text = tr::lng_bulk_disk_free(
				tr::now,
				lt_size,
				Ui::FormatSizeText(available));
			if (needed > 0) {
				text += ' ' + tr::lng_bulk_disk_needed(
					tr::now,
					lt_size,
					Ui::FormatSizeText(needed));
				if (available < needed) {
					text += '\n' + tr::lng_bulk_disk_not_enough(tr::now);
				}
			}
			return text;
		}),
		st::boxDividerLabel));
	box->addSkip(st::boxLittleSkip);
}

Fn<QString()> AddTemplateControls(not_null<Ui::GenericBox*> box) {
	AddSubtitle(box, tr::lng_bulk_section_names(tr::now));
	auto presets = TemplatePresets();
	const auto current = Current().nameTemplate.isEmpty()
		? u"{name}"_q
		: Current().nameTemplate;
	if (!ranges::contains(presets, current, &TemplatePreset::value)) {
		presets.push_back({ current, current });
	}
	const auto index = int(ranges::find(
		presets,
		current,
		&TemplatePreset::value) - begin(presets));
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(index);
	for (auto i = 0; i != int(presets.size()); ++i) {
		box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			group,
			i,
			presets[i].example,
			st::defaultBoxCheckbox));
		box->addSkip(st::boxLittleSkip / 2);
	}
	box->addSkip(st::boxLittleSkip);
	return [=] {
		const auto value = presets[std::clamp(
			group->current(),
			0,
			int(presets.size()) - 1)].value;
		Update([&](Settings &settings) {
			settings.nameTemplate = value;
		});
		return value;
	};
}

Fn<Order()> AddOrderControls(not_null<Ui::GenericBox*> box) {
	AddSubtitle(box, tr::lng_bulk_section_order(tr::now));
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		int(Order::Oldest));
	const auto add = [&](Order order, QString text) {
		box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			group,
			int(order),
			text,
			st::defaultBoxCheckbox));
		box->addSkip(st::boxLittleSkip / 2);
	};
	add(Order::Oldest, tr::lng_bulk_order_oldest(tr::now));
	add(Order::Newest, tr::lng_bulk_order_newest(tr::now));
	add(Order::Smallest, tr::lng_bulk_order_smallest(tr::now));
	add(Order::Largest, tr::lng_bulk_order_largest(tr::now));
	box->addSkip(st::boxLittleSkip);
	return [=] {
		return Order(group->current());
	};
}

Fn<Filters()> AddFilterControls(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		uint32 kinds, // kinds to choose from, one kind hides the choice
		bool withDates) {
	AddSubtitle(box, tr::lng_bulk_section_filters(tr::now));
	auto checks = std::vector<std::pair<Kind, Ui::Checkbox*>>();
	auto count = 0;
	for (auto bit = uint32(1); bit <= uint32(Kind::Round); bit <<= 1) {
		if (kinds & bit) {
			++count;
		}
	}
	if (count > 1) {
		for (auto bit = uint32(1); bit <= uint32(Kind::Round); bit <<= 1) {
			if (!(kinds & bit)) {
				continue;
			}
			const auto kind = Kind(bit);
			const auto check = box->addRow(object_ptr<Ui::Checkbox>(
				box,
				KindName(kind),
				true,
				st::defaultBoxCheckbox));
			box->addSkip(st::boxLittleSkip / 2);
			checks.emplace_back(kind, check);
		}
		box->addSkip(st::boxLittleSkip);
	}
	const auto minSize = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_bulk_filter_min_size(),
		QString()));
	const auto maxSize = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_bulk_filter_max_size(),
		QString()));
	const auto extensions = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_bulk_filter_extensions(),
		QString()));
	box->addSkip(st::boxLittleSkip);

	struct Dates {
		rpl::variable<TimeId> from = 0;
		rpl::variable<TimeId> till = 0;
	};
	const auto dates = box->lifetime().make_state<Dates>();
	if (withDates) {
		const auto from = box->addRow(object_ptr<Ui::LinkButton>(
			box,
			QString()));
		dates->from.value() | rpl::on_next([=](TimeId date) {
			from->setText(tr::lng_bulk_filter_from(
				tr::now,
				lt_date,
				FormatDate(date)));
		}, from->lifetime());
		from->setClickedCallback([=] {
			ChooseDate(window, dates->from.current(), [=](TimeId date) {
				dates->from = date;
			});
		});
		const auto till = box->addRow(object_ptr<Ui::LinkButton>(
			box,
			QString()));
		dates->till.value() | rpl::on_next([=](TimeId date) {
			till->setText(tr::lng_bulk_filter_till(
				tr::now,
				lt_date,
				FormatDate(date)));
		}, till->lifetime());
		till->setClickedCallback([=] {
			ChooseDate(window, dates->till.current(), [=](TimeId date) {
				dates->till = date;
			});
		});
		box->addSkip(st::boxLittleSkip);
	}
	const auto notDownloaded = box->addRow(object_ptr<Ui::Checkbox>(
		box,
		tr::lng_bulk_filter_not_downloaded(tr::now),
		false,
		st::defaultBoxCheckbox));
	box->addSkip(st::boxLittleSkip);

	return [=] {
		auto result = Filters();
		if (!checks.empty()) {
			result.kinds = 0;
			for (const auto &[kind, check] : checks) {
				if (check->checked()) {
					result.kinds |= uint32(kind);
				}
			}
		} else {
			result.kinds = kinds ? kinds : kAllKinds;
		}
		result.minSize = ParseMegabytes(minSize->getLastText());
		result.maxSize = ParseMegabytes(maxSize->getLastText());
		result.extensions = ParseExtensions(extensions->getLastText());
		result.minDate = dates->from.current();
		if (const auto till = dates->till.current()) {
			result.maxDate = till + 24 * 60 * 60 - 1; // the whole day
		}
		result.onlyNotDownloaded = notDownloaded->checked();
		return result;
	};
}

void ShowStartedToast(
		not_null<Window::SessionController*> window,
		const QString &text) {
	const auto weak = base::make_weak(window);
	window->showToast({
		.text = TextWithEntities{ text }.append('\n').append(tr::link(
			tr::lng_bulk_open_manager(tr::now),
			u"internal:bulk_open_manager"_q)),
		.filter = [=](const auto &...) {
			if (const auto strong = weak.get()) {
				ShowManagerBox(strong);
			}
			return false;
		},
		.duration = crl::time(6000),
	});
}

struct Collected {
	std::vector<not_null<HistoryItem*>> items;
	PeerData *peer = nullptr; // if all the messages are from one chat
	int64 totalSize = 0;
	int skipped = 0;
	uint32 kinds = 0;
};

[[nodiscard]] Collected Collect(
		not_null<Main::Session*> session,
		const MessageIdsList &ids) {
	auto result = Collected();
	auto media = base::flat_set<uint64>();
	auto multiplePeers = false;
	for (const auto &id : ids) {
		const auto item = session->data().message(id);
		const auto entry = item ? EntryFromItem(item) : std::nullopt;
		if (!entry) {
			++result.skipped;
			continue;
		} else if (!media.emplace(entry->mediaId).second) {
			continue;
		}
		const auto itemMedia = item->media();
		result.kinds |= uint32(KindOf(
			itemMedia->photo(),
			itemMedia->document()));
		const auto peer = item->history()->peer.get();
		if (!multiplePeers) {
			if (!result.peer) {
				result.peer = peer;
			} else if (result.peer != peer) {
				result.peer = nullptr;
				multiplePeers = true;
			}
		}
		result.totalSize += std::max(entry->size, int64(0));
		result.items.push_back(item);
	}
	return result;
}

void DownloadBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		std::shared_ptr<Collected> collected,
		Fn<void()> started) {
	const auto session = &window->session();
	const auto weak = base::make_weak(window);
	const auto count = int(collected->items.size());

	box->setTitle(tr::lng_bulk_download_title());
	box->setWidth(st::boxWideWidth);

	box->addRow(object_ptr<Ui::FlatLabel>(
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
		AddAbout(box, rpl::single(tr::lng_bulk_download_skipped(
			tr::now,
			lt_count,
			collected->skipped)));
	}
	box->addSkip(st::boxLittleSkip);

	const auto filters = AddFilterControls(
		box,
		window,
		collected->kinds,
		true);
	const auto order = AddOrderControls(box);
	const auto nameTemplate = AddTemplateControls(box);
	const auto folder = AddFolderControls(box, session, collected->peer);
	AddDiskSpaceLabel(box, rpl::duplicate(folder.folderValue), collected->totalSize);

	box->addButton(tr::lng_bulk_download_start(), [=] {
		const auto window = weak.get();
		if (!window) {
			box->closeBox();
			return;
		}
		const auto chosen = filters();
		const auto &manager = Get(session);
		auto entries = std::vector<Entry>();
		for (const auto item : collected->items) {
			if (!EntryMatches(chosen, item, &manager)) {
				continue;
			} else if (auto entry = EntryFromItem(item)) {
				entries.push_back(std::move(*entry));
			}
		}
		if (entries.empty()) {
			box->showToast(tr::lng_bulk_download_nothing_filtered(tr::now));
			return;
		}
		SortEntries(entries, order());
		auto options = JobOptions{
			.folder = folder.folder(),
			.chatName = collected->peer
				? collected->peer->name()
				: QString(),
			.nameTemplate = nameTemplate(),
			.skipExisting = folder.skipExisting(),
		};
		const auto size = int(entries.size());
		const auto title = collected->peer
			? tr::lng_bulk_job_title(
				tr::now,
				lt_count,
				size,
				lt_chat,
				collected->peer->name())
			: tr::lng_bulk_job_title_files(tr::now, lt_count, size);
		Get(session).addJob(
			title,
			collected->peer ? collected->peer->id : PeerId(),
			std::move(options),
			std::move(entries));
		box->closeBox();
		ShowStartedToast(window, tr::lng_bulk_download_started(
			tr::now,
			lt_count,
			size));
		if (started) {
			started();
		}
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void DownloadAllBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		int typeIndex) {
	const auto session = &window->session();
	const auto weak = base::make_weak(window);
	box->setTitle(tr::lng_bulk_all_title());
	box->setWidth(st::boxWideWidth);

	AddAbout(box, rpl::single(tr::lng_bulk_all_about(
		tr::now,
		lt_chat,
		peer->name())));
	box->addSkip(st::boxLittleSkip);

	AddSubtitle(box, tr::lng_bulk_section_type(tr::now));
	const auto &types = ScanTypes();
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		std::clamp(typeIndex, 0, int(types.size()) - 1));
	for (auto i = 0; i != int(types.size()); ++i) {
		box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			group,
			i,
			ScanTypeName(i),
			st::defaultBoxCheckbox));
		box->addSkip(st::boxLittleSkip / 2);
	}
	box->addSkip(st::boxLittleSkip);

	const auto filters = AddFilterControls(box, window, 0, true);
	const auto nameTemplate = AddTemplateControls(box);
	const auto folder = AddFolderControls(box, session, peer);
	AddDiskSpaceLabel(box, rpl::duplicate(folder.folderValue), 0);

	box->addButton(tr::lng_bulk_download_start(), [=] {
		const auto window = weak.get();
		if (!window) {
			box->closeBox();
			return;
		}
		const auto type = group->current();
		auto chosen = filters();
		chosen.kinds = ScanTypes()[type].kinds;
		auto options = JobOptions{
			.folder = folder.folder(),
			.chatName = peer->name(),
			.nameTemplate = nameTemplate(),
			.skipExisting = folder.skipExisting(),
		};
		Get(session).addScanJob(
			tr::lng_bulk_all_job_title(
				tr::now,
				lt_type,
				ScanTypeName(type),
				lt_chat,
				peer->name()),
			peer,
			topicRootId,
			type,
			std::move(chosen),
			std::move(options));
		box->closeBox();
		ShowStartedToast(window, tr::lng_bulk_all_started(tr::now));
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void AutoSaveBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer) {
	const auto session = &window->session();
	auto &manager = Get(session);
	const auto existing = manager.autoSaveRule(peer->id);
	box->setTitle(tr::lng_bulk_autosave_box_title());
	box->setWidth(st::boxWideWidth);

	AddAbout(box, rpl::single(tr::lng_bulk_autosave_about(
		tr::now,
		lt_chat,
		peer->name())));
	box->addSkip(st::boxLittleSkip);

	AddSubtitle(box, tr::lng_bulk_section_type(tr::now));
	auto checks = std::vector<std::pair<Kind, Ui::Checkbox*>>();
	const auto kinds = existing
		? existing->kinds
		: (uint32(Kind::Photo) | uint32(Kind::Video));
	for (auto bit = uint32(1); bit <= uint32(Kind::Round); bit <<= 1) {
		const auto kind = Kind(bit);
		checks.emplace_back(kind, box->addRow(object_ptr<Ui::Checkbox>(
			box,
			KindName(kind),
			(kinds & bit) != 0,
			st::defaultBoxCheckbox)));
		box->addSkip(st::boxLittleSkip / 2);
	}
	box->addSkip(st::boxLittleSkip);

	auto initial = existing ? existing->folder : QString();
	const auto folder = AddFolderControls(
		box,
		session,
		existing ? nullptr : peer.get(),
		false,
		initial);

	box->addButton(tr::lng_settings_save(), [=] {
		auto rule = AutoSaveRule{
			.peerId = peer->id,
			.title = peer->name(),
			.folder = folder.folder(),
		};
		for (const auto &[kind, check] : checks) {
			if (check->checked()) {
				rule.kinds |= uint32(kind);
			}
		}
		Get(session).setAutoSaveRule(rule);
		box->closeBox();
		if (const auto strong = base::make_weak(window).get()) {
			strong->showToast(rule.kinds
				? tr::lng_bulk_autosave_on(tr::now, lt_chat, rule.title)
				: tr::lng_bulk_autosave_off(tr::now));
		}
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	if (existing) {
		box->addLeftButton(tr::lng_bulk_autosave_turn_off(), [=] {
			Get(session).setAutoSaveRule({ .peerId = peer->id });
			box->closeBox();
		});
	}
}

void FilterBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		Fn<void(Filters)> done) {
	box->setTitle(tr::lng_bulk_filter_title());
	box->setWidth(st::boxWideWidth);
	AddAbout(box, rpl::single(tr::lng_bulk_filter_about(tr::now)));
	box->addSkip(st::boxLittleSkip);
	const auto filters = AddFilterControls(box, window, kAllKinds, true);
	box->addButton(tr::lng_bulk_filter_select(), [=] {
		const auto result = filters();
		box->closeBox();
		done(result);
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

class ProgressLine final : public Ui::RpWidget {
public:
	ProgressLine(QWidget *parent, rpl::producer<float64> value)
	: RpWidget(parent) {
		resize(width(), style::ConvertScale(6));
		std::move(value) | rpl::on_next([=](float64 progress) {
			_progress = std::clamp(progress, 0., 1.);
			update();
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return style::ConvertScale(6);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		if (_progress > 0.) {
			const auto filled = std::max(
				int(std::round(width() * _progress)),
				height());
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(QRect(0, 0, filled, height()), radius, radius);
		}
	}

private:
	float64 _progress = 0.;

};

class LinksRow final : public Ui::RpWidget {
public:
	explicit LinksRow(QWidget *parent) : RpWidget(parent) {
	}

	Ui::LinkButton *add(const QString &text, Fn<void()> callback) {
		const auto button = Ui::CreateChild<Ui::LinkButton>(this, text);
		button->setClickedCallback(std::move(callback));
		button->show();
		_buttons.push_back(button);
		resizeToWidth(width());
		return button;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto skip = st::boxLittleSkip;
		auto left = 0;
		auto top = 0;
		auto rowHeight = 0;
		for (const auto button : _buttons) {
			const auto w = button->width();
			if (left > 0 && left + w > newWidth) {
				left = 0;
				top += rowHeight + skip / 2;
				rowHeight = 0;
			}
			button->moveToLeft(left, top, newWidth);
			left += w + skip * 2;
			rowHeight = std::max(rowHeight, button->height());
		}
		return top + rowHeight;
	}

private:
	std::vector<not_null<Ui::LinkButton*>> _buttons;

};

[[nodiscard]] QString JobStatusText(
		const Manager &manager,
		not_null<const Job*> job) {
	const auto progress = manager.progress(job);
	const auto ready = progress.done + progress.skipped + progress.failed;
	const auto files = tr::lng_bulk_status_files(
		tr::now,
		lt_ready,
		QString::number(ready),
		lt_total,
		QString::number(progress.total));
	const auto bytes = tr::lng_bulk_status_bytes(
		tr::now,
		lt_ready,
		Ui::FormatSizeText(progress.bytesDone),
		lt_total,
		Ui::FormatSizeText(progress.bytesTotal));
	auto parts = QStringList();
	if (job->scan.active && !job->scan.finished) {
		parts.push_back(tr::lng_bulk_status_scanning(
			tr::now,
			lt_checked,
			QString::number(job->scan.checked),
			lt_found,
			QString::number(job->scan.found)));
	} else if (!job->scan.error.isEmpty()) {
		parts.push_back(tr::lng_bulk_status_scan_error(
			tr::now,
			lt_error,
			job->scan.error));
	}
	switch (job->state) {
	case JobState::Running: {
		if (job->autoSave) {
			parts.push_front(tr::lng_bulk_status_autosave(tr::now));
		}
		parts.push_back(files);
		parts.push_back(bytes);
		const auto speed = manager.jobSpeed(job->id);
		if (manager.speed().scheduleWaiting) {
			parts.push_back(tr::lng_bulk_status_schedule(
				tr::now,
				lt_hours,
				ScheduleText()));
		} else if (speed > 0 && progress.active > 0) {
			parts.push_back(FormatSpeed(speed));
			const auto left = progress.bytesTotal - progress.bytesDone;
			if (left > 0) {
				if (const auto eta = FormatEta(left / speed); !eta.isEmpty()) {
					parts.push_back(eta);
				}
			}
		}
	} break;
	case JobState::Paused:
		parts.push_front((job->pauseReason == PauseReason::LowDisk)
			? tr::lng_bulk_status_low_disk(tr::now)
			: tr::lng_bulk_status_paused(tr::now));
		parts.push_back(files);
		parts.push_back(bytes);
		break;
	case JobState::Finished:
		parts.push_back(tr::lng_bulk_status_finished(
			tr::now,
			lt_done,
			QString::number(progress.done),
			lt_skipped,
			QString::number(progress.skipped),
			lt_failed,
			QString::number(progress.failed)));
		parts.push_back(bytes);
		break;
	case JobState::Cancelled:
		parts.push_front(tr::lng_bulk_status_cancelled(tr::now));
		parts.push_back(files);
		break;
	}
	if (progress.failed > 0 && job->state != JobState::Finished) {
		parts.push_back(tr::lng_bulk_status_failed(
			tr::now,
			lt_count,
			progress.failed));
	}
	return parts.join(u" \xB7 "_q);
}

[[nodiscard]] float64 JobProgressValue(
		const Manager &manager,
		not_null<const Job*> job) {
	const auto progress = manager.progress(job);
	if (progress.bytesTotal > 0) {
		return float64(progress.bytesDone) / progress.bytesTotal;
	} else if (progress.total > 0) {
		return float64(progress.done + progress.skipped + progress.failed)
			/ progress.total;
	}
	return 0.;
}

[[nodiscard]] QString SpeedPanelText(const Manager &manager) {
	const auto speed = manager.speed();
	auto lines = QStringList();
	lines.push_back(tr::lng_bulk_panel_speed(
		tr::now,
		lt_speed,
		FormatSpeed(speed.bytesPerSecond),
		lt_connections,
		QString::number(speed.connections),
		lt_requests,
		QString::number(speed.requests),
		lt_size,
		Ui::FormatSizeText(speed.bytesInFlight)));
	const auto &settings = Current();
	lines.push_back(tr::lng_bulk_panel_state(
		tr::now,
		lt_premium,
		(speed.premium
			? tr::lng_bulk_yes(tr::now)
			: tr::lng_bulk_no(tr::now)),
		lt_limit,
		(settings.speedLimitKB > 0
			? FormatSpeed(int64(settings.speedLimitKB) * 1024)
			: tr::lng_bulk_panel_no_limit(tr::now)),
		lt_queued,
		QString::number(speed.queuedFiles)));
	if (settings.scheduleEnabled) {
		lines.push_back(tr::lng_bulk_panel_schedule(
			tr::now,
			lt_hours,
			ScheduleText()));
	}
	if (speed.premiumDelays > 0) {
		lines.push_back(tr::lng_bulk_panel_throttled(
			tr::now,
			lt_count,
			speed.premiumDelays));
	}
	return lines.join('\n');
}

void FailedBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		uint64 jobId) {
	const auto session = &window->session();
	auto &manager = Get(session);
	const auto job = manager.job(jobId);
	box->setTitle(tr::lng_bulk_failed_title());
	box->setWidth(st::boxWideWidth);
	if (!job) {
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	auto shown = 0;
	auto total = 0;
	for (const auto &entry : job->entries) {
		if (entry.state != EntryState::Failed) {
			continue;
		} else if (++total > kMaxFailedShown) {
			continue;
		}
		++shown;
		const auto name = entry.path.isEmpty()
			? tr::lng_bulk_failed_message(
				tr::now,
				lt_id,
				QString::number(entry.itemId.msg.bare))
			: QFileInfo(entry.path).fileName();
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(tr::semibold(name)),
			st::boxLabel));
		AddAbout(box, rpl::single(entry.error));
		box->addSkip(st::boxLittleSkip / 2);
	}
	if (total > shown) {
		AddAbout(box, rpl::single(tr::lng_bulk_failed_more(
			tr::now,
			lt_count,
			total - shown)));
	}
	if (!total) {
		AddAbout(box, rpl::single(tr::lng_bulk_failed_none(tr::now)));
	}
	if (total) {
		box->addButton(tr::lng_bulk_retry_all(), [=] {
			Get(session).retryFailed(jobId);
			box->closeBox();
		});
	}
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

void AddJobRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> window,
		uint64 jobId) {
	const auto session = &window->session();
	auto &manager = Get(session);
	const auto job = manager.job(jobId);
	if (!job) {
		return;
	}
	// Everything of the row lives in the row, it is recreated on changes.
	const auto row = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto padding = st::boxRowPadding;
	row->add(
		object_ptr<Ui::FlatLabel>(
			row,
			rpl::single(tr::semibold(job->title)),
			st::boxLabel),
		padding);
	const auto status = row->lifetime().make_state<
		rpl::variable<QString>>(JobStatusText(manager, job));
	const auto value = row->lifetime().make_state<
		rpl::variable<float64>>(JobProgressValue(manager, job));
	rpl::merge(
		manager.progressChanged(),
		manager.structureChanged()
	) | rpl::on_next([=] {
		auto &manager = Get(session);
		if (const auto job = manager.job(jobId)) {
			*status = JobStatusText(manager, job);
			*value = JobProgressValue(manager, job);
		}
	}, row->lifetime());
	row->add(
		object_ptr<Ui::FlatLabel>(
			row,
			status->value(),
			st::boxDividerLabel),
		padding);
	row->add(
		object_ptr<ProgressLine>(row, value->value()),
		QMargins(
			padding.left(),
			st::boxLittleSkip / 2,
			padding.right(),
			st::boxLittleSkip / 2));

	const auto links = row->add(object_ptr<LinksRow>(row), padding);
	const auto weak = base::make_weak(window);
	const auto state = job->state;
	if (state == JobState::Running && !job->autoSave) {
		links->add(tr::lng_bulk_pause(tr::now), [=] {
			Get(session).pause(jobId);
		});
	} else if (state == JobState::Paused) {
		links->add(tr::lng_bulk_resume(tr::now), [=] {
			Get(session).resume(jobId);
		});
	}
	if (state == JobState::Running || state == JobState::Paused) {
		links->add(job->autoSave
			? tr::lng_bulk_autosave_turn_off(tr::now)
			: tr::lng_cancel(tr::now), [=] {
			auto &manager = Get(session);
			if (const auto job = manager.job(jobId); job && job->autoSave) {
				manager.setAutoSaveRule({ .peerId = job->peerId });
			} else {
				manager.cancel(jobId);
			}
		});
	}
	links->add(tr::lng_bulk_download_open_folder(tr::now), [=] {
		if (const auto job = Get(session).job(jobId)) {
			const auto path = job->lastPath;
			if (!path.isEmpty() && QFileInfo::exists(path)) {
				File::ShowInFolder(path);
			} else {
				QDir().mkpath(job->options.folder);
				File::ShowInFolder(job->options.folder);
			}
		}
	});
	const auto failed = manager.progress(job).failed;
	if (failed > 0) {
		links->add(tr::lng_bulk_failed_link(tr::now, lt_count, failed), [=] {
			if (const auto strong = weak.get()) {
				strong->show(Box(FailedBox, strong, jobId));
			}
		});
	}
	if (state == JobState::Finished || state == JobState::Cancelled) {
		links->add(tr::lng_bulk_remove(tr::now), [=] {
			Get(session).remove(jobId);
		});
	}
	row->add(object_ptr<Ui::FixedHeightWidget>(row, st::boxMediumSkip));
}

void AddUploadRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> window,
		Fn<void()> refresh) {
	const auto session = &window->session();
	const auto upload = CurrentUpload(session);
	if (!upload) {
		return;
	}
	const auto row = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto padding = st::boxRowPadding;
	row->add(
		object_ptr<Ui::FlatLabel>(
			row,
			rpl::single(tr::semibold(tr::lng_bulk_upload_row_title(
				tr::now,
				lt_chat,
				upload->chat))),
			st::boxLabel),
		padding);
	row->add(
		object_ptr<Ui::FlatLabel>(
			row,
			tr::lng_bulk_upload_row_status(
				tr::now,
				lt_path,
				QDir::toNativeSeparators(upload->folder),
				lt_sent,
				QString::number(upload->sent),
				lt_total,
				QString::number(upload->total)),
			st::boxDividerLabel),
		padding);
	const auto links = row->add(object_ptr<LinksRow>(row), padding);
	if (upload->paused) {
		links->add(tr::lng_bulk_upload_continue(tr::now), [=] {
			ResumeUpload(session);
			refresh();
		});
	}
	links->add(tr::lng_bulk_upload_stop(tr::now), [=] {
		StopUpload(session);
		refresh();
	});
	row->add(object_ptr<Ui::FixedHeightWidget>(row, st::boxMediumSkip));
}

[[nodiscard]] QString StructureSignature(const Manager &manager) {
	auto result = QString();
	for (const auto &job : manager.jobs()) {
		result += QString::number(job->id) + ':'
			+ QString::number(int(job->state)) + ':'
			+ QString::number(int(job->pauseReason)) + ':'
			+ (job->autoSave ? '1' : '0') + ':'
			+ ((manager.progress(job.get()).failed > 0) ? '1' : '0') + ';';
	}
	return result;
}

void ManagerBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window) {
	const auto session = &window->session();
	auto &manager = Get(session);
	box->setTitle(tr::lng_bulk_manager_title());
	box->setWidth(st::boxWideWidth);

	AddSubtitle(box, tr::lng_bulk_panel_title(tr::now));
	const auto panel = box->lifetime().make_state<rpl::variable<QString>>(
		SpeedPanelText(manager));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		panel->value(),
		st::boxDividerLabel));
	box->addSkip(st::boxMediumSkip);

	const auto container = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		QMargins());
	const auto signature = box->lifetime().make_state<QString>();
	const auto scheduled = box->lifetime().make_state<bool>(false);
	const auto rebuild = box->lifetime().make_state<Fn<void()>>();
	// Rebuilds happen after the click that caused them is processed,
	// because they destroy the buttons.
	const auto scheduleRebuild = [=] {
		if (*scheduled) {
			return;
		}
		*scheduled = true;
		crl::on_main(box, [=] {
			*scheduled = false;
			(*rebuild)();
		});
	};
	*rebuild = [=] {
		auto &manager = Get(session);
		*signature = StructureSignature(manager);
		container->clear();
		AddUploadRow(container, window, scheduleRebuild);
		if (manager.jobs().empty()) {
			container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					tr::lng_bulk_manager_empty(tr::now),
					st::boxDividerLabel),
				st::boxRowPadding);
		}
		for (auto i = manager.jobs().size(); i != 0;) {
			AddJobRow(container, window, manager.jobs()[--i]->id);
		}
		container->resizeToWidth(container->width());
	};
	(*rebuild)();

	manager.structureChanged(
	) | rpl::filter([=] {
		return (*signature != StructureSignature(Get(session)));
	}) | rpl::on_next(scheduleRebuild, box->lifetime());

	manager.progressChanged(
	) | rpl::on_next([=] {
		*panel = SpeedPanelText(Get(session));
	}, box->lifetime());

	const auto weak = base::make_weak(window);
	box->addButton(tr::lng_bulk_settings(), [=] {
		if (const auto strong = weak.get()) {
			ShowSettingsBox(strong);
		}
	});
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	box->addLeftButton(tr::lng_bulk_clear_finished(), [=] {
		Get(session).clearFinished();
	});
}

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window) {
	const auto session = &window->session();
	const auto &settings = Current();
	box->setTitle(tr::lng_bulk_settings_title());
	box->setWidth(st::boxWideWidth);

	const auto number = [&](rpl::producer<QString> placeholder, int value) {
		return box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			std::move(placeholder),
			value ? QString::number(value) : QString()));
	};
	const auto check = [&](QString text, bool value) {
		const auto result = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			text,
			value,
			st::defaultBoxCheckbox));
		box->addSkip(st::boxLittleSkip);
		return result;
	};
	const auto parse = [](not_null<Ui::InputField*> field, int fallback) {
		auto ok = false;
		const auto value = field->getLastText().trimmed().toInt(&ok);
		return ok ? value : fallback;
	};

	AddSubtitle(box, tr::lng_bulk_section_speed(tr::now));
	const auto parallel = number(
		tr::lng_bulk_settings_parallel(),
		settings.parallel);
	AddAbout(box, tr::lng_bulk_settings_parallel_about());
	box->addSkip(st::boxLittleSkip);
	const auto limit = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_bulk_settings_limit(),
		(settings.speedLimitKB > 0)
			? QString::number(settings.speedLimitKB / 1024.0, 'f', 1)
			: QString()));
	AddAbout(box, tr::lng_bulk_settings_limit_about());
	box->addSkip(st::boxLittleSkip);
	const auto schedule = check(
		tr::lng_bulk_settings_schedule(tr::now),
		settings.scheduleEnabled);
	const auto from = number(
		tr::lng_bulk_settings_from(),
		settings.scheduleFrom);
	const auto till = number(
		tr::lng_bulk_settings_till(),
		settings.scheduleTill);
	box->addSkip(st::boxLittleSkip);

	AddSubtitle(box, tr::lng_bulk_section_files(tr::now));
	const auto caption = check(
		tr::lng_bulk_settings_caption(tr::now),
		settings.captionFiles);
	const auto fileDate = check(
		tr::lng_bulk_settings_file_date(tr::now),
		settings.messageDateAsFileDate);
	const auto checksums = check(
		tr::lng_bulk_settings_checksums(tr::now),
		settings.checksums);
	const auto known = std::make_shared<Ui::RadiobuttonGroup>(
		settings.skipDownloadedElsewhere
			? 0
			: settings.copyDownloadedElsewhere
			? 1
			: 2);
	AddAbout(box, tr::lng_bulk_settings_known());
	box->addSkip(st::boxLittleSkip / 2);
	const auto addKnown = [&](int value, QString text) {
		box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			known,
			value,
			text,
			st::defaultBoxCheckbox));
		box->addSkip(st::boxLittleSkip / 2);
	};
	addKnown(0, tr::lng_bulk_settings_known_skip(tr::now));
	addKnown(1, tr::lng_bulk_settings_known_copy(tr::now));
	addKnown(2, tr::lng_bulk_settings_known_download(tr::now));
	box->addSkip(st::boxLittleSkip);
	const auto freeSpace = number(
		tr::lng_bulk_settings_free_space(),
		settings.minFreeSpaceMB / 1024);
	AddAbout(box, tr::lng_bulk_settings_free_space_about());
	box->addSkip(st::boxLittleSkip);

	AddSubtitle(box, tr::lng_bulk_section_privacy(tr::now));
	const auto strip = check(
		tr::lng_bulk_settings_strip(tr::now),
		settings.stripMetadata);
	const auto badges = check(
		tr::lng_bulk_settings_badges(tr::now),
		settings.sizeBadges);
	const auto updates = check(
		tr::lng_bulk_settings_updates(tr::now),
		settings.checkUpdates);
	const auto weak = base::make_weak(window);
	const auto checkNow = box->addRow(object_ptr<Ui::LinkButton>(
		box,
		tr::lng_bulk_settings_check_now(tr::now)));
	checkNow->setClickedCallback([=] {
		if (const auto strong = weak.get()) {
			CheckForUpdatesNow(strong, true);
		}
	});
	const auto commit = BuildCommit();
	AddAbout(box, rpl::single(commit.isEmpty()
		? tr::lng_bulk_settings_build_unknown(tr::now)
		: tr::lng_bulk_settings_build(
			tr::now,
			lt_number,
			QString::number(BuildRunNumber()),
			lt_commit,
			commit.mid(0, 7))));
	box->addSkip(st::boxLittleSkip);

	box->addButton(tr::lng_settings_save(), [=] {
		Update([&](Settings &settings) {
			settings.parallel = parse(parallel, settings.parallel);
			auto ok = false;
			const auto mb = limit->getLastText().trimmed().replace(
				',',
				'.').toDouble(&ok);
			settings.speedLimitKB = (ok && mb > 0) ? int(mb * 1024) : 0;
			settings.scheduleEnabled = schedule->checked();
			settings.scheduleFrom = parse(from, settings.scheduleFrom);
			settings.scheduleTill = parse(till, settings.scheduleTill);
			settings.captionFiles = caption->checked();
			settings.messageDateAsFileDate = fileDate->checked();
			settings.checksums = checksums->checked();
			settings.skipDownloadedElsewhere = (known->current() == 0);
			settings.copyDownloadedElsewhere = (known->current() == 1);
			settings.minFreeSpaceMB = std::max(
				parse(freeSpace, settings.minFreeSpaceMB / 1024),
				0) * 1024;
			settings.stripMetadata = strip->checked();
			settings.sizeBadges = badges->checked();
			settings.checkUpdates = updates->checked();
		});
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	box->addLeftButton(tr::lng_bulk_clear_cache(), [=] {
		if (const auto strong = weak.get()) {
			strong->show(Ui::MakeConfirmBox({
				.text = tr::lng_bulk_clear_cache_sure(),
				.confirmed = [=](Fn<void()> close) {
					session->data().cache().clear();
					session->data().cacheBigFile().clear();
					close();
				},
				.confirmText = tr::lng_bulk_clear_cache(),
			}));
		}
	});
}

[[nodiscard]] QString CsvEscape(QString value) {
	if (value.contains('"')
		|| value.contains(',')
		|| value.contains('\n')
		|| value.contains(';')) {
		value.replace('"', u"\"\""_q);
		return '"' + value + '"';
	}
	return value;
}

} // namespace

void ChooseDate(
		not_null<Window::SessionController*> window,
		TimeId current,
		Fn<void(TimeId)> done) {
	const auto highlighted = current
		? base::unixtime::parse(current).date()
		: QDate::currentDate();
	const auto shared = std::make_shared<base::weak_qptr<Ui::CalendarBox>>();
	const auto finalize = [=](not_null<Ui::CalendarBox*> box) {
		box->addLeftButton(tr::lng_bulk_date_any(), [=] {
			done(0);
			if (const auto strong = shared->get()) {
				strong->closeBox();
			}
		});
	};
	const auto callback = [=](const QDate &date, Fn<void()> close) {
		done(base::unixtime::serialize(date.startOfDay()));
		close();
	};
	auto box = Box<Ui::CalendarBox>(Ui::CalendarBoxArgs{
		.month = highlighted,
		.highlighted = highlighted,
		.callback = callback,
		.finalize = finalize,
		.minDate = QDate(2013, 8, 1),
		.maxDate = QDate::currentDate(),
	});
	*shared = base::make_weak(box.data());
	window->show(std::move(box));
}

void ShowManagerBox(not_null<Window::SessionController*> window) {
	window->show(Box(ManagerBox, window));
}

void ShowSettingsBox(not_null<Window::SessionController*> window) {
	window->show(Box(SettingsBox, window));
}

void ShowDownloadBox(
		not_null<Window::SessionController*> window,
		const MessageIdsList &ids,
		Fn<void()> started) {
	auto collected = std::make_shared<Collected>(
		Collect(&window->session(), ids));
	if (collected->items.empty()) {
		window->showToast(tr::lng_bulk_download_nothing(tr::now));
		return;
	}
	window->show(Box(DownloadBox, window, collected, started));
}

void ShowDownloadAllBox(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		int typeIndex) {
	if (!peer->allowsForwarding()) {
		window->showToast(tr::lng_bulk_all_protected(tr::now));
		return;
	}
	window->show(Box(DownloadAllBox, window, peer, topicRootId, typeIndex));
}

void ShowAutoSaveBox(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer) {
	if (!peer->allowsForwarding()) {
		window->showToast(tr::lng_bulk_all_protected(tr::now));
		return;
	}
	window->show(Box(AutoSaveBox, window, peer));
}

void ShowFilterBox(
		not_null<Window::SessionController*> window,
		Fn<void(Filters)> done) {
	window->show(Box(FilterBox, window, std::move(done)));
}

void ExportCsv(
		not_null<Window::SessionController*> window,
		const MessageIdsList &ids) {
	const auto session = &window->session();
	auto lines = QStringList();
	lines.push_back(u"Chat,Message,Date,Type,Name,Size (bytes),Size,Link"_q);
	auto count = 0;
	for (const auto &id : ids) {
		const auto item = session->data().message(id);
		const auto media = item ? item->media() : nullptr;
		if (!media) {
			continue;
		}
		const auto photo = media->photo();
		const auto document = photo ? nullptr : media->document();
		if (!photo && !document) {
			continue;
		}
		const auto size = photo
			? int64(photo->imageByteSize(Data::PhotoSize::Large))
			: document->size;
		const auto peer = item->history()->peer;
		const auto link = (peer->isChannel() && IsServerMsgId(item->id))
			? session->api().exportDirectMessageLink(item, false)
			: QString();
		lines.push_back(QStringList{
			CsvEscape(peer->name()),
			QString::number(item->id.bare),
			base::unixtime::parse(item->date()).toString(Qt::ISODate),
			CsvEscape(KindName(KindOf(photo, document))),
			CsvEscape(EntryFileName(item)),
			QString::number(size),
			CsvEscape(Ui::FormatSizeText(size)),
			CsvEscape(link),
		}.join(','));
		++count;
	}
	if (!count) {
		window->showToast(tr::lng_bulk_download_nothing(tr::now));
		return;
	}
	const auto content = lines.join(u"\r\n"_q) + u"\r\n"_q;
	const auto weak = base::make_weak(window);
	FileDialog::GetWritePath(
		window->widget().get(),
		tr::lng_bulk_csv_title(tr::now),
		u"CSV (*.csv)"_q,
		DefaultFolder(session) + u"files.csv"_q,
		[=](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			auto file = QFile(path);
			auto ok = file.open(QIODevice::WriteOnly);
			if (ok) {
				file.write("\xEF\xBB\xBF"); // UTF-8 BOM for spreadsheets
				ok = (file.write(content.toUtf8()) > 0);
			}
			if (const auto strong = weak.get()) {
				strong->showToast(ok
					? tr::lng_bulk_csv_done(tr::now, lt_count, count)
					: tr::lng_bulk_csv_failed(tr::now));
			}
		});
}

} // namespace Bulk
