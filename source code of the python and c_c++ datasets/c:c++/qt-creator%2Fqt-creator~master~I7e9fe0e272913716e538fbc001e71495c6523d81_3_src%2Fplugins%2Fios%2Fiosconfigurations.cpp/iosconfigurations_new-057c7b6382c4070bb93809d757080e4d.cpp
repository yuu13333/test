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

#include "iosconfigurations.h"
#include "iosconstants.h"
#include "iosdevice.h"
#include "iossimulator.h"
#include "simulatorcontrol.h"
#include "iosprobe.h"

#include <coreplugin/icore.h>
#include <utils/algorithm.h>
#include <utils/qtcassert.h>
#include <projectexplorer/kitmanager.h>
#include <projectexplorer/kitinformation.h>
#include <projectexplorer/devicesupport/devicemanager.h>
#include <projectexplorer/toolchainmanager.h>
#include <projectexplorer/toolchain.h>
#include <projectexplorer/gcctoolchain.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <debugger/debuggeritemmanager.h>
#include <debugger/debuggeritem.h>
#include <debugger/debuggerkitinformation.h>
#include <qtsupport/baseqtversion.h>
#include <qtsupport/qtkitinformation.h>
#include <qtsupport/qtversionmanager.h>
#include <qtsupport/qtversionfactory.h>

#include <QDir>
#include <QDomDocument>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QLoggingCategory>
#include <QProcess>
#include <QSettings>
#include <QTimer>

using namespace ProjectExplorer;
using namespace QtSupport;
using namespace Utils;
using namespace Debugger;

