/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk/bulk_update_check.h"

#include "bulk/bulk_settings.h"
#include "lang/lang_keys.h"
#include "ui/basic_click_handlers.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_session_controller.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#ifdef Q_OS_WIN
// Qt Network looks up DNS records with dnsapi.dll.
#pragma comment(lib, "dnsapi.lib")
#endif // Q_OS_WIN

#define BULK_STRINGIFY_IMPL(x) #x
#define BULK_STRINGIFY(x) BULK_STRINGIFY_IMPL(x)

namespace Bulk {
namespace {

constexpr auto kTimeout = 20 * 1000;

[[nodiscard]] QNetworkAccessManager &Network() {
	// Never destroyed, it would outlive the application object otherwise.
	static const auto result = new QNetworkAccessManager();
	return *result;
}

} // namespace

QString BuildCommit() {
#ifdef TDESKTOP_BULK_BUILD_SHA
	return QString::fromLatin1(BULK_STRINGIFY(TDESKTOP_BULK_BUILD_SHA));
#else // TDESKTOP_BULK_BUILD_SHA
	return QString();
#endif // TDESKTOP_BULK_BUILD_SHA
}

int BuildRunNumber() {
#ifdef TDESKTOP_BULK_BUILD_RUN
	return int(TDESKTOP_BULK_BUILD_RUN);
#else // TDESKTOP_BULK_BUILD_RUN
	return 0;
#endif // TDESKTOP_BULK_BUILD_RUN
}

QString BuildRepository() {
#ifdef TDESKTOP_BULK_REPO
	return QString::fromLatin1(BULK_STRINGIFY(TDESKTOP_BULK_REPO));
#else // TDESKTOP_BULK_REPO
	return u"Danmrk090/tdesktop"_q;
#endif // TDESKTOP_BULK_REPO
}

void CheckForUpdates(not_null<Window::SessionController*> window) {
	static auto checked = false;
	if (checked || !Current().checkUpdates) {
		return;
	}
	checked = true;
	CheckForUpdatesNow(window, false);
}

void CheckForUpdatesNow(
		not_null<Window::SessionController*> window,
		bool manual) {
	const auto repository = BuildRepository();
	const auto commit = BuildCommit();
	const auto run = BuildRunNumber();
	if (repository.isEmpty()) {
		return;
	}
	auto request = QNetworkRequest(QUrl(
		u"https://api.github.com/repos/%1/actions/workflows/"
		"bulk-windows.yml/runs?branch=bulk-download&status=success"
		"&per_page=1"_q.arg(repository)));
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("User-Agent", "TelegramBulk");
	request.setTransferTimeout(kTimeout);
	const auto reply = Network().get(request);
	const auto weak = base::make_weak(window);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto document = QJsonDocument::fromJson(reply->readAll());
		const auto runs = document.object().value(
			u"workflow_runs"_q).toArray();
		if (reply->error() != QNetworkReply::NoError || runs.isEmpty()) {
			if (manual) {
				strong->show(Ui::MakeInformBox(
					tr::lng_bulk_update_failed()));
			}
			return;
		}
		const auto latest = runs.first().toObject();
		const auto sha = latest.value(u"head_sha"_q).toString();
		const auto number = latest.value(u"run_number"_q).toInt();
		const auto url = latest.value(u"html_url"_q).toString();
		const auto newer = !sha.isEmpty()
			&& (commit.isEmpty() || !sha.startsWith(commit))
			&& (number > run);
		if (!newer) {
			if (manual) {
				strong->show(Ui::MakeInformBox(
					tr::lng_bulk_update_latest()));
			}
			return;
		}
		strong->show(Ui::MakeConfirmBox({
			.text = tr::lng_bulk_update_available(
				tr::now,
				lt_number,
				QString::number(number)),
			.confirmed = [=](Fn<void()> close) {
				UrlClickHandler::Open(url);
				close();
			},
			.confirmText = tr::lng_bulk_update_open(),
		}));
	});
}

} // namespace Bulk
