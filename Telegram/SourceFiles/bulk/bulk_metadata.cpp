/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk/bulk_metadata.h"

#include "base/random.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Bulk {
namespace {

constexpr auto kMaxImageSize = int64(200) * 1024 * 1024;
constexpr auto kMaxBoxDepth = 6;

[[nodiscard]] QString CopiesFolder() {
	return QDir::tempPath() + u"/tdesktop_bulk_strip/"_q;
}

[[nodiscard]] uint32 Read16(const uchar *data, bool bigEndian) {
	return bigEndian
		? ((uint32(data[0]) << 8) | uint32(data[1]))
		: ((uint32(data[1]) << 8) | uint32(data[0]));
}

[[nodiscard]] uint32 Read32(const uchar *data, bool bigEndian) {
	return bigEndian
		? ((uint32(data[0]) << 24)
			| (uint32(data[1]) << 16)
			| (uint32(data[2]) << 8)
			| uint32(data[3]))
		: ((uint32(data[3]) << 24)
			| (uint32(data[2]) << 16)
			| (uint32(data[1]) << 8)
			| uint32(data[0]));
}

// Orientation from an APP1 Exif payload (after the length field).
[[nodiscard]] int ExifOrientation(const QByteArray &payload) {
	const auto size = int(payload.size());
	if (size < 6 + 8 || !payload.startsWith(QByteArray("Exif\0\0", 6))) {
		return 0;
	}
	const auto tiff = reinterpret_cast<const uchar*>(payload.constData()) + 6;
	const auto tiffSize = size - 6;
	const auto bigEndian = (tiff[0] == 'M' && tiff[1] == 'M');
	if (!bigEndian && !(tiff[0] == 'I' && tiff[1] == 'I')) {
		return 0;
	}
	const auto ifd = Read32(tiff + 4, bigEndian);
	if (ifd + 2 > uint32(tiffSize)) {
		return 0;
	}
	const auto count = Read16(tiff + ifd, bigEndian);
	for (auto i = 0; i != int(count); ++i) {
		const auto offset = ifd + 2 + uint32(i) * 12;
		if (offset + 12 > uint32(tiffSize)) {
			break;
		}
		const auto entry = tiff + offset;
		if (Read16(entry, bigEndian) == 0x0112
			&& Read16(entry + 2, bigEndian) == 3) {
			const auto value = int(Read16(entry + 8, bigEndian));
			return (value >= 1 && value <= 8) ? value : 0;
		}
	}
	return 0;
}

[[nodiscard]] QByteArray OrientationSegment(int orientation) {
	auto result = QByteArray();
	result.append(char(0xFF)).append(char(0xE1));
	result.append(char(0x00)).append(char(34)); // 2 + 32 bytes
	result.append("Exif\0\0", 6);
	result.append("MM\0\x2A", 4);
	result.append("\0\0\0\x08", 4); // IFD0 offset
	result.append("\0\x01", 2); // one entry
	result.append("\x01\x12", 2); // Orientation
	result.append("\0\x03", 2); // SHORT
	result.append("\0\0\0\x01", 4); // count
	result.append(char(0)).append(char(orientation)).append("\0\0", 2);
	result.append("\0\0\0\0", 4); // no next IFD
	return result;
}

[[nodiscard]] bool IsXmpUuid(const QByteArray &uuid) {
	static const auto kXmp = QByteArray::fromHex(
		"be7acfcb97a942e89c71999491e3afac");
	return (uuid == kXmp);
}

class BoxPatcher final {
public:
	explicit BoxPatcher(QFile &file) : _file(file) {
	}

	bool run() {
		walk(0, _file.size(), 0);
		return _changed && !_failed;
	}

private:
	struct Header {
		int64 start = 0;
		int64 size = 0;
		int headerSize = 0;
		QByteArray type;
	};