namespace {
Q_LOGGING_CATEGORY(kitSetupLog, "qtc.ios.kitSetup")
Q_LOGGING_CATEGORY(iosSettingsLog, "qtc.ios.common")
}
namespace Ios {
namespace Internal {

const QLatin1String SettingsGroup("IosConfigurations");
const QLatin1String ignoreAllDevicesKey("IgnoreAllDevices");

static const QString XCODE_PLIST_PATH = QDir::homePath() + QStringLiteral("/Library/Preferences/com.apple.dt.Xcode.plist");
static const QString PROVISIONING_PROFILE_DIR_PATH = QDir::homePath() + QStringLiteral("/Library/MobileDevice/Provisioning Profiles");
static const QString simulatorSDKSettings = QStringLiteral("Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator.sdk/SDKSettings.plist");
static const QString deviceSDKSettings = QStringLiteral("Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk/SDKSettings.plist");

static Core::Id deviceId(const Platform &platform)
{
    if (platform.name.startsWith(QLatin1String("iphoneos-")))
        return Constants::IOS_DEVICE_TYPE;
    else if (platform.name.startsWith(QLatin1String("iphonesimulator-")))
        return Constants::IOS_SIMULATOR_TYPE;
    return Core::Id();
}

static bool handledPlatform(const Platform &platform)
{
    // do not want platforms that
    // - are not iphone (e.g. watchos)
    // - are not base
    // - are C++11
    // - are not clang
    return deviceId(platform).isValid()
            && (platform.platformKind & Platform::BasePlatform) != 0
            && (platform.platformKind & Platform::Cxx11Support) == 0
            && platform.compilerPath.toString().contains(QLatin1String("clang"));
}

static QList<Platform> handledPlatforms()
{
    QList<Platform> platforms = IosProbe::detectPlatforms().values();
    return Utils::filtered(platforms, handledPlatform);
}

static QList<ClangToolChain *> clangToolChains(const QList<ToolChain *> &toolChains)
{
    QList<ClangToolChain *> clangToolChains;
    foreach (ToolChain *toolChain, toolChains)
        if (toolChain->typeId() == ProjectExplorer::Constants::CLANG_TOOLCHAIN_TYPEID)
            clangToolChains.append(static_cast<ClangToolChain *>(toolChain));
    return clangToolChains;
}

static QList<ClangToolChain *> autoDetectedIosToolChains()
{
    const QList<ClangToolChain *> toolChains = clangToolChains(ToolChainManager::toolChains());
    return Utils::filtered(toolChains, [](ClangToolChain *toolChain) {
        return toolChain->isAutoDetected()
               && toolChain->displayName().startsWith(QLatin1String("iphone")); // TODO tool chains should be marked directly
    });
}

static ClangToolChain *findToolChainForPlatform(const Platform &platform, const QList<ClangToolChain *> &toolChains)
{
    return Utils::findOrDefault(toolChains, [&platform](ClangToolChain *toolChain) {
        return platform.compilerPath == toolChain->compilerCommand()
               && platform.backendFlags == toolChain->platformCodeGenFlags()
               && platform.backendFlags == toolChain->platformLinkerFlags();
    });
}

static QHash<Platform, ClangToolChain *> findToolChains(const QList<Platform> &platforms)
{
    QHash<Platform, ClangToolChain *> platformToolChainHash;
    const QList<ClangToolChain *> toolChains = autoDetectedIosToolChains();
    foreach (const Platform &platform, platforms) {
        ClangToolChain *toolChain = findToolChainForPlatform(platform, toolChains);
        if (toolChain)
            platformToolChainHash.insert(platform, toolChain);
    }
    return platformToolChainHash;
}

static QHash<Abi::Architecture, QSet<BaseQtVersion *>> iosQtVersions()
{
    QHash<Abi::Architecture, QSet<BaseQtVersion *>> versions;
    foreach (BaseQtVersion *qtVersion, QtVersionManager::unsortedVersions()) {
        if (!qtVersion->isValid() || qtVersion->type() != QLatin1String(Constants::IOSQT))
            continue;
        foreach (const Abi &abi, qtVersion->qtAbis())
            versions[abi.architecture()].insert(qtVersion);
    }
    return versions;
}

static void printQtVersions(const QHash<Abi::Architecture, QSet<BaseQtVersion *> > &map)
{
    foreach (const Abi::Architecture &arch, map.keys()) {
        qCDebug(kitSetupLog) << "-" << Abi::toString(arch);
        foreach (const BaseQtVersion *version, map.value(arch))
            qCDebug(kitSetupLog) << "  -" << version->displayName() << version;
    }
}

static QSet<Kit *> existingAutoDetectedIosKits()
{
    return Utils::filtered(KitManager::kits(), [](Kit *kit) -> bool {
        Core::Id deviceKind = DeviceTypeKitInformation::deviceTypeId(kit);
        return kit->isAutoDetected() && (deviceKind == Constants::IOS_DEVICE_TYPE
                                         || deviceKind == Constants::IOS_SIMULATOR_TYPE);
    }).toSet();
}

static void printKits(const QSet<Kit *> &kits)
{
    foreach (const Kit *kit, kits)
        qCDebug(kitSetupLog) << "  -" << kit->displayName();
}

static void setupKit(Kit *kit, Core::Id pDeviceType, ClangToolChain *pToolchain,
                     const QVariant &debuggerId, const Utils::FileName &sdkPath, BaseQtVersion *qtVersion)
{
    DeviceTypeKitInformation::setDeviceTypeId(kit, pDeviceType);
    ToolChainKitInformation::setToolChain(kit, pToolchain);
    QtKitInformation::setQtVersion(kit, qtVersion);
    // only replace debugger with the default one if we find an unusable one here
    // (since the user could have changed it)
    if ((!DebuggerKitInformation::debugger(kit)
            || !DebuggerKitInformation::debugger(kit)->isValid()
            || DebuggerKitInformation::debugger(kit)->engineType() != LldbEngineType)
            && debuggerId.isValid())
        DebuggerKitInformation::setDebugger(kit, debuggerId);

    kit->setMutable(DeviceKitInformation::id(), true);
    kit->setSticky(QtKitInformation::id(), true);
    kit->setSticky(ToolChainKitInformation::id(), true);
    kit->setSticky(DeviceTypeKitInformation::id(), true);
    kit->setSticky(SysRootKitInformation::id(), true);
    kit->setSticky(DebuggerKitInformation::id(), false);

    SysRootKitInformation::setSysRoot(kit, sdkPath);
}



static IosConfigurations *m_instance = 0;

IosConfigurations *IosConfigurations::instance()
{
    return m_instance;
}

void IosConfigurations::initialize()
{
    QTC_CHECK(m_instance == 0);
    m_instance = new IosConfigurations(0);
}

bool IosConfigurations::ignoreAllDevices()
{
    return m_instance->m_ignoreAllDevices;
}

void IosConfigurations::setIgnoreAllDevices(bool ignoreDevices)
{
    if (ignoreDevices != m_instance->m_ignoreAllDevices) {
        m_instance->m_ignoreAllDevices = ignoreDevices;
        m_instance->save();
        emit m_instance->updated();
    }
}

FileName IosConfigurations::developerPath()
{
    return m_instance->m_developerPath;
}

void IosConfigurations::save()
{
    QSettings *settings = Core::ICore::settings();
    settings->beginGroup(SettingsGroup);
    settings->setValue(ignoreAllDevicesKey, m_ignoreAllDevices);
    settings->endGroup();
}

IosConfigurations::IosConfigurations(QObject *parent)
    : QObject(parent)
{
    load();

    // Watch the provisioing profiles folder and xcode plist for changes and
    // update the content accordingly.
    m_provisioningDataWatcher = new QFileSystemWatcher(this);
    m_provisioningDataWatcher->addPath(XCODE_PLIST_PATH);
    m_provisioningDataWatcher->addPath(PROVISIONING_PROFILE_DIR_PATH);
    connect(m_provisioningDataWatcher, &QFileSystemWatcher::directoryChanged,
            this, &IosConfigurations::loadProvisioningData);
    connect(m_provisioningDataWatcher, &QFileSystemWatcher::fileChanged,
            this, &IosConfigurations::loadProvisioningData);
}

void IosConfigurations::load()
{
    QSettings *settings = Core::ICore::settings();
    settings->beginGroup(SettingsGroup);
    m_ignoreAllDevices = settings->value(ignoreAllDevicesKey, false).toBool();
    settings->endGroup();
}

void IosConfigurations::updateSimulators()
{
    // currently we have just one simulator
    DeviceManager *devManager = DeviceManager::instance();
    Core::Id devId = Constants::IOS_SIMULATOR_DEVICE_ID;
    IDevice::ConstPtr dev = devManager->find(devId);
    if (dev.isNull()) {
        dev = IDevice::ConstPtr(new IosSimulator(devId));
        devManager->addDevice(dev);
    }
    SimulatorControl::updateAvailableSimulators();
}

void IosConfigurations::setDeveloperPath(const FileName &devPath)
{
    static bool hasDevPath = false;
    if (devPath != m_instance->m_developerPath) {
        m_instance->m_developerPath = devPath;
        m_instance->save();
        if (!hasDevPath && !devPath.isEmpty()) {
            hasDevPath = true;
            QTimer::singleShot(1000, IosDeviceManager::instance(),
                               &IosDeviceManager::monitorAvailableDevices);
            m_instance->updateSimulators();
            m_instance->loadTargetSdkVersions();
            m_instance->loadProvisioningData();
        }
        emit m_instance->updated();
    }
}

void IosConfigurations::loadTargetSdkVersions()
{
    auto loadSdkVersions = [] (const Core::Id &deviceType) -> QStringList {
        QStringList targetList;
        Utils::FileName pListPath = developerPath()
                .appendPath(deviceType == Constants::IOS_DEVICE_TYPE ? deviceSDKSettings : simulatorSDKSettings);
        if (pListPath.exists()) {
            const QSettings sdkSettings(pListPath.toString(), QSettings::NativeFormat);
            QVariantMap defaultProperties = sdkSettings.value(QStringLiteral("DefaultProperties")).toMap();
            const QVariantList targetValueList = defaultProperties.values(QStringLiteral("DEPLOYMENT_TARGET_SUGGESTED_VALUES"));
            foreach (const QVariant target, targetValueList) {
                targetList.append(target.toString());
            }
        } else {
            qCDebug(iosSettingsLog) << "Cannot find the Xcode plist to get ios SDK info." << pListPath.toString();
        }
        return targetList;
    };

    m_deviceSDKTargets = loadSdkVersions(Constants::IOS_DEVICE_TYPE);
    m_simulatorSDKTargets = loadSdkVersions(Constants::IOS_SIMULATOR_TYPE);
}

void IosConfigurations::loadProvisioningData()
{
    m_provisioningData.clear();
    const QDir provisioningProflesDir(PROVISIONING_PROFILE_DIR_PATH);
    QStringList fileFilter;
    fileFilter << QStringLiteral("*.mobileprovision");
    foreach (QFileInfo fileInfo, provisioningProflesDir.entryInfoList(fileFilter, QDir::NoDotAndDotDot | QDir::Files)) {
        QDomDocument provisioningDoc;
        QString uuid, teamID;
        if (provisioningDoc.setContent(decodeProvisioningProfile(fileInfo.absoluteFilePath()))) {
            QDomNodeList nodes =  provisioningDoc.elementsByTagName("key");
            for (int i = 0;i<nodes.count(); ++i) {
                QDomElement e = nodes.at(i).toElement();

                if (e.text().compare("UUID") == 0)
                    uuid = e.nextSiblingElement().text();

                if (e.text().compare("TeamIdentifier") == 0)
                    teamID = e.nextSibling().firstChildElement().text();
            }
        } else {
            qCDebug(iosSettingsLog) << "Error reading provisoing profile" << fileInfo.absoluteFilePath();
        }
        m_provisioningData.insertMulti(teamID, uuid);
    }

    // Populate Team id's
    const QSettings xcodeSettings(XCODE_PLIST_PATH, QSettings::NativeFormat);
    const QVariantMap teamMap = xcodeSettings.value("IDEProvisioningTeams").toMap();
    const QString freeTeamTag = QLatin1String("isFreeProvisioningTeam");
    QList<QVariantMap> teams;
    QMapIterator<QString, QVariant> accountiterator(teamMap);
    while (accountiterator.hasNext()) {
        accountiterator.next();
        QVariantMap teamInfo = accountiterator.value().toMap();
        int provisioningTeamIsFree = teamInfo.value(freeTeamTag).toBool() ? 1 : 0;
        teamInfo[freeTeamTag] = provisioningTeamIsFree;
        teamInfo[QStringLiteral("eMail")] = accountiterator.key();
        teams.append(teamInfo);
    }

    // Sort team id's to move the free provisioning teams at last of the list.
    std::sort(teams.begin(), teams.end(), [freeTeamTag](const QVariantMap &teamInfo1, const QVariantMap &teamInfo2) -> bool {
        return teamInfo1.value(freeTeamTag).toInt() < teamInfo2.value(freeTeamTag).toInt();
    });

    m_developerTeams.clear();
    foreach (auto teamInfo, teams) {
        ProvisioningTeam team;
        team.name = teamInfo.value(QStringLiteral("teamName")).toString();
        team.email = teamInfo.value(QStringLiteral("eMail")).toString();
        team.uuid = teamInfo.value(QStringLiteral("teamID")).toString();
        m_developerTeams.append(team);
    }

    emit provisioningDataChanged();
}

QByteArray IosConfigurations::decodeProvisioningProfile(const QString &path)
{
    // path is assumed to be valid file path to .mobileprovision
    QProcess p;
    QStringList args;
    args << QStringLiteral("smime");
    args << QStringLiteral("-inform");
    args << QStringLiteral("der");
    args << QStringLiteral("-verify");
    args << QStringLiteral("-in");
    args << path;
    p.start(QStringLiteral("openssl"), args);
    if (!p.waitForFinished(3000))
        qCDebug(iosSettingsLog) << "Reading signed provisioning file failed" << path;
    return p.readAll();
}

void IosConfigurations::updateAutomaticKitList()
{
    const QList<Platform> platforms = handledPlatforms();
    qCDebug(kitSetupLog) << "Used platforms:" << platforms;
    if (!platforms.isEmpty())
        setDeveloperPath(platforms.first().developerPath);
    qCDebug(kitSetupLog) << "Developer path:" << developerPath();

    // platform name -> tool chain
    const QHash<Platform, ClangToolChain *> platformToolChainHash = findToolChains(platforms);

    const QHash<Abi::Architecture, QSet<BaseQtVersion *> > qtVersionsForArch = iosQtVersions();
    qCDebug(kitSetupLog) << "iOS Qt versions:";
    printQtVersions(qtVersionsForArch);

    const DebuggerItem *possibleDebugger = DebuggerItemManager::findByEngineType(LldbEngineType);
    const QVariant debuggerId = (possibleDebugger ? possibleDebugger->id() : QVariant());

    QSet<Kit *> existingKits = existingAutoDetectedIosKits();
    qCDebug(kitSetupLog) << "Existing auto-detected iOS kits:";
    printKits(existingKits);
    QSet<Kit *> resultingKits;
    // match existing kits and create missing kits
    foreach (const Platform &platform, platforms) {
        qCDebug(kitSetupLog) << "Guaranteeing kits for " << platform.name ;
        ClangToolChain *pToolchain = platformToolChainHash.value(platform);
        if (!pToolchain) {
            qCDebug(kitSetupLog) << "  - No tool chain found";
            continue;
        }
        Core::Id pDeviceType = deviceId(platform);
        QTC_ASSERT(pDeviceType.isValid(), continue);
        Abi::Architecture arch = pToolchain->targetAbi().architecture();

        QSet<BaseQtVersion *> qtVersions = qtVersionsForArch.value(arch);
        foreach (BaseQtVersion *qtVersion, qtVersions) {
            qCDebug(kitSetupLog) << "  - Qt version:" << qtVersion->displayName();
            Kit *kit = Utils::findOrDefault(existingKits, [&pDeviceType, &pToolchain, &qtVersion](const Kit *kit) {
                // we do not compare the sdk (thus automatically upgrading it in place if a
                // new Xcode is used). Change?
                return DeviceTypeKitInformation::deviceTypeId(kit) == pDeviceType
                        && ToolChainKitInformation::toolChain(kit, ToolChain::Language::Cxx) == pToolchain
                        && QtKitInformation::qtVersion(kit) == qtVersion;
            });
            QTC_ASSERT(!resultingKits.contains(kit), continue);
            if (kit) {
                qCDebug(kitSetupLog) << "    - Kit matches:" << kit->displayName();
                kit->blockNotification();
                setupKit(kit, pDeviceType, pToolchain, debuggerId, platform.sdkPath, qtVersion);
                kit->unblockNotification();
            } else {
                qCDebug(kitSetupLog) << "    - Setting up new kit";
                kit = new Kit;
                kit->blockNotification();
                kit->setAutoDetected(true);
                const QString baseDisplayName = tr("%1 %2").arg(platform.name, qtVersion->unexpandedDisplayName());
                kit->setUnexpandedDisplayName(baseDisplayName);
                setupKit(kit, pDeviceType, pToolchain, debuggerId, platform.sdkPath, qtVersion);
                kit->unblockNotification();
                KitManager::registerKit(kit);
            }
            resultingKits.insert(kit);
        }
    }
    // remove unused kits
    existingKits.subtract(resultingKits);
    qCDebug(kitSetupLog) << "Removing unused kits:";
    printKits(existingKits);
    foreach (Kit *kit, existingKits)
        KitManager::deregisterKit(kit);
}

const QStringList &IosConfigurations::targetSdkVersions(const Core::Id &deviceType)
{
    static QStringList dummy;
    Q_ASSERT(m_instance);
    if (deviceType == Constants::IOS_DEVICE_TYPE)
        return m_instance->m_deviceSDKTargets;
    else if (deviceType == Constants::IOS_SIMULATOR_TYPE)
        return m_instance->m_simulatorSDKTargets;
    else
        return dummy;
}

bool IosConfigurations::hasProvisioningProfile(const QString &teamID)
{
    Q_ASSERT(m_instance);
    return m_instance->m_provisioningData.contains(teamID);
}

const QList<ProvisioningTeam> &IosConfigurations::developerTeams()
{
    Q_ASSERT(m_instance);
    return m_instance->m_developerTeams;
}

static ClangToolChain *createToolChain(const Platform &platform)
{
    ClangToolChain *toolChain = new ClangToolChain(ToolChain::AutoDetection);
    toolChain->setLanguage(ToolChain::Language::Cxx);
    toolChain->setDisplayName(platform.name);
    toolChain->setPlatformCodeGenFlags(platform.backendFlags);
    toolChain->setPlatformLinkerFlags(platform.backendFlags);
    toolChain->resetToolChain(platform.compilerPath);
    return toolChain;
}

QSet<ToolChain::Language> IosToolChainFactory::supportedLanguages() const
{
    return { ProjectExplorer::ToolChain::Language::Cxx };
}

QList<ToolChain *> IosToolChainFactory::autoDetect(const QList<ToolChain *> &existingToolChains)
{
    QList<ClangToolChain *> existingClangToolChains = clangToolChains(existingToolChains);
    const QList<Platform> platforms = handledPlatforms();
    QList<ClangToolChain *> toolChains;
    toolChains.reserve(platforms.size());
    foreach (const Platform &platform, platforms) {
        ClangToolChain *toolChain = findToolChainForPlatform(platform, existingClangToolChains);
        if (!toolChain) {
            toolChain = createToolChain(platform);
            existingClangToolChains.append(toolChain);
        }
        toolChains.append(toolChain);
    }
    return Utils::transform(toolChains, [](ClangToolChain *tc) -> ToolChain * { return tc; });
}

} // namespace Internal
} // namespace Ios
