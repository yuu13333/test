/****************************************************************************
**
** Copyright (C) 2016 BogDan Vatra <bog_dan_ro@yahoo.com>
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

#include "androidsettingswidget.h"

#include "ui_androidsettingswidget.h"

#include "androidconfigurations.h"
#include "androidconstants.h"
#include "androidtoolchain.h"
#include "androidavdmanager.h"

#include <utils/environment.h>
#include <utils/hostosinfo.h>
#include <utils/pathchooser.h>
#include <utils/runextensions.h>
#include <utils/utilsicons.h>
#include <projectexplorer/toolchainmanager.h>
#include <projectexplorer/kitmanager.h>
#include <projectexplorer/kitinformation.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <qtsupport/qtkitinformation.h>
#include <qtsupport/qtversionmanager.h>

#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QTimer>
#include <QTime>

#include <QDesktopServices>
#include <QFileDialog>
#include <QMessageBox>
#include <QModelIndex>
#include <QtCore/QUrl>

namespace Android {
namespace Internal {

namespace {
enum JavaValidation {
    JavaPathExistsRow,
    JavaJdkValidRow
};

enum AndroidValidation {
    SdkPathExistsRow,
    SdkToolsInstalledRow,
    PlatformToolsInstalledRow,
    BuildToolsInstalledRow,
    PlatformSdkInstalledRow,
    NdkPathExistsRow,
    NdkDirStructureRow,
    NdkinstallDirOkRow
};
}

class SummaryWidget : public QWidget
{
    struct RowData {
        QLabel *m_iconLabel = nullptr;
        QLabel *m_textLabel = nullptr;
        bool m_valid = false;
    };

public:
    SummaryWidget(const QMap<int, QString> &validationPoints, const QString &validText,
                  const QString &invalidText, Utils::DetailsWidget *detailWidgets = nullptr) :
        QWidget(detailWidgets),
        m_validText(validText),
        m_invalidText(invalidText),
        m_detailsWidget(detailWidgets)
    {
        auto layout = new QGridLayout(this);
        layout->setMargin(12);
        int row = 0;
        for (auto itr = validationPoints.cbegin(); itr != validationPoints.cend(); ++itr) {
            RowData data;
            data.m_iconLabel = new QLabel(this);
            layout->addWidget(data.m_iconLabel, row, 0, 1, 1);
            data.m_textLabel = new QLabel(itr.value(), this);
            data.m_textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            layout->addWidget(data.m_textLabel, row, 1, 1, 1);
            m_rowList[itr.key()] = data;
            setPointValid(itr.key(), true);
            ++row;
        }
    }

    void setPointValid(int key, bool valid)
    {
        if (!m_rowList.contains(key))
            return;
        RowData& data = m_rowList[key];
        data.m_valid = valid;
        data.m_iconLabel->setPixmap(data.m_valid ? Utils::Icons::OK.pixmap() :
                                                   Utils::Icons::BROKEN.pixmap());
        bool ok = allRowsOK();
        m_detailsWidget->setIcon(ok ? Utils::Icons::OK.icon() :
                                      Utils::Icons::CRITICAL.icon());
        m_detailsWidget->setSummaryText(ok ? m_validText : m_invalidText);
    }

    bool allRowsOK() const
    {
        for (auto itr = m_rowList.cbegin(); itr != m_rowList.cend(); ++itr) {
            if (!itr.value().m_valid)
                return false;
        }
        return true;
    }

private:
    QString m_validText;
    QString m_invalidText;
    Utils::DetailsWidget *m_detailsWidget = nullptr;
    QMap<int, RowData> m_rowList;
};

void AvdModel::setAvdList(const AndroidDeviceInfoList &list)
{
    beginResetModel();
    m_list = list;
    endResetModel();
}

QModelIndex AvdModel::indexForAvdName(const QString &avdName) const
{
    for (int i = 0; i < m_list.size(); ++i) {
        if (m_list.at(i).serialNumber == avdName)
            return index(i, 0);
    }
    return QModelIndex();
}

QString AvdModel::avdName(const QModelIndex &index) const
{
    return m_list.at(index.row()).avdname;
}

QVariant AvdModel::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole || !index.isValid())
        return QVariant();
    switch (index.column()) {
        case 0:
            return m_list[index.row()].avdname;
        case 1:
            return QString::fromLatin1("API %1").arg(m_list[index.row()].sdk);
        case 2: {
            QStringList cpuAbis = m_list[index.row()].cpuAbi;
            return cpuAbis.isEmpty() ? QVariant() : QVariant(cpuAbis.first());
        }
    }
    return QVariant();
}

QVariant AvdModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
            case 0:
                //: AVD - Android Virtual Device
                return tr("AVD Name");
            case 1:
                return tr("AVD Target");
            case 2:
                return tr("CPU/ABI");
        }
    }
    return QAbstractItemModel::headerData(section, orientation, role );
}

int AvdModel::rowCount(const QModelIndex &/*parent*/) const
{
    return m_list.size();
}

