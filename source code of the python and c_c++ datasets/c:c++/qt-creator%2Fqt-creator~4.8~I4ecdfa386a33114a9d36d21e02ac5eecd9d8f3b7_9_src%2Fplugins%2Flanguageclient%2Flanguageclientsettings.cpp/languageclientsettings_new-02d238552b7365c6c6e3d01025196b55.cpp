/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
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

#include "languageclientsettings.h"

#include "baseclient.h"
#include "languageclientmanager.h"
#include "languageclient_global.h"

#include <coreplugin/icore.h>
#include <utils/algorithm.h>
#include <utils/delegates.h>
#include <utils/qtcprocess.h>
#include <utils/mimetypes/mimedatabase.h>
#include <languageserverprotocol/lsptypes.h>

#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTreeView>

constexpr char nameKey[] = "name";
constexpr char enabledKey[] = "enabled";
constexpr char mimeTypeKey[] = "mimeType";
constexpr char executableKey[] = "executable";
constexpr char argumentsKey[] = "arguments";
constexpr char settingsGroupKey[] = "LanguageClient";
constexpr char clientsKey[] = "clients";

namespace LanguageClient {

class LanguageClientSettingsModel : public QAbstractListModel
{
public:
    LanguageClientSettingsModel() = default;
    ~LanguageClientSettingsModel();

    // QAbstractItemModel interface
    int rowCount(const QModelIndex &/*parent*/ = QModelIndex()) const final { return m_settings.count(); }
    QVariant data(const QModelIndex &index, int role) const final;
    bool removeRows(int row, int count = 1, const QModelIndex &parent = QModelIndex()) final;
    bool insertRows(int row, int count = 1, const QModelIndex &parent = QModelIndex()) final;
    bool setData(const QModelIndex &index, const QVariant &value, int role) final;
    Qt::ItemFlags flags(const QModelIndex &index) const final;

    void reset(const QList<StdIOSettings *> &settings);
    QList<StdIOSettings *> settings() const { return m_settings; }
    QList<StdIOSettings *> removed() const { return m_removed; }
    StdIOSettings *settingForIndex(const QModelIndex &index) const;

private:
    QList<StdIOSettings *> m_settings; // owned
    QList<StdIOSettings *> m_removed;
};

class LanguageClientSettingsPageWidget : public QWidget
{
public:
    LanguageClientSettingsPageWidget(LanguageClientSettingsModel &settings);
    void activated(const QModelIndex &index);
    void applyCurrentSettings();

private:
    LanguageClientSettingsModel &m_settings;
    QTreeView *m_view = nullptr;
    QPair<StdIOSettings *, QWidget*> m_currentSettings = {nullptr, nullptr};

    void addItem();
    void deleteItem();
};

class LanguageClientSettingsPage : public Core::IOptionsPage
{
public:
    LanguageClientSettingsPage();
    ~LanguageClientSettingsPage() override;

    void init();

    // IOptionsPage interface
    QWidget *widget() override;
    void apply() override;
    void finish() override;

private:
    LanguageClientSettingsModel m_model;
    QList<StdIOSettings *> m_settings; // owned
    QPointer<LanguageClientSettingsPageWidget> m_widget;
};

class LanguageChooseDelegate : public QStyledItemDelegate
{
public:
    QWidget *createEditor(QWidget *parent,
                          const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
};

QWidget *LanguageChooseDelegate::createEditor(QWidget *parent,
                                              const QStyleOptionViewItem &option,
                                              const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    auto editor = new QComboBox(parent);
    editor->addItem(noLanguageFilter);
    editor->addItems(LanguageServerProtocol::languageIds().values());
    return editor;
}

void LanguageChooseDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    if (auto comboBox = qobject_cast<QComboBox*>(editor))
        comboBox->setCurrentText(index.data().toString());
}

LanguageClientSettingsPageWidget::LanguageClientSettingsPageWidget(LanguageClientSettingsModel &settings)
    : m_settings(settings)
    , m_view(new QTreeView())
{
    auto mainLayout = new QVBoxLayout();
    auto layout = new QHBoxLayout();
    m_view->setModel(&m_settings);
    m_view->setHeaderHidden(true);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectItems);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &LanguageClientSettingsPageWidget::activated);
    auto mimeTypes = Utils::transform(Utils::allMimeTypes(), [](const Utils::MimeType &mimeType){
        return mimeType.name();
    });
    auto buttonLayout = new QVBoxLayout();
    auto addButton = new QPushButton(tr("&Add"));
    connect(addButton, &QPushButton::pressed, this, &LanguageClientSettingsPageWidget::addItem);
    auto deleteButton = new QPushButton(tr("&Delete"));
    connect(deleteButton, &QPushButton::pressed, this, &LanguageClientSettingsPageWidget::deleteItem);

    mainLayout->addLayout(layout);
    setLayout(mainLayout);
    layout->addWidget(m_view);
    layout->addLayout(buttonLayout);
    buttonLayout->addWidget(addButton);
    buttonLayout->addWidget(deleteButton);
    buttonLayout->addStretch(10);
}

