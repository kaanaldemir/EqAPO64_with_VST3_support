/*
	This file is part of EqualizerAPO, a system-wide equalizer.
	Copyright (C) 2024  Jonas Dahlinger

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "stdafx.h"
#include <QDir>
#include <QStyleHints>
#include <QCommandLineParser>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QtWidgets/QApplication>
#include <helpers/TaskSchedulerHelper.h>
#include "UpdateChecker.h"
#include "BuildMetadata.h"
#include "version.h"

using namespace std::chrono_literals;

void showFailureMessage(QString message, QString title);
QString getInstalledVersion();
QString getUpdateCheckUrl();
QString getNewestVersion(const QJsonDocument& doc);

int main(int argc, char* argv[])
{
	QCoreApplication::addLibraryPath("qt");

	QApplication app(argc, argv);
	if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark)
		app.setStyle("fusion");

	QLocale::setDefault(QLocale::system());

	QTranslator qtTranslator;
	if (qtTranslator.load(QLocale(), ":/translations/qtbase", "_"))
		app.installTranslator(&qtTranslator);

	QTranslator updateCheckerTranslator;
	if (updateCheckerTranslator.load(
		QLocale(), ":/translations/UpdateChecker", "_"))
		app.installTranslator(&updateCheckerTranslator);

	QCommandLineParser parser;
	QCommandLineOption autoOption("a", "Automatic mode (no dialog if no new version, respect skip version, only check every 24 hours)");
	QCommandLineOption installOption("i", "Install scheduled task");
	QCommandLineOption uninstallOption("u", "Uninstall scheduled task");
	parser.addOptions(QList<QCommandLineOption>() << autoOption << installOption << uninstallOption);
	parser.process(app);
	if (parser.isSet(installOption))
	{
		try
		{
			QString programPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
			QString workingDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
			TaskSchedulerHelper::scheduleAtLogon(L"EqualizerAPOUpdateChecker", programPath.toStdWString(), L"-a", workingDir.toStdWString());
		}
		catch (TaskSchedulerException e)
		{
			QMessageBox::critical(nullptr, UpdateChecker::tr("Error installing Update Checker"), QString::fromStdWString(e.getMessage()));
			return 2;
		}
		return 0;
	}
	if (parser.isSet(uninstallOption))
	{
		try
		{
			TaskSchedulerHelper::unschedule(L"EqualizerAPOUpdateChecker");
		}
		catch (TaskSchedulerException e)
		{
			QMessageBox::critical(nullptr, UpdateChecker::tr("Error uninstalling Update Checker"), QString::fromStdWString(e.getMessage()));
			return 2;
		}
		return 0;
	}
	bool autoMode = parser.isSet(autoOption);

	QString version = getInstalledVersion();

	QString url = getUpdateCheckUrl();
	if (autoMode)
	{
		QSettings settings(QString::fromWCharArray(UPDATE_CHECKER_REGPATH), QSettings::NativeFormat);
		QDateTime lastCheckDate = settings.value("lastCheckDate").toDateTime();

		if (lastCheckDate.isValid() && lastCheckDate.toUTC().daysTo(QDateTime::currentDateTimeUtc()) < 1)
			return 1;
		settings.setValue("lastCheckDate", QDateTime::currentDateTime(QTimeZone::systemTimeZone()).toString(Qt::DateFormat::ISODate));

	}

	QNetworkAccessManager manager;
	QNetworkReply* reply = nullptr;
	int tries = autoMode ? 10 : 1;
	while (tries-- > 0)
	{
		QNetworkRequest request(QUrl(url, QUrl::StrictMode));
		reply = manager.get(request);
		QEventLoop loop;
		QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		QTimer timer;
		timer.setInterval(10s);
		timer.setSingleShot(true);
		QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
		loop.exec();

		if (reply->isFinished() && reply->error() != QNetworkReply::HostNotFoundError)
			break;
		else if (tries > 0)
			QThread::sleep(5);
	}

	int result = 0;
	if (reply != nullptr && reply->isFinished())
	{
		if (reply->error() != QNetworkReply::NoError)
		{
			showFailureMessage(reply->errorString(), UpdateChecker::tr("Error while checking for update"));
		}
		else
		{
			QByteArray json = reply->readAll();
			if (json.isEmpty())
			{
				if (!autoMode)
					QMessageBox::information(nullptr, UpdateChecker::tr("No update available"), UpdateChecker::tr("The installed version %0 of Equalizer APO is up to date.").arg(version));
			}
			else
			{
				QJsonParseError error;
				QJsonDocument doc = QJsonDocument::fromJson(json, &error);
				if (error.error != QJsonParseError::NoError)
				{
					showFailureMessage(error.errorString(), UpdateChecker::tr("Error while reading response of update check"));
				}
				else
				{
					QString newestVersion = getNewestVersion(doc);
					QSettings settings(QString::fromWCharArray(UPDATE_CHECKER_REGPATH), QSettings::NativeFormat);
					QString skipVersion = settings.value("skipVersion").toString();
					if (newestVersion.isEmpty() || newestVersion == version || (autoMode && newestVersion == skipVersion))
					{
						if (!autoMode)
							QMessageBox::information(nullptr, UpdateChecker::tr("No update available"), UpdateChecker::tr("The installed version %0 of Equalizer APO is up to date.").arg(version));
					}
					else
					{
						UpdateChecker dialog(nullptr, doc);
						dialog.show();
						result = app.exec();
					}
				}
			}
		}
	}

	return result;
}

QString getInstalledVersion()
{
	QString version = QString("%0.%1").arg(MAJOR).arg(MINOR);
	if (REVISION != 0)
		version += QString(".%0").arg(REVISION);

	QString flavor = QString::fromUtf8(EAPO_BUILD_FLAVOR).trimmed();
	QString commit = QString::fromUtf8(EAPO_BUILD_COMMIT).trimmed();
	if (!flavor.isEmpty())
	{
		version += "-" + flavor;
		if (!commit.isEmpty())
			version += "." + commit;
	}

	return version;
}

QString getUpdateCheckUrl()
{
	return "https://raw.githubusercontent.com/kaanaldemir/EqAPO64_with_VST3_support/main/UpdateChecker/checkVersion-avx512.json";
}

QString getNewestVersion(const QJsonDocument& doc)
{
	QJsonArray versionsArray = doc.object().value("versions").toArray();
	if (versionsArray.isEmpty())
		return QString();

	return versionsArray.first().toObject().value("version").toString();
}

void showFailureMessage(QString message, QString title)
{
	QSettings settings(QString::fromWCharArray(UPDATE_CHECKER_REGPATH), QSettings::NativeFormat);
	bool hideFailureMessage = settings.value("hideFailureMessage").toBool();
	if (!hideFailureMessage)
	{
		QMessageBox messageBox;
		messageBox.setText(message);
		messageBox.setWindowTitle(title);
		messageBox.setIcon(QMessageBox::Icon::Critical);
		QCheckBox* hideCheckBox = new QCheckBox(UpdateChecker::tr("Don't show message for failed update check again"));
		messageBox.setCheckBox(hideCheckBox);
		messageBox.exec();

		if (hideCheckBox->isChecked())
			settings.setValue("hideFailureMessage", true);
	}
}
