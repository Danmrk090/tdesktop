/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk/bulk_settings.h"

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Bulk {

void ShowManagerBox(not_null<Window::SessionController*> window);
void ShowSettingsBox(not_null<Window::SessionController*> window);

// Downloads the files of the given messages, protected ones are skipped.
void ShowDownloadBox(
	not_null<Window::SessionController*> window,
	const MessageIdsList &ids,
	Fn<void()> started);

// Downloads every file of a type from a chat, without a selection limit.
void ShowDownloadAllBox(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> peer,
	MsgId topicRootId = 0,
	int typeIndex = 0);

void ShowAutoSaveBox(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> peer);

void ShowFilterBox(
	not_null<Window::SessionController*> window,
	Fn<void(Filters)> done);

// Name, size, date, type and link of every file, as a CSV table.
void ExportCsv(
	not_null<Window::SessionController*> window,
	const MessageIdsList &ids);

// Picks a date, nullopt result means "any date".
void ChooseDate(
	not_null<Window::SessionController*> window,
	TimeId current,
	Fn<void(TimeId)> done);

} // namespace Bulk