int AvdModel::columnCount(const QModelIndex &/*parent*/) const
{
    return 3;
}

AndroidSettingsWidget::AndroidSettingsWidget(QWidget *parent)
    : QWidget(parent),
      m_ui(new Ui_AndroidSettingsWidget),
      m_androidConfig(AndroidConfigurations::currentConfig()),
      m_avdManager(new AndroidAvdManager(m_androidConfig))
{
    m_ui->setupUi(this);

    QMap<int, QString> javaValidationPoints;
    javaValidationPoints[JavaPathExistsRow] = tr("JDK path exists.");
    javaValidationPoints[JavaJdkValidRow] = tr("JDK path is a valid JDK root folder.");
    auto javaSummary = new SummaryWidget(javaValidationPoints, tr("Java Settings are OK"),
                                         tr("Java settings have errors"), m_ui->javaDetailsWidget);
    m_ui->javaDetailsWidget->setWidget(javaSummary);

    QMap<int, QString> androidValidationPoints;
    androidValidationPoints[SdkPathExistsRow] = tr("Android SDK path exists.");
    androidValidationPoints[SdkToolsInstalledRow] = tr("SDK tools installed.");
    androidValidationPoints[PlatformToolsInstalledRow] = tr("Platform tools installed.");
    androidValidationPoints[BuildToolsInstalledRow] = tr("Build tools installed.");
    androidValidationPoints[PlatformSdkInstalledRow] = tr("Platform SDK installed.");
    androidValidationPoints[NdkPathExistsRow] = tr("Android NDK path exists.");
    androidValidationPoints[NdkDirStructureRow] = tr("Android NDK directory structure is correct.");
    androidValidationPoints[NdkinstallDirOkRow] = tr("Android NDK installed into a path without "
                                                     "spaces.");
    auto androidSummary = new SummaryWidget(androidValidationPoints, tr("Android setttings are OK"),
                                            tr("Android settings have errors"),
                                            m_ui->androidDetailsWidget);
    m_ui->androidDetailsWidget->setWidget(androidSummary);

    auto kitsDetailsLabel = new QLabel(m_ui->kitWarningDetails);
    kitsDetailsLabel->setWordWrap(true);
    m_ui->kitWarningDetails->setWidget(kitsDetailsLabel);
    m_ui->kitWarningDetails->setIcon(Utils::Icons::WARNING.icon());

    m_ui->SDKLocationPathChooser->setFileName(m_androidConfig.sdkLocation());
    m_ui->SDKLocationPathChooser->setPromptDialogTitle(tr("Select Android SDK folder"));
    m_ui->NDKLocationPathChooser->setFileName(m_androidConfig.ndkLocation());
    m_ui->NDKLocationPathChooser->setPromptDialogTitle(tr("Select Android NDK folder"));

    m_ui->OpenJDKLocationPathChooser->setFileName(m_androidConfig.openJDKLocation());
    m_ui->OpenJDKLocationPathChooser->setPromptDialogTitle(tr("Select JDK Path"));
    m_ui->DataPartitionSizeSpinBox->setValue(m_androidConfig.partitionSize());
    m_ui->CreateKitCheckBox->setChecked(m_androidConfig.automaticKitCreation());
    m_ui->AVDTableView->setModel(&m_AVDModel);
    m_ui->AVDTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_ui->AVDTableView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    m_ui->downloadOpenJDKToolButton->setVisible(!Utils::HostOsInfo::isLinuxHost());

    connect(&m_virtualDevicesWatcher, &QFutureWatcherBase::finished,
            this, &AndroidSettingsWidget::updateAvds);
    connect(&m_futureWatcher, &QFutureWatcherBase::finished,
            this, &AndroidSettingsWidget::avdAdded);
    connect(m_ui->NDKLocationPathChooser, &Utils::PathChooser::rawPathChanged,
            this, &AndroidSettingsWidget::validateNdk);
    connect(m_ui->SDKLocationPathChooser, &Utils::PathChooser::rawPathChanged,
            this, &AndroidSettingsWidget::validateSdk);
    connect(m_ui->OpenJDKLocationPathChooser, &Utils::PathChooser::rawPathChanged,
            this, &AndroidSettingsWidget::validateJdk);
    connect(m_ui->AVDAddPushButton, &QAbstractButton::clicked,
            this, &AndroidSettingsWidget::addAVD);
    connect(m_ui->AVDRemovePushButton, &QAbstractButton::clicked,
            this, &AndroidSettingsWidget::removeAVD);
    connect(m_ui->AVDStartPushButton, &QAbstractButton::clicked,
            this, &AndroidSettingsWidget::startAVD);
    connect(m_ui->AVDTableView, &QAbstractItemView::activated,
            this, &AndroidSettingsWidget::avdActivated);
    connect(m_ui->AVDTableView, &QAbstractItemView::clicked,
            this, &AndroidSettingsWidget::avdActivated);
    connect(m_ui->DataPartitionSizeSpinBox, &QAbstractSpinBox::editingFinished,
            this, &AndroidSettingsWidget::dataPartitionSizeEditingFinished);
    connect(m_ui->manageAVDPushButton, &QAbstractButton::clicked,
            this, &AndroidSettingsWidget::manageAVD);
    connect(m_ui->CreateKitCheckBox, &QAbstractButton::toggled,
            this, &AndroidSettingsWidget::createKitToggled);
    connect(m_ui->downloadSDKToolButton, &QAbstractButton::clicked,
            this, &AndroidSettingsWidget::openSDKDownloadUrl);
    connect(m_ui->downloadNDKToolButton, &QAbstractButton::clicked,
            this, &AndroidSettingsWidget::openNDKDownloadUrl);
    connect(m_ui->downloadOpenJDKToolButton, &QAbstractButton::clicked,
            this, &AndroidSettingsWidget::openOpenJDKDownloadUrl);

    validateJdk();
    validateNdk();
    validateSdk();
}

