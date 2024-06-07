/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
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

#include "androidavdmanager.h"
#include "androiddevice.h"
#include "androiddeviceinfo.h"
#include "androidglobal.h"
#include "androidmanager.h"
#include "androidqmlpreviewworker.h"

#include "coreplugin/icore.h"
#include "projectexplorer/buildsystem.h"
#include "projectexplorer/kit.h"
#include "projectexplorer/project.h"
#include "projectexplorer/target.h"
#include "qmljs/qmljssimplereader.h"
#include "qmlprojectmanager/qmlprojectconstants.h"
#include "qmlprojectmanager/qmlprojectmanagerconstants.h"
#include "qtsupport/baseqtversion.h"
#include "qtsupport/qtkitinformation.h"

#include <QThread>
#include <QTemporaryDir>
#include <QImageReader>
#include <QtConcurrent>

namespace {
static Q_LOGGING_CATEGORY(lcWarning, "qtc.android.qmlpreview", QtWarningMsg)
}

namespace Android {
namespace Internal {

using namespace Utils;

class ApkInfo {
public:
    static const QStringList abis;
    static const char appId[];
    static const char uploadDir[];
    static const char activityId[];
};

const QStringList ApkInfo::abis = {ProjectExplorer::Constants::ANDROID_ABI_X86,
                                   ProjectExplorer::Constants::ANDROID_ABI_X86_64,
                                   ProjectExplorer::Constants::ANDROID_ABI_ARM64_V8A,
                                   ProjectExplorer::Constants::ANDROID_ABI_ARMEABI_V7A};
#define APP_ID "io.qt.designviewer"
const char ApkInfo::appId[] = APP_ID;
const char ApkInfo::uploadDir[] = "/data/local/tmp/" APP_ID "/";
const char ApkInfo::activityId[] = APP_ID "/org.qtproject.qt5.android.bindings.QtActivity";

const char packageSuffix[] = ".qmlrc";

static FilePath viewerApkPath(const QString &avdAbi)
{
    if (avdAbi.isEmpty())
        return {};

    if (ApkInfo::abis.contains(avdAbi))
        return Core::ICore::resourcePath(QString("android/designviewer_%1.apk").arg(avdAbi));
    return {};
}

static const QList<FilePath> uniqueFolders(const FilePaths &filePaths)
{
    QSet<FilePath> folders;
    for (const FilePath &file : filePaths) {
        folders.insert(file.parentDir());
    }
    return folders.values();
}

static bool shouldAdbRetry(const QString &msg)
{
    return msg == "error: closed"
           || msg == "adb: device offline"
           || msg.contains("doesn't match this client")
           || msg.contains("not found")
           || msg.contains("Can't find service: package");
};

static Android::SdkToolResult runAdbCommand(const QString &dev, const QStringList &arguments)
{
    QStringList args;
    if (!dev.isEmpty())
        args << AndroidDeviceInfo::adbSelector(dev);
    args << arguments;

    Android::SdkToolResult result = AndroidManager::runAdbCommand(args);
    if (result.success() || !shouldAdbRetry(result.stdErr()))
        return result;
    // On Failure we shall repeat in a separate thread
    qCDebug(lcWarning()) << "Retrying on:" << result.stdErr();
    static const int numRetries = 5;
    QFuture<Android::SdkToolResult> asyncResult = QtConcurrent::run([args, result] {
        Android::SdkToolResult res(result);
        for (int i = 0 ; i < numRetries && !res.success() && shouldAdbRetry(res.stdErr()); ++i) {
            QThread::sleep(1);
            res = AndroidManager::runAdbCommand(args);
        }
        return res;
    });

    while (asyncResult.isRunning()) {
        QCoreApplication::instance()->processEvents(QEventLoop::AllEvents, 100);
    }
    return asyncResult.result();
}

static Android::SdkToolResult runAdbShellCommand(const QString &dev, const QStringList &arguments)
{
    QStringList shellCmd{"shell"};
    return runAdbCommand(dev, shellCmd + arguments);
}

static QString startAvd(const AndroidAvdManager &avd, const QString &name)
{
    if (!avd.findAvd(name).isEmpty() || avd.startAvdAsync(name)) {
        QFuture<QString> asyncRes = QtConcurrent::run([avd, name] {
            return avd.waitForAvd(name, []{return false;});
        });
        while (asyncRes.isRunning()) {
            QCoreApplication::instance()->processEvents(QEventLoop::AllEvents, 100);
        }
        return asyncRes.result();
    }
    return {};
}

static int pidofPreview(const QString &dev)
{
    const QStringList command{"pidof", ApkInfo::appId};
    SdkToolResult res = runAdbShellCommand(dev, command);
    return res.success() ? res.stdOut().toInt() : -1;
}

static bool isPreviewRunning(const QString &dev, int lastKnownPid = -1)
{
    int pid = pidofPreview(dev);
    return (lastKnownPid > 1) ? lastKnownPid == pid : pid > 1;
}

AndroidQmlPreviewWorker::AndroidQmlPreviewWorker(ProjectExplorer::RunControl *runControl)
    : ProjectExplorer::RunWorker(runControl)
    , m_rc(runControl)
    , m_config(AndroidConfigurations::currentConfig())
{
}

void AndroidQmlPreviewWorker::start()
{
    UploadInfo transfer;
    bool res = ensureAvdIsRunning()
               && elevateAdb()
               && checkAndInstallPreviewApp()
               && prepareUpload(transfer)
               && uploadFiles(transfer)
               && runPreviewApp(transfer);

    if (!res) {
        reportFailure();
        return;
    }
    reportStarted();

    //Thread to monitor preview life
    QtConcurrent::run([this]() {
        QElapsedTimer timer;
        timer.start();
        while (runControl() && runControl()->isRunning()) {
            if (m_viewerPid == -1) {
                m_viewerPid = pidofPreview(m_devInfo.serialNumber);
            } else if (timer.elapsed() > 2000) {
                if (!isPreviewRunning(m_devInfo.serialNumber, m_viewerPid))
                    QMetaObject::invokeMethod(this, &AndroidQmlPreviewWorker::stop);
                timer.restart();
            }
            QThread::msleep(100);
        }
    });
}

void AndroidQmlPreviewWorker::stop()
{
    if (!isPreviewRunning(m_devInfo.serialNumber, m_viewerPid) || stopPreviewApp())
        appendMessage(tr("Qt Design Viewer has been stopped."), NormalMessageFormat);
    m_viewerPid = -1;
    reportStopped();
}

bool AndroidQmlPreviewWorker::elevateAdb()
{
    SdkToolResult res = runAdbCommand(m_devInfo.serialNumber, {"root"});
    if (!res.success())
        appendMessage(res.stdErr(), ErrorMessageFormat);
    return res.success();
}

bool AndroidQmlPreviewWorker::unelevateAdb()
{
    SdkToolResult res = runAdbCommand(m_devInfo.serialNumber, {"unroot"});
    if (!res.success())
        appendMessage(res.stdErr(), ErrorMessageFormat);
    return res.success();
}

bool AndroidQmlPreviewWorker::ensureAvdIsRunning()
{
    AndroidAvdManager avdMan(m_config);
    QString devSN = AndroidManager::deviceSerialNumber(m_rc->target());

    if (devSN.isEmpty())
        devSN = m_devInfo.serialNumber;

    if (!avdMan.isAvdBooted(devSN)) {
        m_devInfo = {};
        int minTargetApi = AndroidManager::minimumSDK(m_rc->target());
        AndroidDeviceInfo devInfoLocal = AndroidConfigurations::showDeviceDialog(m_rc->project(),
                                                                                 minTargetApi,
                                                                                 ApkInfo::abis);
        if (devInfoLocal.isValid()) {
            appendMessage(tr("Launching AVD."), NormalMessageFormat);
            devInfoLocal.serialNumber = startAvd(avdMan, devInfoLocal.avdname);

            if (!devInfoLocal.serialNumber.isEmpty()) {
                m_devInfo = devInfoLocal;
                m_avdAbis = m_config.getAbis(m_config.adbToolPath(), m_devInfo.serialNumber);
            } else {
                appendMessage(tr("Could not run AVD."), ErrorMessageFormat);
            }
            return !devInfoLocal.serialNumber.isEmpty();
        } else {
            appendMessage(tr("No valid AVD has been selected."), ErrorMessageFormat);
        }
        return false;
    }
    m_avdAbis = m_config.getAbis(m_config.adbToolPath(), m_devInfo.serialNumber);
    return true;
}

bool AndroidQmlPreviewWorker::checkAndInstallPreviewApp()
{
    const QStringList command {"pm", "list", "packages", ApkInfo::appId};
    appendMessage(tr("Checking if Qt Design Viewer app is installed."), NormalMessageFormat);
    SdkToolResult res = runAdbShellCommand(m_devInfo.serialNumber, command);
    if (!res.success()) {
        appendMessage(res.stdErr(), ErrorMessageFormat);
        return false;
    }

    if (res.stdOut().isEmpty()) {
        if (m_avdAbis.isEmpty()) {
            appendMessage(tr("ABI of the selected device is unknown. Cannot install APK."),
                          ErrorMessageFormat);
            return false;
        }
        FilePath apkPath = viewerApkPath(m_avdAbis.first());
        if (!apkPath.exists()) {
            appendMessage(tr("Cannot install Qt Design Viewer APK (%1). Appropriate file was not "
                             "found in plugin folders.").arg(m_avdAbis.first()),
                          ErrorMessageFormat);
            return false;
        }

        appendMessage(tr("Installing Qt Design Viewer apk."), NormalMessageFormat);

        SdkToolResult res = runAdbCommand(m_devInfo.serialNumber, {"install", apkPath.toString()});
        if (!res.success()) {
            appendMessage(res.stdErr(), StdErrFormat);

            return false;
        }
    }
    return true;
}

bool AndroidQmlPreviewWorker::prepareUpload(UploadInfo &transfer)
{
    if (m_rc->project()->id() == QmlProjectManager::Constants::QML_PROJECT_ID) {
        const auto bs = m_rc->target()->buildSystem();
        if (bs) {
            transfer.importPaths = bs->additionalData(QmlProjectManager::Constants::
                                                          customImportPaths).toStringList();
            transfer.projectFolder = FilePath::fromString(
                bs->additionalData(QmlProjectManager::Constants::canonicalProjectDir).toString());
            transfer.mainQmlFile = FilePath::fromString(
                bs->additionalData(QmlProjectManager::Constants::mainFilePath).toString());
            transfer.projectFiles = m_rc->project()->files(ProjectExplorer::Project::SourceFiles);

            //Add everything missing from imports folders
            for (const QString &path : qAsConst(transfer.importPaths) ) {
                QDirIterator it((transfer.projectFolder.absoluteFilePath() + "/" + path).toDir(),
                                 QDirIterator::Subdirectories);

                while (it.hasNext()) {
                    QFileInfo fi(it.next());
                    if (fi.isFile() && !transfer.projectFiles.contains(FilePath::fromFileInfo(fi)))
                        transfer.projectFiles.append(FilePath::fromFileInfo(fi));
                }
            }
            return true;
        }
    } else {
        const FilePaths allFiles = m_rc->project()->files(m_rc->project()->SourceFiles);
        FilePaths filesToExport = Utils::filtered(allFiles,[](const FilePath &path) {
            return path.suffix() == "qmlproject";});

        if (filesToExport.size() > 1) {
            appendMessage(tr("Too many qmlproject files in project. Could not decide to pick one."),
                          ErrorMessageFormat);
        } else if (filesToExport.size() < 1) {
            appendMessage(tr("No qmlproject file found among project files."), ErrorMessageFormat);
        } else {
            QmlJS::SimpleReader qmlReader;
            QFileInfo qmlprojectFile = filesToExport.first().toFileInfo();
            const QmlJS::SimpleReaderNode::Ptr rootNode = qmlReader.readFile(qmlprojectFile.
                                                                             filePath());

            if (!qmlReader.errors().isEmpty() || !rootNode->isValid()) {
                appendMessage(tr("Could not parse %1").arg(qmlprojectFile.fileName()),
                              ErrorMessageFormat);
                return false;
            }

            if (rootNode->name() == QLatin1String("Project")) {
                QStringList extensions{"qml", "js", "css", "ttf", "otf", "conf", "qmlproject"};
                const QList<QByteArray> gfxExt = QImageReader::supportedImageFormats();
                extensions += Utils::transform(gfxExt,
                                               [](const auto byteArray) {
                                                   return QString::fromLocal8Bit(byteArray);});

                transfer.projectFolder = FilePath::fromString(qmlprojectFile.absolutePath());
                const QVariant mainFileProperty = rootNode->property(QLatin1String("mainFile"));
                if (mainFileProperty.isValid()) {
                    transfer.mainQmlFile = transfer.
                                           projectFolder.
                                           absoluteFilePath(FilePath::fromString(
                                               mainFileProperty.toString()));
                }

                const QVariant importPathsProperty = rootNode->
                                                     property(QLatin1String("importPaths"));
                if (importPathsProperty.isValid())
                    transfer.importPaths = importPathsProperty.toStringList();

                QStringList fullPathImportPaths;

                QDirIterator it(qmlprojectFile.dir(), QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    QFileInfo fi(it.next());
                    if (extensions.contains(fi.suffix()) || fi.fileName() == "qmldir") {
                        transfer.projectFiles.append(FilePath::fromFileInfo(fi));
                    }
                }
                return true;
            }
            return false;
        }
    }
    appendMessage(tr("Could not gather information on project files."), ErrorMessageFormat);
    return false;
}

FilePath AndroidQmlPreviewWorker::createQmlrcFile(const FilePath &workFolder,
                                                 const QString &basename)
{
    QtSupport::BaseQtVersion *qtVersion = QtSupport::QtKitAspect::qtVersion(m_rc->kit());
    FilePath rccBinary = qtVersion->rccFilePath();
    QtcProcess rccProcess;
    FilePath qrcPath = FilePath::fromString(basename);
    if (qrcPath.suffix() != "qrc")
        qrcPath = qrcPath + ".qrc";
    FilePath qmlrcPath = FilePath::fromString(QDir::tempPath() + "/" + basename + packageSuffix);

    rccProcess.setWorkingDirectory(workFolder);

    const QStringList arguments[2] = {{"--project", "--output", qrcPath.fileName()},
                                      {"--binary", "--output", qmlrcPath.path(),
                                       qrcPath.fileName()}};
    for (const auto &arguments : arguments) {
        rccProcess.setCommand({rccBinary, arguments});
        rccProcess.start();
        if (!rccProcess.waitForStarted()) {
            appendMessage(tr("Could not to create file for Qt Design Viewer \"%1\"").
                          arg(rccProcess.commandLine().toUserOutput()), StdErrFormat);
            return {};
        }
        QByteArray stdOut;
        QByteArray stdErr;
        if (!rccProcess.readDataFromProcess(30, &stdOut, &stdErr, true)) {
            rccProcess.stopProcess();
            appendMessage(tr("A timeout occurred running \"%1\"").
                          arg(rccProcess.commandLine().toUserOutput()), StdErrFormat);
            return {};
        }
        if (!stdOut.trimmed().isEmpty())
            appendMessage(QString::fromLocal8Bit(stdOut), StdErrFormat);

        if (!stdErr.trimmed().isEmpty())
            appendMessage(QString::fromLocal8Bit(stdErr), StdErrFormat);

        if (rccProcess.exitStatus() != QProcess::NormalExit) {
            appendMessage(tr("Crash while creating file for Qt Design Viewer \"%1\"").
                          arg(rccProcess.commandLine().toUserOutput()), StdErrFormat);
            return {};
        }
        if (rccProcess.exitCode() != 0) {
            appendMessage(tr("Creating file for Qt Design Viewer failed. \"%1\" (exit code %2).").
                          arg(rccProcess.commandLine().toUserOutput()).
                          arg(rccProcess.exitCode()), StdErrFormat);
            return {};
        }
    }
    return qmlrcPath;
}

bool AndroidQmlPreviewWorker::uploadFiles(const UploadInfo &transfer)
{
    QTemporaryDir tmp;
    if (tmp.isValid()) {
        appendMessage(tr("Uploading files."), NormalMessageFormat);

        QDir dir(tmp.path());
        if (dir.mkpath(transfer.projectFolder.fileName())) {
            dir.cd(transfer.projectFolder.fileName());
            FilePath tmpDir = FilePath::fromString(dir.path());
            //Create all needed folders in temporary folder
            for (const FilePath &folder : uniqueFolders(transfer.projectFiles)) {
                if (folder != transfer.projectFolder)
                    dir.mkpath(folder.relativePath(transfer.projectFolder).toString());
            }
            //copy all project files to already existing folders
            for (const FilePath &file : transfer.projectFiles)
                file.copyFile(tmpDir.absoluteFilePath(file.relativeChildPath(transfer.
                                                                             projectFolder)));

            FilePath qresPath = createQmlrcFile(tmpDir, transfer.mainQmlFile.baseName());
            if (!qresPath.exists())
                return false;

            runAdbShellCommand(m_devInfo.serialNumber, {"mkdir", "-p", ApkInfo::uploadDir});

            SdkToolResult res = runAdbCommand(m_devInfo.serialNumber,
                                              {"push", qresPath.absoluteFilePath().toString(),
                                               ApkInfo::uploadDir});
            if (!res.success()) {
                appendMessage(res.stdOut(), ErrorMessageFormat);
                appendMessage(res.stdErr(), ErrorMessageFormat);
            }
            return res.success();
        }
    }
    return false;
}

bool AndroidQmlPreviewWorker::runPreviewApp(const UploadInfo &transfer)
{
    stopPreviewApp();
    appendMessage(tr("Starting Qt Design Viewer."), NormalMessageFormat);
    QDir destDir(ApkInfo::uploadDir);
    const QStringList command{"am", "start",
                              "-n", ApkInfo::activityId,
                              "-e", "extraappparams",
                              QString::fromLatin1(
                                  destDir.filePath(transfer.mainQmlFile.baseName() + packageSuffix).
                                  toUtf8().
                                  toBase64())};
    SdkToolResult res = runAdbShellCommand(m_devInfo.serialNumber, command);
    if (!res.success()) {
        appendMessage(res.stdErr(), ErrorMessageFormat);
        return res.success();
    }
    appendMessage(tr("Qt Design Viewer is running."), NormalMessageFormat);
    m_viewerPid = pidofPreview(m_devInfo.serialNumber);
    return true;
}

bool AndroidQmlPreviewWorker::stopPreviewApp()
{
    const QStringList command{"am", "force-stop", ApkInfo::appId};
    SdkToolResult res = runAdbShellCommand(m_devInfo.serialNumber, command);
    if (!res.success()) {
        appendMessage(res.stdErr(), ErrorMessageFormat);
        return res.success();
    }
    return true;
}

}
}
