/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk/bulk_file_guard.h"

#include "core/mime_type.h"

namespace Bulk {
namespace {

constexpr auto kSuspiciousSpaces = 3;

[[nodiscard]] bool IsBidiControl(QChar ch) {
	const auto code = ch.unicode();
	return (code == 0x200E)
		|| (code == 0x200F)
		|| (code == 0x061C)
		|| (code >= 0x202A && code <= 0x202E)
		|| (code >= 0x2066 && code <= 0x2069);
}

[[nodiscard]] bool IsInvisible(QChar ch) {
	const auto code = ch.unicode();
	return (code == 0x200B) // zero width space
		|| (code == 0x200C)
		|| (code == 0x200D)
		|| (code == 0x2060)
		|| (code == 0xFEFF);
}

} // namespace

DisguiseCheck CheckDisguise(const QString &fileName) {
	auto result = DisguiseCheck();
	const auto dot = fileName.lastIndexOf('.');
	result.realExtension = (dot >= 0)
		? fileName.mid(dot + 1).trimmed().toLower()
		: QString();

	auto hidden = false;
	auto visible = QString();
	visible.reserve(fileName.size() + 16);
	auto spaces = 0;
	const auto flushSpaces = [&] {
		if (spaces >= kSuspiciousSpaces) {
			visible += u"[%1 spaces]"_q.arg(spaces);
		} else {
			visible += QString(spaces, ' ');
		}
		spaces = 0;
	};
	for (const auto ch : fileName) {
		if (ch.isSpace()) {
			++spaces;
			continue;
		}
		flushSpaces();
		if (IsBidiControl(ch) || IsInvisible(ch)) {
			hidden = true;
			visible += u"[U+%1]"_q.arg(
				QString::number(ch.unicode(), 16).toUpper(),
				4,
				QChar('0'));
		} else {
			visible += ch;
		}
	}
	flushSpaces();
	result.visibleName = visible;

	const auto executable = (Core::DetectNameType(fileName)
		== Core::NameType::Executable);
	auto doubleExtension = false;
	auto paddedExtension = false;
	if (executable && dot > 0) {
		const auto base = fileName.mid(0, dot);
		auto trailing = 0;
		for (auto i = base.size(); i != 0 && base[i - 1].isSpace(); --i) {
			++trailing;
		}
		paddedExtension = (trailing >= kSuspiciousSpaces);
		const auto inner = base.trimmed();
		const auto innerType = Core::DetectNameType(inner);
		doubleExtension = !Core::FileExtension(inner).isEmpty()
			&& (innerType == Core::NameType::Image
				|| innerType == Core::NameType::Video
				|| innerType == Core::NameType::Audio
				|| innerType == Core::NameType::Document
				|| innerType == Core::NameType::Archive);
	}
	result.disguised = hidden || doubleExtension || paddedExtension;
	return result;
}

} // namespace Bulk
