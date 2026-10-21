# Telegram Desktop – Bulk build

This is the official Telegram Desktop for Windows (x64), built from the
official source code, with a few changes for downloading many files at once.
Everything else works exactly like the official app.

## What is different

**Select up to 1000 items instead of 100**

- In chats, groups and channels: click a message or drag over several
  messages, right-click a message and choose "Select up to this message".
- In the shared media of a chat (click the chat name, then Media / Files /
  Videos): select items with right-click → Select, drag across items,
  "Select up to this message" or "Select all shown".
  Only the items that are already loaded can be selected, so scroll down
  to load more before using "Select all shown".

**Download everything that is selected in one step**

- Chat: the new **Download** button at the top (next to Forward / Delete),
  or right-click → Download Selected.
- Shared media: the new **download icon** at the top, or right-click →
  Download Selected.
- A small window shows how many files and how much space, the folder (you
  can change it), and two options:
  - put the files into a subfolder named after the chat;
  - skip files that are already in the folder (useful if you download the
    same chat again).
- Files go through the normal Telegram downloader, so with Telegram Premium
  you get the Premium download speed. Progress is shown in the Downloads
  bar at the bottom of the chat list. Photos are saved a few at a time.
- Messages without files, and chats where the owner disabled saving
  ("protected content"), are skipped.

**Faster chat export**

Settings → Advanced → Export Telegram data (or ⋮ → Export chat history)
now downloads each file in large parts, many at a time, instead of one
small part at a time. Large videos export several times faster.

**Forward and delete more than 100 messages**

These actions are sent to Telegram in groups of 100, so they keep working
with up to 1000 selected messages.

## Installing

1. Create a folder only for this app, for example `C:\Telegram Bulk\`.
2. Put `Telegram.exe` there and start it. Log in with your phone number.
3. The app keeps its data (your login) in the `tdata` folder next to
   `Telegram.exe`. It does not touch an official Telegram that may be
   installed, and both can run at the same time.

Windows may say "Windows protected your PC" because the file is not signed
by a company: click "More info" → "Run anyway".

## Safety notes

- The app talks only to Telegram, with your own API key.
- Automatic updates are disabled (an update would replace this build with
  the official one). To update, build a new version from the latest source.
- Crash reports are not sent.
- Keep the `tdata` folder private: it contains your logged-in session.
  Log out from Settings, or from another device (Settings → Devices), if
  you stop using this copy.
