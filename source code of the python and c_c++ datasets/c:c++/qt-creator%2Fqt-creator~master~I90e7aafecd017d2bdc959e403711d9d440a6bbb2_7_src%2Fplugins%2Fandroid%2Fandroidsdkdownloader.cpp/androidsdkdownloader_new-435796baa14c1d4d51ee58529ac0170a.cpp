/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/
#include "androidsdkdownloader.h"

#include <QDir>
#include <QDirIterator>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>

namespace {
Q_LOGGING_CATEGORY(sdkDownloaderLog, "qtc.android.sdkDownloader", QtWarningMsg)
}

/**
 * @class SdkDownloader
 * @brief Download Android SDK tools package from within Qt Creator.
 */
AndroidSdkDownloader::AndroidSdkDownloader()
{
    connect(&m_manager, SIGNAL(finished(QNetworkReply *)), SLOT(downloadFinished(QNetworkReply *)));
}

void AndroidSdkDownloader::sslErrors(const QList<QSslError> &sslErrors)
{
#if QT_CONFIG(ssl)
    for (const QSslError &error : sslErrors)
        qCDebug(sdkDownloaderLog, "SSL error: %s\n", qPrintable(error.errorString()));
#else
    Q_UNUSED(sslErrors);
#endif
}

static void setSdkFilesExecPermission( const QString &sdkExtractPath)
{
    QDirIterator it(sdkExtractPath + QDir::separator() + "tools", QStringList() << "*",
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (!file.fileName().contains('.')) {
            QFlags<QFileDevice::Permission> currentPermissions
                = file.permissions();
            file.setPermissions(currentPermissions | QFileDevice::ExeOwner);
        }
    }
}

void AndroidSdkDownloader::downloadAndExtractSdk(const QUrl &sdkUrl, const QString &jdkPath, const QString &sdkExtractPath)
{
    if (sdkUrl.isEmpty()) {
        QMessageBox msgBox(QMessageBox::Warning,
                           tr("Download SDK Tools"),
                           tr("Couldn't get a valid SDK Tools download URL."));
        msgBox.exec();
        return;
    }

    QUrl url(sdkUrl);
    QNetworkRequest request(url);
    m_reply = m_manager.get(request);

#if QT_CONFIG(ssl)
    connect(m_reply, SIGNAL(sslErrors(QList<QSslError>)), SLOT(sslErrors(QList<QSslError>)));
#endif

    m_progressDialog = new QProgressDialog("Downloading SDK Tools package...", "Cancel", 0, 100);
    m_progressDialog->setWindowModality(Qt::WindowModal);
    m_progressDialog->setWindowTitle("Download SDK Tools");
    m_progressDialog->setFixedSize(m_progressDialog->sizeHint());

    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 max) {
        m_progressDialog->setRange(0, max);
        m_progressDialog->setValue(received);
    });

    connect(m_progressDialog, SIGNAL(canceled()), this, SLOT(cancel()));

    connect(this, &AndroidSdkDownloader::sdkPackageWriteFinished, this, [this, jdkPath, sdkExtractPath]() {
        if (extractSdk(jdkPath, sdkExtractPath)) {
            setSdkFilesExecPermission(sdkExtractPath);
            emit sdkExtracted();
        }
    });
}

bool AndroidSdkDownloader::extractSdk(const QString &jdkPath, const QString &sdkExtractPath)
{
    if (!QDir(sdkExtractPath).exists()) {
        if (!QDir().mkdir(sdkExtractPath))
            qCDebug(sdkDownloaderLog, "Coud not create SDK folder (%s)", qPrintable(sdkExtractPath));
    }

    QString jarCmdPath = jdkPath + QDir::separator() + "bin" + QDir::separator() + "jar";
    QStringList args;
    args << "xf" << m_sdkFilename;

    QProcess *jarExtractProc = new QProcess();
    jarExtractProc->setWorkingDirectory(sdkExtractPath);
    jarExtractProc->start(jarCmdPath, args);
    jarExtractProc->waitForFinished();
    jarExtractProc->close();

    return jarExtractProc->exitCode() ? false : true;
}

void AndroidSdkDownloader::cancel()
{
    m_reply->abort();
    m_reply->deleteLater();
    m_progressDialog->hide();
}

QString AndroidSdkDownloader::getSaveFilename(const QUrl &url)
{
    QString path = url.path();
    QString basename = QFileInfo(path).fileName();

    if (basename.isEmpty())
        basename = "sdk-tools.zip";

    if (QFile::exists(basename)) {
        int i = 0;
        basename += '.';
        while (QFile::exists(basename + QString::number(i)))
            ++i;
        basename += QString::number(i);
    }

    QString fullPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                       + QDir::separator() + basename;
    return fullPath;
}

bool AndroidSdkDownloader::saveToDisk(const QString &filename, QIODevice *data)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly)) {
        qCDebug(sdkDownloaderLog,
                "Could not open %s for writing: %s",
                qPrintable(filename),
                qPrintable(file.errorString()));
        return false;
    }

    file.write(data->readAll());
    file.close();

    return true;
}

bool AndroidSdkDownloader::isHttpRedirect(QNetworkReply *reply)
{
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 305
           || statusCode == 307 || statusCode == 308;
}

void AndroidSdkDownloader::downloadFinished(QNetworkReply *reply)
{
    QUrl url = reply->url();
    if (reply->error()) {
        qCDebug(sdkDownloaderLog,
                "Downloading Android SDK Tools from URL (%s) failed: %s",
                url.toEncoded().constData(),
                qPrintable(reply->errorString()));
    } else {
        if (isHttpRedirect(reply)) {
            qCDebug(sdkDownloaderLog,
                    "Download (%s) was redirected",
                    url.toEncoded().constData());
        } else {
            m_sdkFilename = getSaveFilename(url);
            if (saveToDisk(m_sdkFilename, reply)) {
                qCDebug(sdkDownloaderLog,
                        "Download of Android SDK Tools saved to (%s)",
                        qPrintable(m_sdkFilename));
                emit sdkPackageWriteFinished();
            }
        }
    }

    reply->deleteLater();
}