	[[nodiscard]] std::optional<Header> readHeader(int64 start, int64 end) {
		if (start + 8 > end || !_file.seek(start)) {
			return std::nullopt;
		}
		const auto bytes = _file.read(16);
		if (bytes.size() < 8) {
			return std::nullopt;
		}
		const auto data = reinterpret_cast<const uchar*>(bytes.constData());
		auto result = Header{ .start = start };
		const auto size32 = Read32(data, true);
		result.type = bytes.mid(4, 4);
		if (size32 == 1) {
			if (bytes.size() < 16) {
				return std::nullopt;
			}
			result.size = (int64(Read32(data + 8, true)) << 32)
				| int64(Read32(data + 12, true));
			result.headerSize = 16;
		} else if (size32 == 0) {
			result.size = end - start;
			result.headerSize = 8;
		} else {
			result.size = int64(size32);
			result.headerSize = 8;
		}
		if (result.size < result.headerSize || start + result.size > end) {
			return std::nullopt;
		}
		return result;
	}

	void rename(const Header &header) {
		if (_file.seek(header.start + 4) && _file.write("free", 4) == 4) {
			_changed = true;
		} else {
			_failed = true;
		}
	}

	void zeroTimes(const Header &header) {
		const auto body = header.start + header.headerSize;
		if (!_file.seek(body)) {
			return;
		}
		const auto version = _file.read(1);
		if (version.size() != 1) {
			return;
		}
		const auto length = (version[0] == 1) ? 16 : 8;
		if (body + 4 + length > header.start + header.size
			|| !_file.seek(body + 4)) {
			return;
		}
		if (_file.write(QByteArray(length, 0)) == length) {
			_changed = true;
		} else {
			_failed = true;
		}
	}

	void walk(int64 start, int64 end, int depth) {
		auto position = start;
		while (!_failed && position < end) {
			const auto header = readHeader(position, end);
			if (!header) {
				return;
			}
			const auto &type = header->type;
			if (depth == 0 && type == "uuid") {
				if (_file.seek(position + header->headerSize)
					&& IsXmpUuid(_file.read(16))) {
					rename(*header);
				}
			} else if (depth > 0 && (type == "udta" || type == "meta")) {
				rename(*header);
			} else if (type == "mvhd" || type == "tkhd" || type == "mdhd") {
				zeroTimes(*header);
			} else if ((type == "moov" || type == "trak" || type == "mdia")
				&& depth < kMaxBoxDepth) {
				walk(
					position + header->headerSize,
					position + header->size,
					depth + 1);
			}
			if (header->size <= 0) {
				return;
			}
			position += header->size;
		}
	}

	QFile &_file;
	bool _changed = false;
	bool _failed = false;

};

[[nodiscard]] QString MakeCopyPath(const QString &path) {
	const auto folder = CopiesFolder()
		+ QString::number(base::RandomValue<uint64>(), 16)
		+ '/';
	if (!QDir().mkpath(folder)) {
		return QString();
	}
	return folder + QFileInfo(path).fileName();
}

[[nodiscard]] QString StripImageFile(
		const QString &path,
		QByteArray(*method)(const QByteArray&)) {
	auto file = QFile(path);
	if (file.size() > kMaxImageSize || !file.open(QIODevice::ReadOnly)) {
		return path;
	}
	const auto original = file.readAll();
	file.close();
	const auto stripped = method(original);
	if (stripped.size() == original.size() && stripped == original) {
		return path;
	}
	const auto copy = MakeCopyPath(path);
	if (copy.isEmpty()) {
		return path;
	}
	auto out = QFile(copy);
	if (!out.open(QIODevice::WriteOnly)
		|| out.write(stripped) != stripped.size()) {
		out.close();
		QFile::remove(copy);
		return path;
	}
	return copy;
}

[[nodiscard]] QString StripVideoFile(const QString &path) {
	const auto copy = MakeCopyPath(path);
	if (copy.isEmpty() || !QFile::copy(path, copy)) {
		return path;
	}
	auto file = QFile(copy);
	if (!file.open(QIODevice::ReadWrite)) {
		QFile::remove(copy);
		return path;
	}
	auto patcher = BoxPatcher(file);
	const auto changed = patcher.run();
	file.close();
	if (!changed) {
		QFile::remove(copy);
		return path;
	}
	return copy;
}

} // namespace