void LanguageClientSettingsPageWidget::activated(const QModelIndex &index)
{
    if (m_currentSettings.second) {
        applyCurrentSettings();
        layout()->removeWidget(m_currentSettings.second);
        delete m_currentSettings.second;
    }

    m_currentSettings.first = m_settings.settingForIndex(index);
    m_currentSettings.second = m_currentSettings.first->createSettingsWidget(this);
    layout()->addWidget(m_currentSettings.second);
}

void LanguageClientSettingsPageWidget::applyCurrentSettings()
{
    if (m_currentSettings.first)
        m_currentSettings.first->applyFromSettingsWidget(m_currentSettings.second);
}

void LanguageClientSettingsPageWidget::addItem()
{
    const int row = m_settings.rowCount();
    m_settings.insertRows(row);
}

void LanguageClientSettingsPageWidget::deleteItem()
{
    auto index = m_view->currentIndex();
    if (index.isValid())
        m_settings.removeRows(index.row());
}

LanguageClientSettingsPage::LanguageClientSettingsPage()
{
    setId("LanguageClient.General");
    setDisplayName(tr("General"));
    setCategory(Constants::LANGUAGECLIENT_SETTINGS_CATEGORY);
    setDisplayCategory(QCoreApplication::translate("LanguageClient",
                                                   Constants::LANGUAGECLIENT_SETTINGS_TR));
    setCategoryIcon(Utils::Icon({{":/languageclient/images/settingscategory_languageclient.png",
                    Utils::Theme::PanelTextColorDark}}, Utils::Icon::Tint));
}

LanguageClientSettingsPage::~LanguageClientSettingsPage()
{
    if (m_widget)
        delete m_widget;
    qDeleteAll(m_settings);
}

void LanguageClientSettingsPage::init()
{
    m_model.reset(LanguageClientSettings::fromSettings(Core::ICore::settings()));
    apply();
}

QWidget *LanguageClientSettingsPage::widget()
{
    if (!m_widget)
        m_widget = new LanguageClientSettingsPageWidget(m_model);
    return m_widget;
}

void LanguageClientSettingsPage::apply()
{
    qDeleteAll(m_settings);
    if (m_widget)
        m_widget->applyCurrentSettings();
    m_settings = Utils::transform(m_model.settings(), [](const StdIOSettings *other){
        return dynamic_cast<StdIOSettings *>(other->copy());
    });
    LanguageClientSettings::toSettings(Core::ICore::settings(), m_settings);

    QList<StdIOSettings *> restarts = Utils::filtered(m_settings, &StdIOSettings::needsRestart);
    for (auto setting : restarts + m_model.removed()) {
        if (auto client = setting->m_client) {
            if (client->reachable())
                client->shutdown();
            else
                LanguageClientManager::deleteClient(client);
        }
    }
    for (StdIOSettings *setting : m_settings) {
        if (setting && setting->isValid() && setting->m_enabled) {
            if (auto client = setting->createClient()) {
                setting->m_client = client;
                LanguageClientManager::startClient(client);
            }
        }
    }
}

void LanguageClientSettingsPage::finish()
{
    m_model.reset(m_settings);
}

LanguageClientSettingsModel::~LanguageClientSettingsModel()
{
    qDeleteAll(m_settings);
}

