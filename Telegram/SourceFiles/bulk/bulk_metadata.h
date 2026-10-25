/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Bulk {

// Location, camera and date details are removed from a copy of the file
// (JPEG, PNG, MP4, MOV), the original file is never changed.
// Returns the original path if there is nothing to remove or on errors.
// May be called from any thread.
[[nodiscard]] QString PrepareStrippedCopy(const QString &path);

// Returns the input if nothing was removed.
[[nodiscard]] QByteArray StripJpeg(const QByteArray &bytes);
[[nodiscard]] QByteArray StripPng(const QByteArray &bytes);

// Removes copies left from the previous runs.
void CleanStrippedCopies();

} // namespace Bulk
