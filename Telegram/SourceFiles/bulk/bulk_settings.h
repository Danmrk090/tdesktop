/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;
class DocumentData;
class PhotoData;

namespace Bulk {

// Kinds of files, used in filters and auto-save rules.
enum class Kind : uint32 {
	Photo = 0x01,
	Video = 0x02,
	File = 0x04,
	Music = 0x08,
	Voice = 0x10,
	Gif = 0x20,
	Round = 0x40,
};
inline constexpr auto kAllKinds = uint32(0x7F);

[[nodiscard]] Kind KindOf(
	PhotoData *photo,
	DocumentData *document);

struct Settings {
	int parallel = 3; // files downloaded at the same time
	QString nameTemplate; // empty means "{name}"
	QString lastFolder;
	bool captionFiles = false;
	bool messageDateAsFileDate = true;
	bool checksums = false;
	bool skipDownloadedElsewhere = true;
	bool copyDownloadedElsewhere = false;
	int speedLimitKB = 0; // KB per second, 0 means no limit
	bool scheduleEnabled = false;
	int scheduleFrom = 1; // hour of day
	int scheduleTill = 7; // hour of day
	int minFreeSpaceMB = 2048;
	bool stripMetadata = true;
	bool checkUpdates = true;
	bool sizeBadges = true;
};

[[nodiscard]] const Settings &Current();
void Update(Fn<void(Settings&)> change);
[[nodiscard]] rpl::producer<> Changes();

// tdata/bulk/ with a trailing slash, created on demand.
[[nodiscard]] QString StateFolder();

// Downloads allowed by the night schedule right now.
[[nodiscard]] bool ScheduleAllowsNow();
[[nodiscard]] QString ScheduleText();

// Speed limit for all downloads, 0 means no limit.
// May be called from any thread.
[[nodiscard]] int64 SpeedLimitBytesPerSecond();

// May be called from any thread.
[[nodiscard]] bool StripMetadataEnabled();

struct NameParts {
	QString chat;
	QString type;
	QString name; // file name with extension
	TimeId date = 0;
	MsgId msgId = 0;
};

// Relative path from a template, components are sanitized.
[[nodiscard]] QString ApplyTemplate(
	const QString &nameTemplate,
	const NameParts &parts);

struct TemplatePreset {
	QString value;
	QString example;
};
[[nodiscard]] std::vector<TemplatePreset> TemplatePresets();

[[nodiscard]] QString KindName(Kind kind);

struct Filters {
	int64 minSize = 0; // bytes, 0 means no limit
	int64 maxSize = 0; // bytes, 0 means no limit
	TimeId minDate = 0; // 0 means no limit
	TimeId maxDate = 0; // 0 means no limit
	QStringList extensions; // lowercase without a dot, empty means any
	uint32 kinds = kAllKinds;
	bool onlyNotDownloaded = false;

	[[nodiscard]] bool empty() const {
		return !minSize
			&& !maxSize
			&& !minDate
			&& !maxDate
			&& extensions.isEmpty()
			&& (kinds == kAllKinds)
			&& !onlyNotDownloaded;
	}
};

[[nodiscard]] QStringList ParseExtensions(const QString &text);
[[nodiscard]] QString ExtensionOf(const QString &name);

enum class Order : uchar {
	Oldest,
	Newest,
	Smallest,
	Largest,
};

[[nodiscard]] QString FormatSpeed(int64 bytesPerSecond);
[[nodiscard]] QString FormatEta(int64 seconds);

} // namespace Bulk
