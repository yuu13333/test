#include "pythonextensionsplugin.h"
#include "pythonextensionsconstants.h"

#include "pyutil.h"

#include <coreplugin/icore.h>
#include <coreplugin/icontext.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/command.h>
#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/coreconstants.h>

#include <extensionsystem/pluginmanager.h>

#include <QDir>
#include <QIODevice>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QString>
#include <QStringList>


namespace PythonExtensions {
namespace Internal {

PythonExtensionsPlugin::PythonExtensionsPlugin()
{
    // Create your members
}

PythonExtensionsPlugin::~PythonExtensionsPlugin()
{
    // Unregister objects from the plugin manager's object pool
    // Delete members
}

bool PythonExtensionsPlugin::initialize(const QStringList &arguments, QString *errorString)
{
    // Register objects in the plugin manager's object pool
    // Load settings
    // Add actions to menus
    // Connect to other plugins' signals
    // In the initialize function, a plugin can be sure that the plugins it
    // depends on have initialized their members.

    Q_UNUSED(arguments)
    Q_UNUSED(errorString)

    this->initializePythonBindings();

    // Python extensions are loaded after C++ plugins for now (plan: later flag can be set)

    return true;
}

void PythonExtensionsPlugin::extensionsInitialized()
{
    // Retrieve objects from the plugin manager's object pool
    // In the extensionsInitialized function, a plugin can be sure that all
    // plugins that depend on it are completely initialized.
    // this->initializePythonPlugins();
}

bool PythonExtensionsPlugin::delayedInitialize() {
    // Python plugins are initialized here, to avoid blocking on startup
    this->initializePythonExtensions();
    return true;
}

ExtensionSystem::IPlugin::ShutdownFlag PythonExtensionsPlugin::aboutToShutdown()
{
    // Save settings
    // Disconnect from signals that are not needed during shutdown
    // Hide UI (if you add UI that is not in the main window directly)
    return SynchronousShutdown;
}

void PythonExtensionsPlugin::initializePythonBindings()
{
    // Initialize the Python context and register global variables

    // Core namespace / module
    // TODO: Make this more modular (have macros for the names)

    // QtCreator module
    PyUtil::createModule("QtCreator");
    // QtCreator.Core
    PyUtil::createModule("QtCreator.Core");
    PyUtil::bindObject("QtCreator.Core",
        "ActionManager", PyUtil::CoreActionManager, Core::ActionManager::instance());
    PyUtil::bindObject("QtCreator.Core",
        "ICore", PyUtil::CoreICore, Core::ICore::instance());
    // QtCreator.Core.Constants (TODO: Find better way to expose than this...)
    //                          (NOTE: One can simply use the strings directly from Python)
    PyUtil::createModule("QtCreator.Core.Constants");
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_FILE", PyUtil::CoreId, new Core::Id(Core::Constants::M_FILE));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_FILE_RECENTFILES", PyUtil::CoreId, new Core::Id(Core::Constants::M_FILE_RECENTFILES));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_EDIT", PyUtil::CoreId, new Core::Id(Core::Constants::M_EDIT));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_EDIT_ADVANCED", PyUtil::CoreId, new Core::Id(Core::Constants::M_EDIT_ADVANCED));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_TOOLS", PyUtil::CoreId, new Core::Id(Core::Constants::M_TOOLS));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_TOOLS_EXTERNAL", PyUtil::CoreId, new Core::Id(Core::Constants::M_TOOLS_EXTERNAL));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_WINDOW", PyUtil::CoreId, new Core::Id(Core::Constants::M_WINDOW));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_WINDOW_PANES", PyUtil::CoreId, new Core::Id(Core::Constants::M_WINDOW_PANES));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_WINDOW_MODESTYLES", PyUtil::CoreId, new Core::Id(Core::Constants::M_WINDOW_MODESTYLES));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_WINDOW_VIEWS", PyUtil::CoreId, new Core::Id(Core::Constants::M_WINDOW_VIEWS));
    PyUtil::bindObject("QtCreator.Core.Constants",
        "M_HELP", PyUtil::CoreId, new Core::Id(Core::Constants::M_HELP));
}

void PythonExtensionsPlugin::initializePythonExtensions()
{
    // Search python directory in plugin paths
    QDir *extension_dir;
    for (int i = 0; i < ExtensionSystem::PluginManager::pluginPaths().size(); i++) {
        extension_dir = new QDir(ExtensionSystem::PluginManager::pluginPaths()[i] + PythonExtensions::Constants::EXTENSIONS_DIR);
        if (extension_dir->exists()) {
            qDebug() << "Found Python extension directory at location" << extension_dir->absolutePath();
            break;
        }
    }

    if (!extension_dir->exists()) {
        qWarning() << "Python extension directory not found";
        delete extension_dir;
        return;
    }

    QStringList extension_names = extension_dir->entryList(QDir::AllDirs);

    // FIXME: Do other systems include exactly two 'special' files? (i.e. ./ and ../)
    qDebug() << "Number of Python extensions found:" << (extension_names.size()-2);
    int loaded = 0;

    // Run the extension initialization code
    for (int i = 0; i < extension_names.size(); i ++) {
        if (extension_names.at(i) == ".." || extension_names.at(i) == ".") {
            // FIXME: Do similar things exist in Windows?
            continue;
        }

        qDebug() << "Trying to initialize extension" << extension_names.at(i);

        QFile extension_main(extension_dir->absolutePath() + (QString)"/" + extension_names.at(i) + (QString)"/main.py");
        if (extension_main.open(QIODevice::ReadOnly)) {
            QTextStream in(&extension_main);
            QString extension_code = in.readAll();
            if (!PyUtil::runScript(extension_code.toStdString())) {
                qWarning() << "Failed to initialize extension" << extension_names.at(i);
            } else {
                loaded ++;
            }
        } else {
            qWarning() << "Failed to load main.py for extension" << extension_names.at(i);
        }
    }

    qDebug() << "Number of Python extensions loaded:" << loaded;
    delete extension_dir;
}

} // namespace Internal
} // namespace PythonExtensions
