/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include "bulk/bulk_boxes.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h" // HistoryView::SelectedItem.
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

namespace Menu {
namespace {

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
	menu->addAction(tr::lng_bulk_csv_menu(tr::now), [=] {
		if (const auto strong = weak.get()) {
			Bulk::ExportCsv(strong, ids);
		}
	}, &st::menuIconExport);
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
	Bulk::ShowDownloadBox(window, ids, std::move(started));
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
