# Telegram Desktop – Bulk build

This is the official Telegram Desktop for Windows (x64), built from the
official source code, with changes for downloading (and sending) many files
at once. Everything else works exactly like the official app.

## Where to find things

| What | Where |
| --- | --- |
| Progress of all bulk downloads, speed panel, settings | Main menu (☰) → **Bulk downloads** |
| Download the selected files | Chat: **Download** button at the top. Media tab: **download icon** at the top. Or right-click → Download Selected |
| Download a whole chat (no 1000 limit) | Chat menu (⋮) → **Download all media…**, or right-click in a media tab → Download all media… |
| Save new files of a chat automatically | Chat menu (⋮) → **Auto-save new files…** |
| Send a whole folder as albums | Chat menu (⋮) → **Upload a folder…** |
| Load the whole media list, jump to a date, select by size/type/date, export a CSV list | Right-click in a media tab |

## Downloading

**Select up to 1000 items instead of 100**, in chats and in the media tabs
(Media / Files / Videos…). Right-click → "Select up to this message" and
"Select all shown" make big selections quick.

**The download window** shows how many files and how much space they need,
the free space on the disk, and lets you choose:

- filters: file types, minimum/maximum size, date range, extensions
  (for example `mp4, pdf`), and "only files I have not downloaded yet";
- the order: oldest first, newest first, smallest first, largest first;
- file names: the original name, or with the date, or in folders per month
  or per type (for example `2026-10/Videos/video.mp4`);
- the folder, a subfolder named after the chat, and "skip files that are
  already in the folder".

**Download all media** downloads every photo, video, file, song, voice
message or GIF of a chat (or of one forum topic), with the same filters.
The chat is searched in the background and the downloads start right away.

**Bulk downloads window** (☰ → Bulk downloads):

- each download with files done / total, size done / total, speed and time
  left, and **Pause**, **Resume**, **Cancel**, **Open folder**;
- **Failed** list with the reason for every file, and **Retry all**;
- the **Connection and speed** panel: real download speed, open
  connections, parts in progress, whether you have Premium, and how many
  times Telegram slowed the downloads down (it only happens without
  Premium);
- unfinished folder uploads, and auto-save rules.

**Faster, steadier downloads**

- Only a few files download at the same time (3 by default, you can choose
  1–8), so each one gets the full speed and finishes sooner.
- Big files are requested in 512 KB parts instead of 128 KB, start with two
  connections and add more connections faster (up to 8).
- Telegram's CDN servers still use 128 KB parts, because they check every
  128 KB part separately.

**Reliable**

- Unfinished bulk downloads continue when you start the app again.
- Failed files are retried automatically after 5 seconds, 30 seconds and
  2 minutes; files still failing go to the Failed list. No error pop-up per
  file.
- Every finished file is checked: if its size is not what Telegram reports,
  it is deleted and downloaded again.
- Optionally a `SHA256SUMS.txt` checksum list is written when a download
  finishes.
- The app remembers every file it saved (by Telegram's file id). The same
  video forwarded to five channels is downloaded once; you can choose to
  skip it, copy it into the new folder, or download it again.
- Downloads pause when the disk is almost full (2 GB by default) and
  continue when there is space again.

**Settings** (Bulk downloads → Settings): files at the same time, speed
limit (for all downloads), download hours (for example only at night),
caption saved as a `.txt` next to each file, message date as the file date,
free space to keep, clear Telegram's media cache.

## Media tabs

Right-click in a media tab:

- **Load all** loads the whole list instead of a few screens, so "Select all
  shown" and the filters see everything.
- **Jump to date…** shows the media around a chosen day.
- **Select by filter…** selects the loaded files by type, size, date,
  extension, or "not downloaded yet".
- **Export list as CSV…** saves the name, size, date, type and message link
  of the selected files (or of the whole loaded list).
- Video thumbnails show the file size next to the duration (can be turned
  off in the settings).

Sorting the media grid itself by size is not possible (Telegram sends the
list by date); use "Smallest first / Largest first" in the download window.

## Sending

**Upload a folder** sends all files of a folder (optionally with subfolders)
as albums of up to 10, one album after another. If Telegram is closed in
the middle, it offers to continue on the next start.

**Privacy**: photos and videos you send *as files* (JPEG, PNG, MP4, MOV) are
sent without GPS location, camera details and dates. The original file on
your disk is never changed; a cleaned copy is sent. Photos sent compressed
are cleaned by Telegram anyway. (Can be turned off in the settings.)

## Exporting chats

Settings → Advanced → Export Telegram data (or ⋮ → Export chat history)
downloads big files in large parts, many at a time, and small files
(photos) several at a time.

## Safety

- **Disguised files**: before opening a file such as `photo.jpg.exe`,
  `video.mp4          .scr`, or a name with hidden right-to-left characters,
  the app always asks and shows the real file type.
- Chats where the owner disabled saving ("protected content") are skipped.
- The app talks only to Telegram with your own API key. It checks GitHub for
  a newer build of your own fork and only tells you about it; it never
  downloads or installs anything (can be turned off in the settings).
- Automatic updates are disabled (an update would replace this build with
  the official one). Crash reports are not sent.
- Keep the `tdata` folder private: it contains your logged-in session.

## Installing

1. Create a folder only for this app, for example `C:\Telegram Bulk\`.
2. Put `Telegram.exe` there and start it. Log in with your phone number.
   If you used the previous bulk build, replace its `Telegram.exe`: your
   login and settings in `tdata` stay.
3. The app keeps its data in the `tdata` folder next to `Telegram.exe`
   (bulk downloads in `tdata\bulk`). It does not touch an official Telegram
   that may be installed.

Windows may say "Windows protected your PC" because the file is not signed
by a company: click "More info" → "Run anyway".