AndroidSettingsWidget::~AndroidSettingsWidget()
{
    delete m_ui;
    m_futureWatcher.waitForFinished();
}

void AndroidSettingsWidget::disableAvdControls()
{
    m_ui->AVDAddPushButton->setEnabled(false);
    m_ui->AVDTableView->setEnabled(false);
    m_ui->AVDRemovePushButton->setEnabled(false);
    m_ui->AVDStartPushButton->setEnabled(false);
}

void AndroidSettingsWidget::enableAvdControls()
{
    m_ui->AVDTableView->setEnabled(true);
    m_ui->AVDAddPushButton->setEnabled(true);
    avdActivated(m_ui->AVDTableView->currentIndex());
}

void AndroidSettingsWidget::startUpdateAvd()
{
    disableAvdControls();
    m_virtualDevicesWatcher.setFuture(m_avdManager->avdList());
}

void AndroidSettingsWidget::updateAvds()
{
    m_AVDModel.setAvdList(m_virtualDevicesWatcher.result());
    if (!m_lastAddedAvd.isEmpty()) {
        m_ui->AVDTableView->setCurrentIndex(m_AVDModel.indexForAvdName(m_lastAddedAvd));
        m_lastAddedAvd.clear();
    }
    enableAvdControls();
}

void AndroidSettingsWidget::saveSettings()
{
    AndroidConfigurations::setConfig(m_androidConfig);
}

void AndroidSettingsWidget::validateJdk()
{
    auto javaPath = Utils::FileName::fromUserInput(m_ui->OpenJDKLocationPathChooser->rawPath());
    m_androidConfig.setOpenJDKLocation(javaPath);
    bool jdkPathExists = m_androidConfig.openJDKLocation().exists();
    auto summaryWidget = static_cast<SummaryWidget *>(m_ui->javaDetailsWidget->widget());
    summaryWidget->setPointValid(JavaPathExistsRow, jdkPathExists);

    Utils::FileName bin = m_androidConfig.openJDKLocation();
    bin.appendPath(QLatin1String("bin/javac" QTC_HOST_EXE_SUFFIX));
    summaryWidget->setPointValid(JavaJdkValidRow, jdkPathExists && bin.exists());
    updateUI();
}

