/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk/bulk_settings.h"

#include "base/base_file_utilities.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "lang/lang_keys.h"
#include "ui/text/format_values.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

#include <atomic>

namespace Bulk {
namespace {

constexpr auto kMaxComponentLength = 120;
constexpr auto kMaxParallel = 8;

struct State {
	Settings settings;
	bool loaded = false;
	rpl::event_stream<> changes;
};

std::atomic<int64> SpeedLimit = 0;
std::atomic<bool> StripMetadata = true;

void Publish(const Settings &settings) {
	SpeedLimit = int64(settings.speedLimitKB) * 1024;
	StripMetadata = settings.stripMetadata;
}

[[nodiscard]] State &GetState() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString SettingsPath() {
	return StateFolder() + u"settings.json"_q;
}

void Normalize(Settings &settings) {
	settings.parallel = std::clamp(settings.parallel, 1, kMaxParallel);
	settings.speedLimitKB = std::max(settings.speedLimitKB, 0);
	settings.scheduleFrom = std::clamp(settings.scheduleFrom, 0, 23);
	settings.scheduleTill = std::clamp(settings.scheduleTill, 0, 23);
	settings.minFreeSpaceMB = std::max(settings.minFreeSpaceMB, 0);
	if (settings.skipDownloadedElsewhere) {
		settings.copyDownloadedElsewhere = false;
	}
}

void Load(Settings &settings) {
	auto file = QFile(SettingsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return;
	}
	const auto object = document.object();
	const auto readInt = [&](const char *key, int &value) {
		const auto i = object.find(QLatin1String(key));
		if (i != object.end() && i->isDouble()) {
			value = i->toInt();
		}
	};
	const auto readBool = [&](const char *key, bool &value) {
		const auto i = object.find(QLatin1String(key));
		if (i != object.end() && i->isBool()) {
			value = i->toBool();
		}
	};
	readInt("parallel", settings.parallel);
	if (const auto i = object.find(u"nameTemplate"_q);
		i != object.end() && i->isString()) {
		settings.nameTemplate = i->toString();
	}
	if (const auto i = object.find(u"lastFolder"_q);
		i != object.end() && i->isString()) {
		settings.lastFolder = i->toString();
	}
	readBool("captionFiles", settings.captionFiles);
	readBool("messageDateAsFileDate", settings.messageDateAsFileDate);
	readBool("checksums", settings.checksums);
	readBool("skipDownloadedElsewhere", settings.skipDownloadedElsewhere);
	readBool("copyDownloadedElsewhere", settings.copyDownloadedElsewhere);
	readInt("speedLimitKB", settings.speedLimitKB);
	readBool("scheduleEnabled", settings.scheduleEnabled);
	readInt("scheduleFrom", settings.scheduleFrom);
	readInt("scheduleTill", settings.scheduleTill);
	readInt("minFreeSpaceMB", settings.minFreeSpaceMB);
	readBool("stripMetadata", settings.stripMetadata);
	readBool("checkUpdates", settings.checkUpdates);
	readBool("sizeBadges", settings.sizeBadges);
	Normalize(settings);
}

void Save(const Settings &settings) {
	auto object = QJsonObject();
	object.insert(u"parallel"_q, settings.parallel);
	object.insert(u"nameTemplate"_q, settings.nameTemplate);
	object.insert(u"lastFolder"_q, settings.lastFolder);
	object.insert(u"captionFiles"_q, settings.captionFiles);
	object.insert(
		u"messageDateAsFileDate"_q,
		settings.messageDateAsFileDate);
	object.insert(u"checksums"_q, settings.checksums);
	object.insert(
		u"skipDownloadedElsewhere"_q,
		settings.skipDownloadedElsewhere);
	object.insert(
		u"copyDownloadedElsewhere"_q,
		settings.copyDownloadedElsewhere);
	object.insert(u"speedLimitKB"_q, settings.speedLimitKB);
	object.insert(u"scheduleEnabled"_q, settings.scheduleEnabled);
	object.insert(u"scheduleFrom"_q, settings.scheduleFrom);
	object.insert(u"scheduleTill"_q, settings.scheduleTill);
	object.insert(u"minFreeSpaceMB"_q, settings.minFreeSpaceMB);
	object.insert(u"stripMetadata"_q, settings.stripMetadata);
	object.insert(u"checkUpdates"_q, settings.checkUpdates);
	object.insert(u"sizeBadges"_q, settings.sizeBadges);

	auto file = QSaveFile(SettingsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
		file.commit();
	}
}

[[nodiscard]] QString Two(int value) {
	return QString::number(value).rightJustified(2, '0');
}

[[nodiscard]] QString SanitizeComponent(QString component) {
	component = component.trimmed();
	if (component.size() > kMaxComponentLength) {
		component = component.mid(0, kMaxComponentLength).trimmed();
	}
	if (component.isEmpty() || component == u"."_q || component == u".."_q) {
		return QString();
	}
	component = base::FileNameFromUserString(component);
	while (component.endsWith('.') || component.endsWith(' ')) {
		component.chop(1);
	}
	return component;
}

} // namespace

Kind KindOf(PhotoData *photo, DocumentData *document) {
	if (photo || !document) {
		return Kind::Photo;
	} else if (document->isVideoMessage()) {
		return Kind::Round;
	} else if (document->isVoiceMessage()) {
		return Kind::Voice;
	} else if (document->isAnimation()) {
		return Kind::Gif;
	} else if (document->isVideoFile()) {
		return Kind::Video;
	} else if (document->isAudioFile()) {
		return Kind::Music;
	}
	return Kind::File;
}

const Settings &Current() {
	auto &state = GetState();
	if (!state.loaded) {
		state.loaded = true;
		Load(state.settings);
		Publish(state.settings);
	}
	return state.settings;
}

void Update(Fn<void(Settings&)> change) {
	auto &state = GetState();
	auto copy = Current();
	change(copy);
	Normalize(copy);
	state.settings = copy;
	Publish(copy);
	Save(copy);
	state.changes.fire({});
}

rpl::producer<> Changes() {
	return GetState().changes.events();
}

QString StateFolder() {
	const auto result = cWorkingDir() + u"tdata/bulk/"_q;
	QDir().mkpath(result);
	return result;
}

bool ScheduleAllowsNow() {
	const auto &settings = Current();
	if (!settings.scheduleEnabled
		|| settings.scheduleFrom == settings.scheduleTill) {
		return true;
	}
	const auto hour = QTime::currentTime().hour();
	const auto from = settings.scheduleFrom;
	const auto till = settings.scheduleTill;
	return (from < till)
		? (hour >= from && hour < till)
		: (hour >= from || hour < till);
}

QString ScheduleText() {
	const auto &settings = Current();
	return tr::lng_bulk_schedule_hours(
		tr::now,
		lt_from,
		Two(settings.scheduleFrom) + u":00"_q,
		lt_till,
		Two(settings.scheduleTill) + u":00"_q);
}

int64 SpeedLimitBytesPerSecond() {
	return SpeedLimit.load();
}

bool StripMetadataEnabled() {
	return StripMetadata.load();
}

QString ApplyTemplate(const QString &nameTemplate, const NameParts &parts) {
	const auto name = parts.name.isEmpty() ? u"file"_q : parts.name;
	const auto dot = name.lastIndexOf('.');
	const auto base = (dot > 0) ? name.mid(0, dot) : name;
	const auto extension = (dot > 0) ? name.mid(dot + 1) : QString();
	const auto when = parts.date
		? base::unixtime::parse(parts.date)
		: QDateTime::currentDateTime();
	const auto date = when.date();
	const auto time = when.time();

	auto pattern = nameTemplate.trimmed();
	if (pattern.isEmpty()) {
		pattern = u"{name}"_q;
	}
	pattern.replace('\\', '/');
	const auto hasName = pattern.contains(u"{name}"_q)
		|| pattern.contains(u"{ext}"_q);
	auto result = pattern;
	result.replace(u"{chat}"_q, parts.chat.isEmpty() ? u"Chat"_q : parts.chat);
	result.replace(u"{type}"_q, parts.type);
	result.replace(u"{year}"_q, QString::number(date.year()));
	result.replace(u"{month}"_q, Two(date.month()));
	result.replace(u"{day}"_q, Two(date.day()));
	result.replace(
		u"{date}"_q,
		u"%1-%2-%3"_q
			.arg(date.year())
			.arg(Two(date.month()))
			.arg(Two(date.day())));
	result.replace(
		u"{time}"_q,
		u"%1-%2-%3"_q
			.arg(Two(time.hour()))
			.arg(Two(time.minute()))
			.arg(Two(time.second())));
	result.replace(u"{msg}"_q, QString::number(parts.msgId.bare));
	result.replace(u"{base}"_q, base);
	result.replace(u"{ext}"_q, extension);
	result.replace(u"{name}"_q, name);

	auto components = QStringList();
	for (const auto &component : result.split('/')) {
		if (auto clean = SanitizeComponent(component); !clean.isEmpty()) {
			components.push_back(clean);
		}
	}
	if (components.isEmpty()) {
		components.push_back(SanitizeComponent(name));
	} else if (!hasName && !extension.isEmpty()) {
		components.back() += '.' + extension;
	}
	if (components.back().isEmpty()) {
		components.back() = u"file"_q;
	}
	return components.join('/');
}

std::vector<TemplatePreset> TemplatePresets() {
	return {
		{ u"{name}"_q, u"video.mp4"_q },
		{ u"{date}_{name}"_q, u"2026-10-25_video.mp4"_q },
		{ u"{year}-{month}/{date}_{name}"_q, u"2026-10/2026-10-25_video.mp4"_q },
		{ u"{type}/{name}"_q, u"Videos/video.mp4"_q },
		{ u"{year}-{month}/{type}/{name}"_q, u"2026-10/Videos/video.mp4"_q },
	};
}

QString KindName(Kind kind) {
	switch (kind) {
	case Kind::Photo: return tr::lng_bulk_kind_photos(tr::now);
	case Kind::Video: return tr::lng_bulk_kind_videos(tr::now);
	case Kind::File: return tr::lng_bulk_kind_files(tr::now);
	case Kind::Music: return tr::lng_bulk_kind_music(tr::now);
	case Kind::Voice: return tr::lng_bulk_kind_voice(tr::now);
	case Kind::Gif: return tr::lng_bulk_kind_gifs(tr::now);
	case Kind::Round: return tr::lng_bulk_kind_round(tr::now);
	}
	return QString();
}

QStringList ParseExtensions(const QString &text) {
	auto result = QStringList();
	for (auto part : text.split(QRegularExpression(u"[\\s,;]+"_q))) {
		part = part.trimmed().toLower();
		while (part.startsWith('.') || part.startsWith('*')) {
			part = part.mid(1);
		}
		if (!part.isEmpty() && !result.contains(part)) {
			result.push_back(part);
		}
	}
	return result;
}

QString ExtensionOf(const QString &name) {
	const auto dot = name.lastIndexOf('.');
	return (dot > 0 && dot + 1 < name.size())
		? name.mid(dot + 1).toLower()
		: QString();
}

QString FormatSpeed(int64 bytesPerSecond) {
	return tr::lng_bulk_speed_value(
		tr::now,
		lt_size,
		Ui::FormatSizeText(std::max(bytesPerSecond, int64(0))));
}

QString FormatEta(int64 seconds) {
	if (seconds <= 0) {
		return QString();
	} else if (seconds < 60) {
		return tr::lng_bulk_eta_seconds(tr::now, lt_count, int(seconds));
	} else if (seconds < 3600) {
		return tr::lng_bulk_eta_minutes(
			tr::now,
			lt_count,
			int((seconds + 59) / 60));
	}
	const auto hours = int(seconds / 3600);
	const auto minutes = int((seconds % 3600) / 60);
	return tr::lng_bulk_eta_hours(
		tr::now,
		lt_hours,
		QString::number(hours),
		lt_minutes,
		Two(minutes));
}

} // namespace Bulk