QByteArray StripJpeg(const QByteArray &bytes) {
	const auto size = int(bytes.size());
	const auto data = reinterpret_cast<const uchar*>(bytes.constData());
	if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
		return bytes;
	}
	auto result = QByteArray();
	result.reserve(size);
	result.append(bytes.constData(), 2);
	auto changed = false;
	auto orientation = 0;
	auto position = 2;
	while (true) {
		if (position + 4 > size || data[position] != 0xFF) {
			return bytes; // malformed or truncated
		}
		const auto marker = data[position + 1];
		if (marker == 0xFF) {
			++position;
			continue;
		} else if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
			result.append(bytes.constData() + position, 2);
			position += 2;
			continue;
		} else if (marker == 0xD9 || marker == 0xDA) {
			result.append(bytes.constData() + position, size - position);
			break;
		}
		const auto length = int(Read16(data + position + 2, true));
		if (length < 2 || position + 2 + length > size) {
			return bytes;
		}
		const auto payload = QByteArray::fromRawData(
			bytes.constData() + position + 4,
			length - 2);
		auto drop = false;
		if (marker == 0xE1) { // Exif or XMP
			if (const auto value = ExifOrientation(payload)) {
				orientation = value;
			}
			drop = true;
		} else if (marker == 0xED || marker == 0xFE) { // IPTC, comment
			drop = true;
		} else if (marker >= 0xE3 && marker <= 0xEF && marker != 0xEE) {
			drop = true; // APP3-APP15 except Adobe color info
		}
		if (drop) {
			changed = true;
		} else {
			result.append(bytes.constData() + position, 2 + length);
		}
		position += 2 + length;
	}
	if (!changed) {
		return bytes;
	} else if (orientation > 1) {
		result.insert(2, OrientationSegment(orientation));
	}
	return result;
}

QByteArray StripPng(const QByteArray &bytes) {
	static const auto kSignature = QByteArray::fromHex("89504e470d0a1a0a");
	const auto size = int64(bytes.size());
	if (!bytes.startsWith(kSignature)) {
		return bytes;
	}
	const auto data = reinterpret_cast<const uchar*>(bytes.constData());
	auto result = QByteArray();
	result.reserve(size);
	result.append(kSignature);
	auto changed = false;
	auto position = int64(kSignature.size());
	while (position + 12 <= size) {
		const auto length = int64(Read32(data + position, true));
		const auto total = 12 + length;
		if (length > size || position + total > size) {
			return bytes;
		}
		const auto type = bytes.mid(position + 4, 4);
		if (type == "tEXt"
			|| type == "zTXt"
			|| type == "iTXt"
			|| type == "eXIf"
			|| type == "tIME") {
			changed = true;
		} else {
			result.append(bytes.constData() + position, total);
		}
		position += total;
		if (type == "IEND") {
			break;
		}
	}
	return changed ? result : bytes;
}

QString PrepareStrippedCopy(const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.exists() || !info.isFile()) {
		return path;
	}
	const auto extension = info.suffix().toLower();
	if (extension == u"jpg"_q
		|| extension == u"jpeg"_q
		|| extension == u"jfif"_q) {
		return StripImageFile(path, &StripJpeg);
	} else if (extension == u"png"_q) {
		return StripImageFile(path, &StripPng);
	} else if (extension == u"mp4"_q
		|| extension == u"m4v"_q
		|| extension == u"mov"_q
		|| extension == u"3gp"_q) {
		return StripVideoFile(path);
	}
	return path;
}

void CleanStrippedCopies() {
	const auto folder = CopiesFolder();
	crl::async([=] {
		QDir(folder).removeRecursively();
	});
}

} // namespace Bulk