void AndroidSettingsWidget::validateNdk()
{
    auto ndkPath = Utils::FileName::fromUserInput(m_ui->NDKLocationPathChooser->rawPath());
    m_androidConfig.setNdkLocation(ndkPath);

    auto summaryWidget = static_cast<SummaryWidget *>(m_ui->androidDetailsWidget->widget());
    summaryWidget->setPointValid(NdkPathExistsRow, m_androidConfig.ndkLocation().exists());

    Utils::FileName ndkPlatformsDir(ndkPath);
    ndkPlatformsDir.appendPath("platforms");
    Utils::FileName ndkToolChainsDir(ndkPath);
    ndkToolChainsDir.appendPath("toolchains");
    Utils::FileName ndkSourcesDir(ndkPath);
    ndkSourcesDir.appendPath("sources/cxx-stl");
    summaryWidget->setPointValid(NdkDirStructureRow,
                                 ndkPlatformsDir.exists()
                                 && ndkToolChainsDir.exists()
                                 && ndkSourcesDir.exists());
    summaryWidget->setPointValid(NdkinstallDirOkRow,
                                 ndkPlatformsDir.exists() &&
                                 !ndkPlatformsDir.toString().contains(' '));
    updateUI();
}

void AndroidSettingsWidget::validateSdk()
{
    auto sdkPath = Utils::FileName::fromUserInput(m_ui->SDKLocationPathChooser->rawPath());
    m_androidConfig.setSdkLocation(sdkPath);

    auto summaryWidget = static_cast<SummaryWidget *>(m_ui->androidDetailsWidget->widget());
    summaryWidget->setPointValid(SdkPathExistsRow, m_androidConfig.sdkLocation().exists());
    summaryWidget->setPointValid(SdkToolsInstalledRow,
                                 !m_androidConfig.sdkToolsVersion().isNull());
    summaryWidget->setPointValid(PlatformToolsInstalledRow,
                                 m_androidConfig.adbToolPath().exists());
    summaryWidget->setPointValid(BuildToolsInstalledRow,
                                 !m_androidConfig.buildToolsVersion().isNull());
    summaryWidget->setPointValid(PlatformSdkInstalledRow,
                                 !m_androidConfig.sdkTargets().isEmpty());
    updateUI();
}

void AndroidSettingsWidget::openSDKDownloadUrl()
{
    QDesktopServices::openUrl(QUrl::fromUserInput("https://developer.android.com/studio/"));
}

void AndroidSettingsWidget::openNDKDownloadUrl()
{
    QDesktopServices::openUrl(QUrl::fromUserInput("https://developer.android.com/ndk/downloads/"));
}

void AndroidSettingsWidget::openOpenJDKDownloadUrl()
{
    QDesktopServices::openUrl(QUrl::fromUserInput("http://www.oracle.com/technetwork/java/javase/downloads/"));
}

void AndroidSettingsWidget::addAVD()
{
    disableAvdControls();
    AndroidConfig::CreateAvdInfo info = m_androidConfig.gatherCreateAVDInfo(this);

    if (!info.target.isValid()) {
        enableAvdControls();
        return;
    }

    m_futureWatcher.setFuture(m_avdManager->createAvd(info));
}

void AndroidSettingsWidget::avdAdded()
{
    AndroidConfig::CreateAvdInfo info = m_futureWatcher.result();
    if (!info.error.isEmpty()) {
        enableAvdControls();
        QMessageBox::critical(this, QApplication::translate("AndroidConfig", "Error Creating AVD"), info.error);
        return;
    }

    startUpdateAvd();
    m_lastAddedAvd = info.name;
}

void AndroidSettingsWidget::removeAVD()
{
    disableAvdControls();
    QString avdName = m_AVDModel.avdName(m_ui->AVDTableView->currentIndex());
    if (QMessageBox::question(this, tr("Remove Android Virtual Device"),
                              tr("Remove device \"%1\"? This cannot be undone.").arg(avdName),
                              QMessageBox::Yes | QMessageBox::No)
            == QMessageBox::No) {
        enableAvdControls();
        return;
    }

    m_avdManager->removeAvd(avdName);
    startUpdateAvd();
}

void AndroidSettingsWidget::startAVD()
{
    m_avdManager->startAvdAsync(m_AVDModel.avdName(m_ui->AVDTableView->currentIndex()));
}

