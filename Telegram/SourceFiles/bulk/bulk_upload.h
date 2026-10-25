/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Bulk {

[[nodiscard]] bool CanUploadTo(not_null<PeerData*> peer);

// Sends every file of a folder as albums, one album after another,
// so an interrupted upload can continue after a restart.
void ShowUploadFolderBox(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> peer);

void CheckUnfinishedUploads(not_null<Window::SessionController*> window);

struct UploadProgress {
	QString chat;
	QString folder;
	int sent = 0;
	int total = 0;
	int skipped = 0;
	bool active = false;
	bool paused = false;
};
[[nodiscard]] std::optional<UploadProgress> CurrentUpload(
	not_null<Main::Session*> session);
void StopUpload(not_null<Main::Session*> session);
void ResumeUpload(not_null<Main::Session*> session);

} // namespace Bulk