QVariant LanguageClientSettingsModel::data(const QModelIndex &index, int role) const
{
    StdIOSettings *setting = settingForIndex(index);
    if (!setting)
        return QVariant();
    if (role == Qt::DisplayRole)
        return setting->m_name;
    else if (role == Qt::CheckStateRole)
        return setting->m_enabled ? Qt::Checked : Qt::Unchecked;
    return QVariant();
}

bool LanguageClientSettingsModel::removeRows(int row, int count, const QModelIndex &parent)
{
    if (row >= int(m_settings.size()))
        return false;
    const int end = qMin(row + count - 1, int(m_settings.size()) - 1);
    beginRemoveRows(parent, row, end);
    for (auto i = end; i >= row; --i)
        m_removed << m_settings.takeAt(i);
    endRemoveRows();
    return true;
}

bool LanguageClientSettingsModel::insertRows(int row, int count, const QModelIndex &parent)
{
    if (row > m_settings.size() || row < 0)
        return false;
    beginInsertRows(parent, row, row + count - 1);
    for (int i = 0; i < count; ++i)
        m_settings.insert(row + i, new StdIOSettings());
    endInsertRows();
    return true;
}

bool LanguageClientSettingsModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    StdIOSettings *setting = settingForIndex(index);
    if (!setting || role != Qt::CheckStateRole)
        return false;

    if (setting->m_enabled != value.toBool()) {
        setting->m_enabled = !setting->m_enabled;
        emit dataChanged(index, index, { Qt::CheckStateRole });
    }
    return true;
}

Qt::ItemFlags LanguageClientSettingsModel::flags(const QModelIndex &/*index*/) const
{
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
}

void LanguageClientSettingsModel::reset(const QList<StdIOSettings *> &settings)
{
    beginResetModel();
    qDeleteAll(m_settings);
    qDeleteAll(m_removed);
    m_removed.clear();
    m_settings = Utils::transform(settings, [](const StdIOSettings *other){
        return dynamic_cast<StdIOSettings *>(other->copy());
    });
    endResetModel();
}

StdIOSettings *LanguageClientSettingsModel::settingForIndex(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() > m_settings.size())
        return nullptr;
    return m_settings[index.row()];
}

void BaseSettings::applyFromSettingsWidget(QWidget *widget)
{
    if (auto settingsWidget = qobject_cast<BaseSettingsWidget *>(widget)) {
        m_name = settingsWidget->name();
        m_mimeType = settingsWidget->mimeType();
    }
}

QWidget *BaseSettings::createSettingsWidget(QWidget *parent) const
{
    return new BaseSettingsWidget(this, parent);
}

bool BaseSettings::needsRestart() const
{
    return m_client ? !m_enabled || m_client->needsRestart(this) : m_enabled;
}

bool BaseSettings::isValid() const
{
    return !m_name.isEmpty();
}

BaseClient *BaseSettings::createClient() const
{
    return nullptr;
}

QVariantMap BaseSettings::toMap() const
{
    QVariantMap map;
    map.insert(nameKey, m_name);
    map.insert(enabledKey, m_enabled);
    map.insert(mimeTypeKey, m_mimeType);
    return map;
}

void BaseSettings::fromMap(const QVariantMap &map)
{
    m_name = map[nameKey].toString();
    m_enabled = map[enabledKey].toBool();
    m_mimeType = map[mimeTypeKey].toString();
}

void LanguageClientSettings::init()
{
    static LanguageClientSettingsPage settingsPage;
    settingsPage.init();
}

QList<StdIOSettings *> LanguageClientSettings::fromSettings(QSettings *settingsIn)
{
    settingsIn->beginGroup(settingsGroupKey);
    auto variants = settingsIn->value(clientsKey).toList();
    auto settings = Utils::transform(variants, [](const QVariant& var){
        auto settings = new StdIOSettings();
        settings->fromMap(var.toMap());
        return settings;
    });
    settingsIn->endGroup();
    return settings;
}