void AndroidSettingsWidget::avdActivated(const QModelIndex &index)
{
    m_ui->AVDRemovePushButton->setEnabled(index.isValid());
    m_ui->AVDStartPushButton->setEnabled(index.isValid());
}

void AndroidSettingsWidget::dataPartitionSizeEditingFinished()
{
    m_androidConfig.setPartitionSize(m_ui->DataPartitionSizeSpinBox->value());
}

void AndroidSettingsWidget::createKitToggled()
{
    m_androidConfig.setAutomaticKitCreation(m_ui->CreateKitCheckBox->isChecked());
}

void AndroidSettingsWidget::checkMissingQtVersion()
{
    auto summaryWidget = static_cast<SummaryWidget *>(m_ui->androidDetailsWidget->widget());
    if (!summaryWidget->allRowsOK()) {
        m_ui->kitWarningDetails->setVisible(false);
        m_ui->kitWarningDetails->setState(Utils::DetailsWidget::Collapsed);
        return;
    }

    QList<AndroidToolChainFactory::AndroidToolChainInformation> compilerPaths
            = AndroidToolChainFactory::toolchainPathsForNdk(m_androidConfig.ndkLocation());

    // See if we have qt versions for those toolchains
    QSet<ProjectExplorer::Abi> toolchainsForAbi;
    foreach (const AndroidToolChainFactory::AndroidToolChainInformation &ati, compilerPaths) {
        if (ati.language == Core::Id(ProjectExplorer::Constants::CXX_LANGUAGE_ID))
            toolchainsForAbi.insert(ati.abi);
    }

    const QList<QtSupport::BaseQtVersion *> androidQts
            = QtSupport::QtVersionManager::versions([](const QtSupport::BaseQtVersion *v) {
        return v->type() == QLatin1String(Constants::ANDROIDQT) && !v->qtAbis().isEmpty();
    });
    QSet<ProjectExplorer::Abi> qtVersionsForAbi;
    foreach (QtSupport::BaseQtVersion *qtVersion, androidQts)
        qtVersionsForAbi.insert(qtVersion->qtAbis().first());

    QSet<ProjectExplorer::Abi> missingQtArchs = toolchainsForAbi.subtract(qtVersionsForAbi);
    bool isArchMissing =  !missingQtArchs.isEmpty();
    m_ui->kitWarningDetails->setVisible(isArchMissing);
    if (isArchMissing) {
        m_ui->kitWarningDetails->setSummaryText(tr("Can not create kits for all architectures"));
        QLabel *detailsLabel = static_cast<QLabel *>(m_ui->kitWarningDetails->widget());
        QStringList archNames;
        for (auto abi : missingQtArchs)
            archNames << abi.toString();
        detailsLabel->setText(tr("Qt versions are missing for the following architectures:\n%1"
                                 "\n\nTo add the Qt version, select Options > Build & Run > Qt"
                                 " Versions.").arg(archNames.join(", ")));
    }
}

void AndroidSettingsWidget::updateUI()
{
    auto javaSummaryWidget = static_cast<SummaryWidget *>(m_ui->javaDetailsWidget->widget());
    auto androidSummaryWidget = static_cast<SummaryWidget *>(m_ui->androidDetailsWidget->widget());
    m_ui->AVDManagerFrame->setEnabled(javaSummaryWidget->allRowsOK()
                                      && androidSummaryWidget->allRowsOK());
    m_ui->javaDetailsWidget->setState(javaSummaryWidget->allRowsOK() ?
                                          Utils::DetailsWidget::Collapsed :
                                          Utils::DetailsWidget::Expanded);
    m_ui->androidDetailsWidget->setState(androidSummaryWidget->allRowsOK() ?
                                             Utils::DetailsWidget::Collapsed :
                                             Utils::DetailsWidget::Expanded);
    startUpdateAvd();
    checkMissingQtVersion();
}

void AndroidSettingsWidget::manageAVD()
{
    if (m_avdManager->avdManagerUiToolAvailable()) {
        m_avdManager->launchAvdManagerUiTool();
    } else {
        QMessageBox::warning(this, tr("AVD Manager Not Available"),
                             tr("AVD manager UI tool is not available in the installed SDK tools"
                                "(version %1). Use the command line tool \"avdmanager\" for "
                                "advanced AVD management.")
                             .arg(m_androidConfig.sdkToolsVersion().toString()));
    }
}


} // namespace Internal
} // namespace Android
