/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Bulk {

// Only tells about a newer build of this fork, never installs anything.
void CheckForUpdates(not_null<Window::SessionController*> window);
void CheckForUpdatesNow(
	not_null<Window::SessionController*> window,
	bool manual);

[[nodiscard]] QString BuildCommit();
[[nodiscard]] int BuildRunNumber();
[[nodiscard]] QString BuildRepository();

} // namespace Bulk
