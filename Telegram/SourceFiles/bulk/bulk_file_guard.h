/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Bulk {

struct DisguiseCheck {
	QString realExtension; // lowercase, without a dot
	QString visibleName; // hidden characters and long spaces made visible
	bool disguised = false;
};

// Detects names that hide their real type, like "photo.jpg.exe",
// "video.mp4        .scr" or names with right-to-left override marks.
[[nodiscard]] DisguiseCheck CheckDisguise(const QString &fileName);

} // namespace Bulk