void LanguageClientSettings::toSettings(QSettings *settings, const QList<StdIOSettings *> &languageClientSettings)
{
    settings->beginGroup(settingsGroupKey);
    settings->setValue(clientsKey, Utils::transform(languageClientSettings,
                                                    [](const StdIOSettings *setting){
        return QVariant(setting->toMap());
    }));
    settings->endGroup();
}

void StdIOSettings::applyFromSettingsWidget(QWidget *widget)
{
    if (auto settingsWidget = qobject_cast<StdIOSettingsWidget *>(widget)) {
        BaseSettings::applyFromSettingsWidget(settingsWidget);
        m_executable = settingsWidget->executable();
        m_arguments = settingsWidget->arguments();
    }
}

QWidget *StdIOSettings::createSettingsWidget(QWidget *parent) const
{
    return new StdIOSettingsWidget(this, parent);
}

bool StdIOSettings::needsRestart() const
{
    if (BaseSettings::needsRestart())
        return true;
    if (auto stdIOClient = qobject_cast<StdIOClient *>(m_client))
        return stdIOClient->needsRestart(this);
    return false;
}

bool StdIOSettings::isValid() const
{
    return BaseSettings::isValid() && !m_executable.isEmpty();
}

BaseClient *StdIOSettings::createClient() const
{
    auto client = new StdIOClient(m_executable, m_arguments);
    client->setName(m_name);
    if (m_mimeType != noLanguageFilter)
        client->setSupportedMimeType({m_mimeType});
    return client;
}

QVariantMap StdIOSettings::toMap() const
{
    QVariantMap map = BaseSettings::toMap();
    map.insert(executableKey, m_executable);
    map.insert(argumentsKey, m_arguments);
    return map;
}

void StdIOSettings::fromMap(const QVariantMap &map)
{
    BaseSettings::fromMap(map);
    m_executable = map[executableKey].toString();
    m_arguments = map[argumentsKey].toString();
}

BaseSettingsWidget::BaseSettingsWidget(const BaseSettings *settings, QWidget *parent)
    : QWidget(parent)
    , m_name(new QLineEdit(settings->m_name, this))
    , m_mimeType(new QLineEdit(settings->m_mimeType, this))
{
    auto *mainLayout = new QGridLayout(this);
    mainLayout->addWidget(new QLabel(tr("Name:")), 0, 0);
    mainLayout->addWidget(m_name, 0, 1);
    mainLayout->addWidget(new QLabel(tr("Language:")), 1, 0);
    mainLayout->addWidget(m_mimeType, 1 , 1);

    auto mimeTypes = Utils::transform(Utils::allMimeTypes(), [](const Utils::MimeType &mimeType){
        return mimeType.name();
    });
    auto mimeTypeCompleter = new QCompleter(mimeTypes);
    mimeTypeCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    mimeTypeCompleter->setFilterMode(Qt::MatchContains);
    m_mimeType->setCompleter(mimeTypeCompleter);

    setLayout(mainLayout);
}

QString BaseSettingsWidget::name() const
{
    return m_name->text();
}

QString BaseSettingsWidget::mimeType() const
{
    return m_mimeType->text();
}

StdIOSettingsWidget::StdIOSettingsWidget(const StdIOSettings *settings, QWidget *parent)
    : BaseSettingsWidget(settings, parent)
    , m_executable(new Utils::PathChooser(this))
    , m_arguments(new QLineEdit(settings->m_arguments, this))
{
    auto mainLayout = qobject_cast<QGridLayout *>(layout());
    QTC_ASSERT(mainLayout, return);
    const int baseRows = mainLayout->rowCount();
    mainLayout->addWidget(new QLabel(tr("Executable:")), baseRows, 0);
    mainLayout->addWidget(m_executable, baseRows, 1);
    mainLayout->addWidget(new QLabel(tr("Arguments:")), baseRows + 1, 0);
    m_executable->setExpectedKind(Utils::PathChooser::Command);
    m_executable->setPath(QDir::toNativeSeparators(settings->m_executable));
    mainLayout->addWidget(m_arguments, baseRows + 1, 1);
}

QString StdIOSettingsWidget::executable() const
{
    return m_executable->path();
}

QString StdIOSettingsWidget::arguments() const
{
    return m_arguments->text();
}

} // namespace LanguageClient
