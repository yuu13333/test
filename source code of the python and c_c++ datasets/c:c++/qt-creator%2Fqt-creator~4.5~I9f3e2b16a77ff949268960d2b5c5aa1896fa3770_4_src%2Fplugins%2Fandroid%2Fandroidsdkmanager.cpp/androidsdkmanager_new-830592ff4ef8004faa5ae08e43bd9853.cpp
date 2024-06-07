/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
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
#include "androidsdkmanager.h"

#include "androidconstants.h"
#include "androidmanager.h"
#include "androidtoolmanager.h"

#include "coreplugin/icore.h"
#include "utils/algorithm.h"
#include "utils/qtcassert.h"
#include "utils/runextensions.h"
#include "utils/synchronousprocess.h"
#include "utils/environment.h"

#include <QEventLoop>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

namespace {
Q_LOGGING_CATEGORY(sdkManagerLog, "qtc.android.sdkManager")
}

namespace Android {
namespace Internal {

// Though sdk manager is introduced in 25.2.3 but the verbose mode is avaialble in 25.3.0
// and android tool is supported in 25.2.3
const QVersionNumber sdkManagerIntroVersion(25, 3 ,0);

const char sdkManagerArgsKey[] = "SdkManagerArguments";

const char installLocationKey[] = "Installed Location:";
const char revisionKey[] = "Version:";
const char descriptionKey[] = "Description:";
const char commonArgsKey[] = "Common Arguments:";

const int sdkManagerCmdTimeoutS = 60;
const int sdkManagerOperationTimeoutS = 600;

using namespace Utils;
using SdkCmdFutureInterface = QFutureInterface<AndroidSdkManager::OperationOutput>;

int platformNameToApiLevel(const QString &platformName)
{
    int apiLevel = -1;
    QRegularExpression re("(android-)(?<apiLevel>[0-9]{1,})",
                          QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = re.match(platformName);
    if (match.hasMatch()) {
        QString apiLevelStr = match.captured("apiLevel");
        apiLevel = apiLevelStr.toInt();
    }
    return apiLevel;
}

/*!
    Parses the \a line for a [spaces]key[spaces]value[spaces] pattern and returns
    \c true if \a key is found, false otherwise. Result is copied into \a value.
 */
static bool valueForKey(QString key, const QString &line, QString *value = nullptr)
{
    auto trimmedInput = line.trimmed();
    if (trimmedInput.startsWith(key)) {
        if (value)
            *value = trimmedInput.section(key, 1, 1).trimmed();
        return true;
    }
    return false;
}

/*!
    Runs the \c sdkmanger tool with arguments \a args. Returns \c true if the command is
    successfully executed. Output is copied into \a output. The function blocks the calling thread.
 */
static bool sdkManagerCommand(const Utils::FileName &toolPath, const QStringList &args,
                              QString *output, int timeout = sdkManagerCmdTimeoutS)
{
    SynchronousProcess proc;
    proc.setTimeoutS(timeout);
    proc.setTimeOutMessageBoxEnabled(true);
    SynchronousProcessResponse response = proc.run(toolPath.toString(), args);
    if (output)
        *output = response.allOutput();
    return response.result == SynchronousProcessResponse::Finished;
}

/*!
    Runs the \c sdkmanger tool with arguments \a args. The operation command progress is updated in
    to the future interface \a fi and \a output is populated with command output. The command listens
    to cancel signal emmitted by \a sdkManager and kill the commands. The command is also killed
    after the lapse of \a timeout seconds. The function blocks the calling thread.
 */
static void sdkManagerCommand(const Utils::FileName &toolPath, const QStringList &args,
                              AndroidSdkManager &sdkManager,
                              SdkCmdFutureInterface &fi,
                              AndroidSdkManager::OperationOutput &output,
                              int timeout = sdkManagerOperationTimeoutS)
{
    std::unique_ptr<QProcess> proc(new QProcess());

    // Parse the %age text returned by the SdkManager command output.
    QRegularExpression reg("(?<progress>\\d*)%");
    auto parseProgress = [&proc, reg, &fi]() {
        auto stdOut = QString::fromLatin1(proc->readAllStandardOutput());
        QStringList lines = stdOut.split(QRegularExpression("[\\n\\r]"), QString::SkipEmptyParts);
        for (const QString &line : lines) {
            QRegularExpressionMatch match = reg.match(line);
            if (match.hasMatch()) {
                int progress = match.captured("progress").toInt();
                if (progress > 0 && progress <= 100)
                    fi.setProgressValue(progress);
            }
        }
    };
    QEventLoop eventLoop;
    QObject::connect(proc.get(), &QProcess::readyReadStandardOutput, parseProgress);
    QObject::connect(proc.get(), static_cast<void(QProcess::*)(int)>(&QProcess::finished),
                     [&eventLoop, &proc, &output](int exitCode) {
        output.commandOutput = QString::fromLatin1(proc->readAllStandardOutput());
        output.success = exitCode == QProcess::NormalExit;
        eventLoop.quit();
    });

    // Listen to explicit cancel request.
    QObject::connect(&sdkManager, &AndroidSdkManager::cancelActiveOperations,
                     proc.get(), &QProcess::kill);

    // execute the sdkmanager command.
    proc->start(toolPath.toString(), args);

    // setup a timeout kill timer.
    QTimer cmdKillTimer;
    cmdKillTimer.setInterval(timeout * 1000);
    cmdKillTimer.setSingleShot(true);
    QObject::connect(&cmdKillTimer, &QTimer::timeout,
                     proc.get(), &QProcess::kill);

    // Wait untill QProcess finishes.
    eventLoop.exec();
}


class AndroidSdkManagerPrivate
{
public:
    AndroidSdkManagerPrivate(AndroidSdkManager &sdkManager, const AndroidConfig &config);
    ~AndroidSdkManagerPrivate();

    AndroidSdkPackageList filteredPackages(AndroidSdkPackage::PackageState state,
                                           AndroidSdkPackage::PackageType type,
                                           bool forceUpdate = false);
    const AndroidSdkPackageList &allPackages(bool forceUpdate = false);
    void refreshSdkPackages(bool forceReload = false);

    void parseCommonArguments(QFutureInterface<QString> &fi);

    void installPackage(SdkCmdFutureInterface &fi, const QString &sdkStylePath);
    void uninstallPackage(SdkCmdFutureInterface &fi, const QString &sdkStylePath);
    void updateInstalled(SdkCmdFutureInterface &fi);

    void addWatcher(const QFuture<AndroidSdkManager::OperationOutput> &future);

    QList<QFutureWatcher<void> *> m_activeOperations;
    QStringList m_sdkManagerArgs;

private:
    void reloadSdkPackages();
    void clearPackages();
    void load();
    void save() const;

private:
    AndroidSdkManager &m_sdkManager;
    const AndroidConfig &m_config;
    AndroidSdkPackageList m_allPackages;
    FileName lastSdkManagerPath;
};

/*!
    \class SdkManagerOutputParser
    \brief The SdkManagerOutputParser class is a helper class to parse the output of the \c sdkmanager
    commands.
 */
class SdkManagerOutputParser
{
    struct GenericPackageData
    {
        bool isValid() const { return !revision.isNull() && !description.isNull(); }
        QStringList headerParts;
        QVersionNumber revision;
        QString description;
        Utils::FileName installedLocation;
        QMap<QString, QString> extraData;
    };

public:
    enum MarkerTag
    {
        None                        = 0x001,
        InstalledPackagesMarker     = 0x002,
        AvailablePackagesMarkers    = 0x004,
        AvailableUpdatesMarker      = 0x008,
        EmptyMarker                 = 0x010,
        PlatformMarker              = 0x020,
        SystemImageMarker           = 0x040,
        BuildToolsMarker            = 0x080,
        SdkToolsMarker              = 0x100,
        PlatformToolsMarker         = 0x200,
        SectionMarkers = InstalledPackagesMarker | AvailablePackagesMarkers | AvailableUpdatesMarker
    };

    SdkManagerOutputParser(AndroidSdkPackageList &container) : m_packages(container) {}
    void parsePackageListing(const QString &output);

    AndroidSdkPackageList &m_packages;

private:
    void compilePackageAssociations();
    void parsePackageData(MarkerTag packageMarker, const QStringList &data);
    bool parseAbstractData(GenericPackageData &output, const QStringList &input, int minParts,
                           const QString &logStrTag, QStringList extraKeys = QStringList()) const;
    AndroidSdkPackage *parsePlatform(const QStringList &data) const;
    QPair<SystemImage *, int> parseSystemImage(const QStringList &data) const;
    BuildTools *parseBuildToolsPackage(const QStringList &data) const;
    SdkTools *parseSdkToolsPackage(const QStringList &data) const;
    PlatformTools *parsePlatformToolsPackage(const QStringList &data) const;
    MarkerTag parseMarkers(const QString &line);

    MarkerTag m_currentSection = MarkerTag::None;
    QHash<AndroidSdkPackage *, int> m_systemImages;
};

const std::map<SdkManagerOutputParser::MarkerTag, const char *> markerTags {
    {SdkManagerOutputParser::MarkerTag::InstalledPackagesMarker,    "Installed packages:"},
    {SdkManagerOutputParser::MarkerTag::AvailablePackagesMarkers,   "Available Packages:"},
    {SdkManagerOutputParser::MarkerTag::AvailablePackagesMarkers,   "Available Updates:"},
    {SdkManagerOutputParser::MarkerTag::PlatformMarker,             "platforms"},
    {SdkManagerOutputParser::MarkerTag::SystemImageMarker,          "system-images"},
    {SdkManagerOutputParser::MarkerTag::BuildToolsMarker,           "build-tools"},
    {SdkManagerOutputParser::MarkerTag::SdkToolsMarker,             "tools"},
    {SdkManagerOutputParser::MarkerTag::PlatformToolsMarker,        "platform-tools"}
};

AndroidSdkManager::AndroidSdkManager(const AndroidConfig &config, QObject *parent):
    QObject(parent),
    m_d(new AndroidSdkManagerPrivate(*this, config))
{
}

AndroidSdkManager::~AndroidSdkManager()
{
    cancelOperatons();
}

AndroidSdkManager::AndroidSdkManager(const AndroidSdkManager &other)
{
    *this = other;
}

const AndroidSdkManager &AndroidSdkManager::operator=(const AndroidSdkManager &other)
{
    m_d->m_sdkManagerArgs = other.m_d->m_sdkManagerArgs;
    return *this;
}

SdkPlatformList AndroidSdkManager::installedSdkPlatforms()
{
    AndroidSdkPackageList list = m_d->filteredPackages(AndroidSdkPackage::Installed,
                                                       AndroidSdkPackage::SdkPlatformPackage);
    return Utils::transform(list, [](AndroidSdkPackage *p) {
       return static_cast<SdkPlatform *>(p);
    });
}

const AndroidSdkPackageList &AndroidSdkManager::allSdkPackages()
{
    return m_d->allPackages();
}

AndroidSdkPackageList AndroidSdkManager::availableSdkPackages()
{
    return m_d->filteredPackages(AndroidSdkPackage::Available, AndroidSdkPackage::AnyValidType);
}

AndroidSdkPackageList AndroidSdkManager::installedSdkPackages()
{
    return m_d->filteredPackages(AndroidSdkPackage::Installed, AndroidSdkPackage::AnyValidType);
}

SdkPlatform *AndroidSdkManager::latestAndroidSdkPlatform(AndroidSdkPackage::PackageState state)
{
    SdkPlatform *result = nullptr;
    AndroidSdkPackageList list = m_d->filteredPackages(state,
                                                       AndroidSdkPackage::SdkPlatformPackage);
    for (AndroidSdkPackage *p : list) {
        auto platform = static_cast<SdkPlatform *>(p);
        if (!result || result->apiLevel() < platform->apiLevel())
            result = platform;
    }
    return result;
}

SdkPlatformList AndroidSdkManager::filteredSdkPlatforms(int minApiLevel,
                                                        AndroidSdkPackage::PackageState state)
{
    AndroidSdkPackageList list = m_d->filteredPackages(state,
                                                       AndroidSdkPackage::SdkPlatformPackage);

    SdkPlatformList result;
    for (AndroidSdkPackage *p : list) {
        auto platform = static_cast<SdkPlatform *>(p);
        if (platform && platform->apiLevel() >= minApiLevel)
            result << platform;
    }
    return result;
}

void AndroidSdkManager::forceReloadPackages()
{
    m_d->refreshSdkPackages(true);
}

const QStringList &AndroidSdkManager::sdkManagerArguments() const
{
    return m_d->m_sdkManagerArgs;
}

void AndroidSdkManager::setSdkManagerArguments(const QStringList &arguments)
{
    m_d->m_sdkManagerArgs = arguments;
}

QFuture<QString> AndroidSdkManager::availableArguments() const
{
    return Utils::runAsync(&AndroidSdkManagerPrivate::parseCommonArguments, m_d.get());
}

QFuture<AndroidSdkManager::OperationOutput>
AndroidSdkManager::installPackage(const QString &sdkStylePath)
{
    auto future = Utils::runAsync(&AndroidSdkManagerPrivate::installPackage, m_d.get(),
                                  sdkStylePath);
    m_d->addWatcher(future);
    return future;
}

QFuture<AndroidSdkManager::OperationOutput>
AndroidSdkManager::uninstallPackage(const QString &sdkStylePath)
{
    auto future = Utils::runAsync(&AndroidSdkManagerPrivate::uninstallPackage, m_d.get(),
                                  sdkStylePath);
    m_d->addWatcher(future);
    return future;
}

QFuture<AndroidSdkManager::OperationOutput> AndroidSdkManager::updateAll()
{
    auto future = Utils::runAsync(&AndroidSdkManagerPrivate::updateInstalled, m_d.get());
    m_d->addWatcher(future);
    return future;
}

void AndroidSdkManager::cancelOperatons()
{
    emit cancelActiveOperations();
    foreach (auto f, m_d->m_activeOperations) {
        if (!f->isFinished())
            f->waitForFinished();
        delete f;
    }
    m_d->m_activeOperations.clear();
}

void SdkManagerOutputParser::parsePackageListing(const QString &output)
{
    QStringList packageData;
    bool collectingPackageData = false;
    MarkerTag currentPackageMarker = MarkerTag::None;

    auto processCurrentPackage = [&]() {
        if (collectingPackageData) {
            collectingPackageData = false;
            parsePackageData(currentPackageMarker, packageData);
            packageData.clear();
        }
    };

    QRegularExpression delimiters("[\n\r]");
    foreach (QString outputLine, output.split(delimiters)) {
        MarkerTag marker = parseMarkers(outputLine);

        if (marker & SectionMarkers) {
            // Section marker found. Update the current section being parsed.
            m_currentSection = marker;
            processCurrentPackage();
            continue;
        }

        if (m_currentSection == None)
            continue; // Continue with the verbose output utill a valid section starts.

        if (marker == EmptyMarker) {
            // Empty marker. Occurs at the end of a package details.
            // Process the collected package data, if any.
            processCurrentPackage();
            continue;
        }

        if (marker == None) {
            if (collectingPackageData)
                packageData << outputLine; // Collect data until next marker.
            else
                continue;
        } else {
            // Package marker found.
            processCurrentPackage(); // New package starts. Process the collected package data, if any.
            currentPackageMarker = marker;
            collectingPackageData = true;
            packageData << outputLine;
        }
    }
    compilePackageAssociations();
}

void SdkManagerOutputParser::compilePackageAssociations()
{
    // Index of the installed package having same sdk style path and same revision as of p.
    auto isInstalled = [](const AndroidSdkPackageList &container, AndroidSdkPackage *p) {
        return Utils::anyOf(container, [p](AndroidSdkPackage *other) {
            return other->state() == AndroidSdkPackage::Installed &&
                    other->sdkStylePath() == p->sdkStylePath() &&
                    other->revision() == p->revision();
        });
    };

    auto deleteAlreeadyInstalled = [isInstalled](AndroidSdkPackageList &packages) {
        for (auto p = packages.begin(); p != packages.end();) {
            if ((*p)->state() == AndroidSdkPackage::Available && isInstalled(packages, *p)) {
                delete *p;
                p = packages.erase(p);
            } else {
                ++p;
            }
        }
    };

    // Remove already installed packages.
    deleteAlreeadyInstalled(m_packages);

    // Filter out available images that are already installed.
    AndroidSdkPackageList images = m_systemImages.keys();
    deleteAlreeadyInstalled(images);

    // Associate the system images with sdk platforms.
    for (AndroidSdkPackage *image : images) {
        int imageApi = m_systemImages[image];
        auto findPlatform = [imageApi](const AndroidSdkPackage *p) {
            const SdkPlatform *platform = nullptr;
            if (p->type() == AndroidSdkPackage::SdkPlatformPackage)
                platform = static_cast<const SdkPlatform*>(p);
            return platform && platform->apiLevel() == imageApi;
        };
        auto itr = std::find_if(m_packages.begin(), m_packages.end(),
                                findPlatform);
        if (itr != m_packages.end()) {
            SdkPlatform *platform = static_cast<SdkPlatform*>(*itr);
            platform->addSystemImage(static_cast<SystemImage *>(image));
        }
    }
}

void SdkManagerOutputParser::parsePackageData(MarkerTag packageMarker, const QStringList &data)
{
    QTC_ASSERT(!data.isEmpty() && packageMarker != None, return);

    AndroidSdkPackage *package = nullptr;
    auto createPackage = [&](std::function<AndroidSdkPackage *(SdkManagerOutputParser *,
                                                               const QStringList &)> creator) {
        if ((package = creator(this, data)))
            m_packages.append(package);
    };

    switch (packageMarker) {
    case MarkerTag::BuildToolsMarker:
        createPackage(&SdkManagerOutputParser::parseBuildToolsPackage);
        break;

    case MarkerTag::SdkToolsMarker:
        createPackage(&SdkManagerOutputParser::parseSdkToolsPackage);
        break;

    case MarkerTag::PlatformToolsMarker:
        createPackage(&SdkManagerOutputParser::parsePlatformToolsPackage);
        break;

    case MarkerTag::PlatformMarker:
        createPackage(&SdkManagerOutputParser::parsePlatform);
        break;

    case MarkerTag::SystemImageMarker:
    {
        QPair<SystemImage *, int> result = parseSystemImage(data);
        if (result.first) {
            m_systemImages[result.first] = result.second;
            package = result.first;
        }
    }
        break;

    default:
        qCDebug(sdkManagerLog) << "Unhandled package: " << markerTags.at(packageMarker);
        break;
    }

    if (package) {
        switch (m_currentSection) {
        case MarkerTag::InstalledPackagesMarker:
            package->setState(AndroidSdkPackage::Installed);
            break;
        case MarkerTag::AvailablePackagesMarkers:
        case MarkerTag::AvailableUpdatesMarker:
            package->setState(AndroidSdkPackage::Available);
            break;
        default:
            qCDebug(sdkManagerLog) << "Invalid section marker: " << markerTags.at(m_currentSection);
            break;
        }
    }
}

bool SdkManagerOutputParser::parseAbstractData(SdkManagerOutputParser::GenericPackageData &output,
                                               const QStringList &input, int minParts,
                                               const QString &logStrTag,
                                               QStringList extraKeys) const
{
    if (input.isEmpty()) {
        qCDebug(sdkManagerLog) << logStrTag + ": Empty input";
        return false;
    }

    output.headerParts = input.at(0).split(';');
    if (output.headerParts.count() < minParts) {
        qCDebug(sdkManagerLog) << logStrTag + "%1: Unexpected header:" << input;
        return false;
    }

    extraKeys << installLocationKey << revisionKey << descriptionKey;
    foreach (QString line, input) {
        QString value;
        for (auto key: extraKeys) {
            if (valueForKey(key, line, &value)) {
                if (key == installLocationKey)
                    output.installedLocation = Utils::FileName::fromString(value);
                else if (key == revisionKey)
                    output.revision = QVersionNumber::fromString(value);
                else if (key == descriptionKey)
                    output.description = value;
                else
                    output.extraData[key] = value;
                break;
            }
        }
    }

    return output.isValid();
}

AndroidSdkPackage *SdkManagerOutputParser::parsePlatform(const QStringList &data) const
{
    SdkPlatform *platform = nullptr;
    GenericPackageData packageData;
    if (parseAbstractData(packageData, data, 2, "Platform")) {
        int apiLevel = platformNameToApiLevel(packageData.headerParts.at(1));
        if (apiLevel == -1) {
            qCDebug(sdkManagerLog) << "Platform: Can not parse api level:"<< data;
            return nullptr;
        }
        platform = new SdkPlatform(packageData.revision, data.at(0), apiLevel);
        platform->setDescriptionText(packageData.description);
        platform->setInstalledLocation(packageData.installedLocation);
    } else {
        qCDebug(sdkManagerLog) << "Platform: Parsing failed. Minimum required data unavailable:"
                               << data;
    }
    return platform;
}

QPair<SystemImage *, int> SdkManagerOutputParser::parseSystemImage(const QStringList &data) const
{
    QPair <SystemImage *, int> result(nullptr, -1);
    GenericPackageData packageData;
    if (parseAbstractData(packageData, data, 4, "System-image")) {
        int apiLevel = platformNameToApiLevel(packageData.headerParts.at(1));
        if (apiLevel == -1) {
            qCDebug(sdkManagerLog) << "System-image: Can not parse api level:"<< data;
            return result;
        }
        auto image = new SystemImage(packageData.revision, data.at(0),
                                     packageData.headerParts.at(3));
        image->setInstalledLocation(packageData.installedLocation);
        image->setDisplayText(packageData.description);
        image->setDescriptionText(packageData.description);
        result = qMakePair(image, apiLevel);
    } else {
        qCDebug(sdkManagerLog) << "System-image: Minimum required data unavailable: "<< data;
    }
    return result;
}

BuildTools *SdkManagerOutputParser::parseBuildToolsPackage(const QStringList &data) const
{
    BuildTools *buildTools = nullptr;
    GenericPackageData packageData;
    if (parseAbstractData(packageData, data, 2, "Build-tools")) {
        buildTools = new BuildTools(packageData.revision, data.at(0));
        buildTools->setDescriptionText(packageData.description);
        buildTools->setDisplayText(packageData.description);
        buildTools->setInstalledLocation(packageData.installedLocation);
    } else {
        qCDebug(sdkManagerLog) << "Build-tools: Parsing failed. Minimum required data unavailable:"
                               << data;
    }
    return buildTools;
}

SdkTools *SdkManagerOutputParser::parseSdkToolsPackage(const QStringList &data) const
{
    SdkTools *sdkTools = nullptr;
    GenericPackageData packageData;
    if (parseAbstractData(packageData, data, 1, "SDK-tools")) {
        sdkTools = new SdkTools(packageData.revision, data.at(0));
        sdkTools->setDescriptionText(packageData.description);
        sdkTools->setDisplayText(packageData.description);
        sdkTools->setInstalledLocation(packageData.installedLocation);
    } else {
        qCDebug(sdkManagerLog) << "SDK-tools: Parsing failed. Minimum required data unavailable:"
                               << data;
    }
    return sdkTools;
}

PlatformTools *SdkManagerOutputParser::parsePlatformToolsPackage(const QStringList &data) const
{
    PlatformTools *platformTools = nullptr;
    GenericPackageData packageData;
    if (parseAbstractData(packageData, data, 1, "Platform-tools")) {
        platformTools = new PlatformTools(packageData.revision, data.at(0));
        platformTools->setDescriptionText(packageData.description);
        platformTools->setDisplayText(packageData.description);
        platformTools->setInstalledLocation(packageData.installedLocation);
    } else {
        qCDebug(sdkManagerLog) << "Platform-tools: Parsing failed. Minimum required data "
                                  "unavailable:" << data;
    }
    return platformTools;
}

SdkManagerOutputParser::MarkerTag SdkManagerOutputParser::parseMarkers(const QString &line)
{
    if (line.isEmpty())
        return EmptyMarker;

    for (auto pair: markerTags) {
        if (line.startsWith(QLatin1String(pair.second)))
            return pair.first;
    }

    return None;
}

AndroidSdkManagerPrivate::AndroidSdkManagerPrivate(AndroidSdkManager &sdkManager,
                                                   const AndroidConfig &config):
    m_sdkManager(sdkManager),
    m_config(config)
{
    load();
}

AndroidSdkManagerPrivate::~AndroidSdkManagerPrivate()
{
    save();
    clearPackages();
}

AndroidSdkPackageList
AndroidSdkManagerPrivate::filteredPackages(AndroidSdkPackage::PackageState state,
                                           AndroidSdkPackage::PackageType type, bool forceUpdate)
{
    refreshSdkPackages(forceUpdate);
    return Utils::filtered(m_allPackages, [state, type](const AndroidSdkPackage *p) {
       return p->state() & state && p->type() & type;
    });
}

const AndroidSdkPackageList &AndroidSdkManagerPrivate::allPackages(bool forceUpdate)
{
    refreshSdkPackages(forceUpdate);
    return m_allPackages;
}

void AndroidSdkManagerPrivate::reloadSdkPackages()
{
    m_sdkManager.packageReloadBegin();
    clearPackages();

    lastSdkManagerPath = m_config.sdkManagerToolPath();

    if (m_config.sdkToolsVersion().isNull()) {
        // Configuration has invalid sdk path or corrupt installation.
        m_sdkManager.packageReloadFinished();
        return;
    }

    if (m_config.sdkToolsVersion() < sdkManagerIntroVersion) {
        // Old Sdk tools.
        AndroidToolManager toolManager(m_config);
        auto toAndroidSdkPackages = [](SdkPlatform *p) -> AndroidSdkPackage *{
            return p;
        };
        m_allPackages = Utils::transform(toolManager.availableSdkPlatforms(), toAndroidSdkPackages);
    } else {
        QString packageListing;
        QStringList args({"--list", "--verbose"});
        args << m_sdkManagerArgs;
        if (sdkManagerCommand(m_config.sdkManagerToolPath(), args, &packageListing)) {
            SdkManagerOutputParser parser(m_allPackages);
            parser.parsePackageListing(packageListing);
        }
    }
    m_sdkManager.packageReloadFinished();
}

void AndroidSdkManagerPrivate::refreshSdkPackages(bool forceReload)
{
    // Sdk path changed. Updated packages.
    // QTC updates the package listing only
    if (m_config.sdkManagerToolPath() != lastSdkManagerPath || forceReload)
        reloadSdkPackages();
}

void AndroidSdkManagerPrivate::installPackage(SdkCmdFutureInterface &fi,
                                              const QString &sdkStylePath)
{
    AndroidSdkManager::OperationOutput result;
    result.sdkStylePath = sdkStylePath;
    QStringList args(sdkStylePath);
    args << m_sdkManagerArgs;
    if (!fi.isCanceled())
        sdkManagerCommand(m_config.sdkManagerToolPath(), args, m_sdkManager, fi, result);
    else
        qCDebug(sdkManagerLog) << sdkStylePath << "Install: Operation cancelled before start";
    fi.reportResult(result);
}

void AndroidSdkManagerPrivate::uninstallPackage(SdkCmdFutureInterface &fi,
                                                const QString &sdkStylePath)
{
    AndroidSdkManager::OperationOutput result;
    result.sdkStylePath = sdkStylePath;
    QStringList args({"--uninstall", sdkStylePath});
    args << m_sdkManagerArgs;
    if (!fi.isCanceled())
        sdkManagerCommand(m_config.sdkManagerToolPath(), args, m_sdkManager, fi, result);
    else
        qCDebug(sdkManagerLog) << sdkStylePath << "Uninstall: Operation cancelled before start";
    fi.reportResult(result);
}

void AndroidSdkManagerPrivate::updateInstalled(SdkCmdFutureInterface &fi)
{
    AndroidSdkManager::OperationOutput result;
    QStringList args("--update");
    args << m_sdkManagerArgs;
    if (!fi.isCanceled())
        sdkManagerCommand(m_config.sdkManagerToolPath(), args, m_sdkManager, fi, result);
    else
        qCDebug(sdkManagerLog) << "Update: Operation cancelled before start";
    fi.reportResult(result);
}

void AndroidSdkManagerPrivate::addWatcher(const QFuture<AndroidSdkManager::OperationOutput> &future)
{
    if (future.isFinished())
        return;
    auto watcher = new QFutureWatcher<void>();
    watcher->setFuture(future);
    m_activeOperations << watcher;
    auto onFutureFinished = [watcher, this]() {
        m_activeOperations.removeAll(watcher);
        watcher->deleteLater();
    };
    QObject::connect(watcher, &QFutureWatcher<void>::finished, onFutureFinished);
}

void AndroidSdkManagerPrivate::parseCommonArguments(QFutureInterface<QString> &fi)
{
    QString argumentDetails;
    QString output;
    sdkManagerCommand(m_config.sdkManagerToolPath(), QStringList("--help"), &output);
    bool foundTag = false;
    for (const QString& line : output.split('\n')) {
        if (fi.isCanceled())
            break;
        if (foundTag)
            argumentDetails.append(line + "\n");
        else if (line.startsWith(commonArgsKey))
            foundTag = true;
    }

    if (!fi.isCanceled())
        fi.reportResult(argumentDetails);
}

void AndroidSdkManagerPrivate::clearPackages()
{
    for (AndroidSdkPackage *p : m_allPackages)
        delete p;
    m_allPackages.clear();
}

void AndroidSdkManagerPrivate::load()
{
    QSettings *settings = Core::ICore::settings();
    settings->beginGroup(Constants::ANDROID_SETTINGS_GROUP);
    settings->beginGroup(Constants::ANDROID_SETTINGS_SDKMANAGER);
    m_sdkManagerArgs = settings->value(sdkManagerArgsKey).toStringList();
    settings->endGroup();
    settings->endGroup();
}

void AndroidSdkManagerPrivate::save() const
{
    QSettings *settings = Core::ICore::settings();
    settings->beginGroup(Constants::ANDROID_SETTINGS_GROUP);
    settings->beginGroup(Constants::ANDROID_SETTINGS_SDKMANAGER);
    settings->setValue(sdkManagerArgsKey, m_sdkManagerArgs);
    settings->endGroup();
    settings->endGroup();
}

} // namespace Internal
} // namespace Android
