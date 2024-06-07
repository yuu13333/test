/****************************************************************************
**
** Copyright (C) 2017 Intel Corporation.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtTest/QtTest>
#include <qplatformdefs.h>
#include <private/qfsfileengine_p.h>
#include <private/qfilesystementry_p.h>
#include <private/qfilesystemengine_p.h>
#include <private/qfilesystemmetadata_p.h>

#ifdef Q_OS_UNIX
#  include <private/qcore_unix_p.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <fcntl.h>
#  include <unistd.h>

#  define SYMLINK_EXTENSION ""
enum { SupportsSymlinks = true };
#endif

#ifdef Q_OS_WIN
#  include <qt_windows.h>
#  define access _access
#  define chmod _chmod
#  define unlink _unlink
#  define mkdir(name, x) QT_MKDIR(name)

#  ifdef Q_OS_WINRT
enum { SupportsSymlinks = false };
#  else
#    include <shlobj.h>
#    include <accctrl.h>
#    define SYMLINK_EXTENSION ".lnk"
#  endif
#endif

Q_DECLARE_METATYPE(QSystemError)

char *toString(QSystemError err)
{
    return qstrdup(err.toString().toUtf8());
}

char *toString(QFile::Permissions p)
{
    char buf[7];
    snprintf(buf, sizeof(buf), "0x%04x", int(p));
    return qstrdup(buf);
}

static QDateTime round(QDateTime dt)
{
    dt.setSecsSinceEpoch(dt.toSecsSinceEpoch());
    return dt.toLocalTime();
}

static void del(const QString &path)
{
    QVERIFY2(unlink(QFile::encodeName(path)) == 0,
             (QSystemError::stdString() + " removing file " + path).toUtf8());
};

#ifdef Q_OS_UNIX
static bool isOneOfOurGroups(gid_t gid)
{
    if (gid == getgid())
        return true;

    // get additional GIDs
    QVarLengthArray<gid_t, 16> additional(sysconf(_SC_NGROUPS_MAX));
    int n = getgroups(additional.size(), additional.data());
    if (n == -1) {
        qErrnoWarning("getgroups");
        return false;
    }

    additional.resize(n);
    return std::find(additional.begin(), additional.end(), gid) != additional.end();
}
#endif

struct FileDescriptorCloser
{
    int fd;
    ~FileDescriptorCloser() { QT_CLOSE(fd); }
};

class tst_QFileSystemEngine : public QObject
{
    Q_OBJECT
    QString m_testData;
    QString m_tempDir;

#ifdef Q_OS_WIN
    // there are several paths in C:/Windows that we should not have access to,
    // so choose one
    QString unreadablePath = "C:/windows/System32/config/";
    QString unwritablePath = "C:/";
    QString unreadableFile = "C:/pagefile.sys";
#else
    QString unreadablePath = "/root/";
    QString unwritablePath = "/";
    QString unreadableFile = "/etc/shadow";
#endif

public:
    tst_QFileSystemEngine();

private slots:
    void initTestCase();
    void cleanupTestCase();

    void fillMetaData_data();
    void fillMetaData();
#ifdef Q_OS_UNIX
    void fillMetaData_removedFile();
#endif

    void setPermissions_data();
    void setPermissions();

    void setFileTime_data();
    void setFileTime();
};

tst_QFileSystemEngine::tst_QFileSystemEngine()
{
    // confirm that the paths are as advertised
    if (access(QFile::encodeName(unreadablePath), F_OK) != 0 || access(QFile::encodeName(unreadablePath), R_OK) == 0)
        unreadablePath.clear();

    if (access(QFile::encodeName(unreadableFile), F_OK) != 0 || access(QFile::encodeName(unreadableFile), R_OK) == 0)
        unreadableFile.clear();

    if (access(QFile::encodeName(unwritablePath), F_OK) != 0 || access(QFile::encodeName(unwritablePath), R_OK) != 0
            || access(QFile::encodeName(unwritablePath), W_OK) == 0)
        unwritablePath.clear();

#ifdef Q_OS_UNIX
    if (access(QFile::encodeName(unreadablePath), X_OK) == 0)
        unreadablePath.clear();
#endif
}

void tst_QFileSystemEngine::initTestCase()
{
    m_testData = QFINDTESTDATA("testdata");
    QVERIFY(!m_testData.isEmpty());
    QVERIFY2(!m_testData.startsWith(':'), "Test data cannot be in Qt resources");
    m_testData.append('/');

    m_tempDir = QFileSystemEngine::tempPath() + "/tst_qfilesystemengine-" +
            QString::number(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());
    QVERIFY2(mkdir(QFile::encodeName(m_tempDir), 0700) == 0, QSystemError::stdString().toUtf8());
    QVERIFY2(mkdir(QFile::encodeName(m_tempDir + "/subdir"), 0700) == 0, QSystemError::stdString().toUtf8());
    m_tempDir += '/';

    auto touch = [=](const char *name, uint perms = 0600) {
        int fd = QT_OPEN(QFile::encodeName(m_tempDir + name),
                         QT_OPEN_RDWR | QT_OPEN_CREAT, perms);
        if (fd != -1)
            QT_CLOSE(fd);
        return fd != -1;
    };

    QVERIFY2(touch("dummy"), QSystemError::stdString().toUtf8());

    // used by fillMetaData
    QVERIFY2(touch("writablefile.txt", 0600), QSystemError::stdString().toUtf8());
    QVERIFY2(touch("readonlyfile.txt", 0400), QSystemError::stdString().toUtf8());

#ifdef Q_OS_UNIX
    QVERIFY2(touch("unreadablefile.txt", 0), QSystemError::stdString().toUtf8());
    QVERIFY2(mkfifo(QFile::encodeName(m_tempDir + "fifo"), 0600) == 0,
             QSystemError::stdString().toUtf8());
#endif

    // using QFileInfo here would use QFileSystemEngine, so we try with C
    // library functions (which accept Unix-style forward slashes even on
    // Windows)
    QT_STATBUF statBuffer;
    QByteArray testDir = QFile::encodeName(m_testData) ;

    QVERIFY2(QT_STAT(testDir, &statBuffer) == 0, QSystemError::stdString().toUtf8());
    QVERIFY((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_DIR);
    QVERIFY2(QT_STAT(testDir + ".hiddendirectory", &statBuffer) == 0, QSystemError::stdString().toUtf8());
    QVERIFY((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_DIR);

    QVERIFY2(QT_STAT(testDir + "emptyfile", &statBuffer) == 0, QSystemError::stdString().toUtf8());
    QVERIFY((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_REG);
    QCOMPARE(qint64(statBuffer.st_size), qint64(0));

    QVERIFY2(QT_STAT(testDir + "regularfile.txt", &statBuffer) == 0, QSystemError::stdString().toUtf8());
    QVERIFY((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_REG);
    QVERIFY(statBuffer.st_size != 0);

    QVERIFY2(QT_STAT(testDir + ".hiddenfile", &statBuffer) == 0, QSystemError::stdString().toUtf8());
    QVERIFY((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_REG);

#ifdef Q_OS_UNIX
    QVERIFY2(QT_ACCESS(testDir + "link-to-regularfile.txt", R_OK) == 0, QSystemError::stdString().toUtf8());
    QVERIFY2(QT_ACCESS(testDir + "link-to-emptyfile", R_OK) == 0, QSystemError::stdString().toUtf8());
    QVERIFY2(QT_LSTAT(testDir + "link-to-nonexistent", &statBuffer) == 0, QSystemError::stdString().toUtf8());
    QVERIFY(S_ISLNK(statBuffer.st_mode));
#else
    QVERIFY2(QT_ACCESS(testDir + "link-to-nonexistent.lnk", R_OK) == 0, QSystemError::stdString().toUtf8());

    // however, the non-broken symlink must be created now
    QFSFileEngine engine(m_testData + "regularfile.txt");
    QVERIFY2(engine.link(m_tempDir + "link-to-regularfile.txt.lnk"), engine.errorString().toUtf8());
#endif
}

void tst_QFileSystemEngine::cleanupTestCase()
{
#ifdef Q_OS_WIN
    del(m_tempDir + "link-to-regularfile.txt.lnk");
#else
    del(m_tempDir + "unreadablefile.txt");
    del(m_tempDir + "fifo");
#endif

    chmod(QFile::encodeName(m_tempDir + "readonlyfile.txt"), 0600);
    del(m_tempDir + "readonlyfile.txt");
    del(m_tempDir + "writablefile.txt");

    del(m_tempDir + "dummy");

    [=]() {
        QVERIFY2(QT_RMDIR(QFile::encodeName(m_tempDir + "subdir")) == 0, QSystemError::stdString().toUtf8());
    }();
    QVERIFY2(QT_RMDIR(QFile::encodeName(m_tempDir)) == 0, QSystemError::stdString().toUtf8());
}

void tst_QFileSystemEngine::fillMetaData_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("isLink");
    QTest::addColumn<uint>("mustHaveFlags");
    QTest::addColumn<uint>("mustNotHaveFlags");

    QTest::newRow("nonexistent")
            << "nonexistent" << false
            << 0U << uint(QFileSystemMetaData::ExistsAttribute);
    QTest::newRow("L:nonexistent")
            << "nonexistent" << true
            << 0U << uint(QFileSystemMetaData::ExistsAttribute);

    // directories
    uint expectedDirFlags = QFileSystemMetaData::DirectoryType;
    uint unexpectedDirFlags = uint(QFileSystemMetaData::FileType | QFileSystemMetaData::SequentialType |
                                   QFileSystemMetaData::HiddenAttribute);
#ifdef Q_OS_UNIX
    expectedDirFlags |= QFileSystemMetaData::ReadPermissions | QFileSystemMetaData::ExecutePermissions;
#endif
    QTest::newRow("directory")
            << m_testData << false
            << expectedDirFlags << unexpectedDirFlags;
    QTest::newRow("L:directory")
            << m_testData << true
            << expectedDirFlags << unexpectedDirFlags;
#ifdef Q_OS_UNIX
    QTest::newRow("/")
            << "/" << false
            << expectedDirFlags
            << uint(unexpectedDirFlags | (QFileSystemMetaData::WritePermissions & ~QFileSystemMetaData::OwnerWritePermission));
    QTest::newRow("TMPDIR") // might be a private dir
            << QFileSystemEngine::tempPath() << false
            << uint(QFileSystemMetaData::DirectoryType | QFileSystemMetaData::UserPermissions)
            << uint(unexpectedDirFlags);
    uint tmpHidden = 0;
#  ifdef Q_OS_MACOS
    tmpHidden = QFileSystemMetaData::HiddenAttribute;
#  endif
    QTest::newRow("/tmp")
            << "/tmp" << false
            << (expectedDirFlags | QFileSystemMetaData::WritePermissions | tmpHidden)
            << uint(unexpectedDirFlags & ~tmpHidden);
    if (access("/root", R_OK) == -1 && errno == EACCES)
        QTest::newRow("/root")
                << "/root" << false
                << uint(QFileSystemMetaData::DirectoryType | QFileSystemMetaData::OwnerPermissions)
                << uint(unexpectedDirFlags | (QFileSystemMetaData::Permissions & ~QFileSystemMetaData::OwnerPermissions));
    QTest::newRow("hiddendirectory")
            << (m_testData + ".hiddendirectory") << false
            << uint(expectedDirFlags | QFileSystemMetaData::HiddenAttribute)
            << uint(unexpectedDirFlags & ~QFileSystemMetaData::HiddenAttribute);
    QTest::newRow("L:hiddendirectory")
            << (m_testData + ".hiddendirectory") << true
            << uint(expectedDirFlags | QFileSystemMetaData::HiddenAttribute)
            << uint(unexpectedDirFlags & ~QFileSystemMetaData::HiddenAttribute);
#endif

    // files
    // files from the Git repository may be group/other writable or not
    // files we've just created aren't
    uint expectedFileFlags = QFileSystemMetaData::FileType;
    uint unexpectedFileFlags = uint(QFileSystemMetaData::DirectoryType | QFileSystemMetaData::SequentialType |
                                    QFileSystemMetaData::HiddenAttribute | QFileSystemMetaData::ExecutePermissions);
    uint unexpectedOurFileFlags = unexpectedFileFlags |
            uint(QFileSystemMetaData::GroupPermissions | QFileSystemMetaData::OtherPermissions);

#ifdef Q_OS_UNIX
    expectedFileFlags |= QFileSystemMetaData::OwnerReadPermission;
#endif
    QTest::newRow("regularfile.txt")
            << (m_testData + "regularfile.txt") << false
            << expectedFileFlags << unexpectedFileFlags;
    QTest::newRow("L:regularfile.txt")
            << (m_testData + "regularfile.txt") << true
            << expectedFileFlags << unexpectedFileFlags;
    QTest::newRow("emptyfile")
            << (m_testData + "emptyfile") << false
            << expectedFileFlags << unexpectedFileFlags;
    QTest::newRow("L:emptyfile")
            << (m_testData + "emptyfile") << true
            << expectedFileFlags << unexpectedFileFlags;    
    QTest::newRow("writablefile.txt")
            << (m_tempDir + "writablefile.txt") << false
            << uint(expectedFileFlags | QFileSystemMetaData::UserWritePermission | QFileSystemMetaData::OwnerWritePermission)
            << unexpectedOurFileFlags;
    QTest::newRow("L:writablefile.txt")
            << (m_tempDir + "writablefile.txt") << true
            << uint(expectedFileFlags | QFileSystemMetaData::UserWritePermission | QFileSystemMetaData::OwnerWritePermission)
            << unexpectedOurFileFlags;
    QTest::newRow("readonlyfile.txt")
            << (m_tempDir + "readonlyfile.txt") << false
            << uint(expectedFileFlags | QFileSystemMetaData::OwnerReadPermission)
            << uint(unexpectedOurFileFlags | QFileSystemMetaData::WritePermissions);
    QTest::newRow("L:readonlyfile.txt")
            << (m_tempDir + "readonlyfile.txt") << true
            << uint(expectedFileFlags | QFileSystemMetaData::OwnerReadPermission)
            << uint(unexpectedOurFileFlags | QFileSystemMetaData::WritePermissions);
    if (!unreadableFile.isEmpty())
        QTest::newRow("system-unreadable-file")
                << unreadableFile << false
                << uint(QFileSystemMetaData::FileType | QFileSystemMetaData::OwnerReadPermission | QFileSystemMetaData::OwnerWritePermission)
                << uint(unexpectedFileFlags | QFileSystemMetaData::UserPermissions | QFileSystemMetaData::OtherPermissions);
#ifdef Q_OS_UNIX
    QTest::newRow("unreadablefile.txt")
            << (m_tempDir + "unreadablefile.txt") << false
            << uint(expectedFileFlags & ~QFileSystemMetaData::ReadPermissions)
            << uint(unexpectedFileFlags | QFileSystemMetaData::Permissions);
    QTest::newRow("L:unreadablefile.txt")
            << (m_tempDir + "unreadablefile.txt") << true
            << uint(expectedFileFlags & ~QFileSystemMetaData::ReadPermissions)
            << uint(unexpectedFileFlags | QFileSystemMetaData::Permissions);
    QTest::newRow("hiddenfile")
            << (m_testData + ".hiddenfile") << false
            << uint(expectedFileFlags | QFileSystemMetaData::HiddenAttribute)
            << uint(unexpectedFileFlags & ~QFileSystemMetaData::HiddenAttribute);
    QTest::newRow("L:hiddenfile")
            << (m_testData + ".hiddenfile") << true
            << uint(expectedFileFlags | QFileSystemMetaData::HiddenAttribute)
            << uint(unexpectedFileFlags & ~QFileSystemMetaData::HiddenAttribute);
#endif

    // links
    QTest::newRow("link-to-regularfile.txt" SYMLINK_EXTENSION)
            << (m_testData + "link-to-regularfile.txt" SYMLINK_EXTENSION) << true
            << expectedFileFlags << unexpectedFileFlags;
    QTest::newRow("L:link-to-regularfile.txt" SYMLINK_EXTENSION)
            << (m_testData + "link-to-regularfile.txt" SYMLINK_EXTENSION) << true
            << (expectedFileFlags | QFileSystemMetaData::LegacyLinkType)
            << unexpectedFileFlags;
    QTest::newRow("link-to-nonexistent")
#ifdef Q_OS_WIN
            << (m_tempDir + "link-to-nonexistent.lnk")
#else
            << (m_testData + "link-to-nonexistent")
#endif
            << false << 0U << uint(QFileSystemMetaData::ExistsAttribute);
    QTest::newRow("L:link-to-nonexistent")
#ifdef Q_OS_WIN
            << (m_tempDir + "link-to-nonexistent.lnk")
#else
            << (m_testData + "link-to-nonexistent")
#endif
            << true << uint(QFileSystemMetaData::LegacyLinkType)
            << uint(QFileSystemMetaData::ExistsAttribute);

    // special files
#ifdef Q_OS_UNIX
    QTest::newRow("/dev/null")
            << "/dev/null" << false
            << uint(QFileSystemMetaData::ReadPermissions | QFileSystemMetaData::WritePermissions | QFileSystemMetaData::SequentialType)
            << uint(QFileSystemMetaData::ExecutePermissions | QFileSystemMetaData::HiddenAttribute);
#endif
#ifdef Q_OS_LINUX
    QTest::newRow("fifo")
            << (m_tempDir + "fifo") << false
            << uint(QFileSystemMetaData::OwnerReadPermission | QFileSystemMetaData::OwnerWritePermission |
                    QFileSystemMetaData::UserReadPermission | QFileSystemMetaData::UserWritePermission | QFileSystemMetaData::SequentialType)
            << uint(QFileSystemMetaData::ExecutePermissions | QFileSystemMetaData::GroupPermissions |
                    QFileSystemMetaData::OtherPermissions | QFileSystemMetaData::HiddenAttribute);
#endif
}

void tst_QFileSystemEngine::fillMetaData()
{
    QFETCH(QString, name);
    QFETCH(bool, isLink);
    QFETCH(uint, mustHaveFlags);
    QFETCH(uint, mustNotHaveFlags);
    QVERIFY2((mustHaveFlags & mustNotHaveFlags) == 0,
             "Malformed test - 0x" + QByteArray::number(mustHaveFlags & mustNotHaveFlags, 16));

    QFileSystemMetaData::MetaDataFlags what;
#ifdef Q_OS_WIN
    what = QFileSystemMetaData::WinStatFlags;
#else
    what = QFileSystemMetaData::PosixStatFlags | QFileSystemMetaData::HiddenAttribute
            | QFileSystemMetaData::BundleType | QFileSystemMetaData::AliasType
            | QFileSystemMetaData::UserPermissions;
#endif
    if (isLink)
        what |= QFileSystemMetaData::LegacyLinkType;

    bool mustExist = true;
    if (mustNotHaveFlags & QFileSystemMetaData::ExistsAttribute) {
        mustExist = false;
    } else {
        mustHaveFlags |= QFileSystemMetaData::ExistsAttribute;
    }
    mustHaveFlags &= what;

    QFileSystemEntry entry(name);
    QFileSystemMetaData data;
    QSystemResult<> r = QFileSystemEngine::fillMetaData(entry, data, what);

    if (!mustExist) {
        QVERIFY(r.isError());
        if (mustHaveFlags & QFileSystemMetaData::LegacyLinkType)
            QVERIFY(data.isLegacyLink());
        return;
    }

    if (r.isError())
        QVERIFY2(!r.isError(), r.error().toString().toUtf8());

    // some information is always present
    QVERIFY(data.hasFlags(QFileSystemMetaData::HiddenAttribute));
    QVERIFY(data.hasFlags(QFileSystemMetaData::SizeAttribute));
    QVERIFY(data.hasFlags(QFileSystemMetaData::ExistsAttribute));
    QVERIFY(data.hasFlags(QFileSystemMetaData::WasDeletedAttribute));
#ifdef Q_OS_WIN
    QVERIFY(data.hasFlags(QFileSystemMetaData::WinStatFlags));
#else
    QVERIFY(data.hasFlags(QFileSystemMetaData::PosixStatFlags));
#endif

#define ATTRCOMPARE(test, flag) \
    if (mustHaveFlags & (flag)) \
        QVERIFY(test); \
    else if (mustNotHaveFlags & (flag)) \
        QVERIFY(!test)
    ATTRCOMPARE(data.exists(), QFileSystemMetaData::ExistsAttribute);
    ATTRCOMPARE(data.isLegacyLink(), QFileSystemMetaData::LegacyLinkType);
    ATTRCOMPARE(data.isFile(), QFileSystemMetaData::FileType);
    ATTRCOMPARE(data.isDirectory(), QFileSystemMetaData::DirectoryType);
    ATTRCOMPARE(data.isSequential(), QFileSystemMetaData::SequentialType);
    ATTRCOMPARE(data.isHidden(), QFileSystemMetaData::HiddenAttribute);
    QVERIFY(!data.wasDeleted());

    if (mustHaveFlags & QFileSystemMetaData::Permissions) {
        QFile::Permissions perms = data.permissions();
        auto mustHavePerms = QFile::Permissions(mustHaveFlags & QFileSystemMetaData::Permissions);
        QCOMPARE(perms & mustHaveFlags, mustHavePerms);
    }
    if (mustNotHaveFlags & QFileSystemMetaData::Permissions) {
        QFile::Permissions perms = data.permissions();
        QCOMPARE(perms & mustNotHaveFlags, QFile::Permissions());
    }

#ifdef Q_OS_WIN
    QVERIFY(data.birthTime().isValid());
#endif
    QVERIFY(data.modificationTime().isValid());
    QVERIFY(data.metadataChangeTime().isValid());
    QVERIFY(data.accessTime().isValid());

    // compare some information
    QT_STATBUF statBuffer = {};
    QVERIFY2(QT_STAT(QFile::encodeName(name), &statBuffer) == 0, QSystemError::stdString().toUtf8());
    if ((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_REG) {
        QVERIFY(data.isFile());
        QVERIFY(!data.isDirectory());
        QVERIFY(!data.isSequential());
        QCOMPARE(data.size(), qint64(statBuffer.st_size));
    } else if ((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_DIR) {
        QVERIFY(!data.isFile());
        QVERIFY(data.isDirectory());
        QVERIFY(!data.isSequential());
    } else {
        QVERIFY(!data.isFile());
        QVERIFY(!data.isDirectory());
    }
    QCOMPARE(!data.wasDeleted(), statBuffer.st_nlink > 0);
    QCOMPARE(round(data.modificationTime()), QDateTime::fromSecsSinceEpoch(statBuffer.st_mtime));
    QCOMPARE(round(data.accessTime()), QDateTime::fromSecsSinceEpoch(statBuffer.st_atime));
#ifdef Q_OS_WIN
    // On Windows, st_ctime stores the birth time; no change time is stored
    QCOMPARE(round(data.birthTime()), QDateTime::fromSecsSinceEpoch(statBuffer.st_ctime));
#else
    QCOMPARE(round(data.metadataChangeTime()), QDateTime::fromSecsSinceEpoch(statBuffer.st_ctime));
#  ifdef Q_OS_BSD4
    // POSIX has no st_birthtime, though it's present on some systems
    if (statBuffer.st_birthtime)
        QCOMPARE(round(data.birthTime()), QDateTime::fromSecsSinceEpoch(statBuffer.st_birthtime));
#  endif
#endif
    if (data.hasFlags(QFileSystemMetaData::OwnerIds)) {
        QCOMPARE(data.userId(), uint(statBuffer.st_uid));
        QCOMPARE(data.groupId(), uint(statBuffer.st_gid));
    }

    // now try with a handle
    int openflags = QT_OPEN_RDONLY;
#ifdef O_PATH
    openflags |= O_PATH;    // can open (but not read) unreadable file
#endif
#ifdef O_DIRECTORY
    if (data.isDirectory())
        openflags |= O_DIRECTORY;
#else
    if (data.isDirectory()) {
        // don't try to open a directory without O_DIRECTORY
        return;
    }
#endif

    int fd = QT_OPEN(QFile::encodeName(name), openflags);
    if (fd == -1 && (mustNotHaveFlags & QFileSystemMetaData::UserReadPermission))
        return;
    QVERIFY2(fd != -1, QSystemError::stdString().toUtf8());

    // automatically close the file for is
    FileDescriptorCloser closeFd = { fd };

    QFileSystemMetaData data2;
    r = QFileSystemEngine::fillMetaData(fd, data2);
    if (r.isError())
        QVERIFY2(!r.isError(), r.error().toString().toUtf8());

#ifdef Q_OS_WIN
    QVERIFY(data2.hasFlags(QFileSystemMetaData::WinStatFlags));
    QVERIFY(data2.hasFlags(QFileSystemMetaData::ExistsAttribute));
    QVERIFY(data2.exists());
    ATTRCOMPARE(data2.isHidden(), QFileSystemMetaData::HiddenAttribute);
#else
    QVERIFY(data2.hasFlags(QFileSystemMetaData::PosixStatFlags));
    QVERIFY(!data2.hasFlags(QFileSystemMetaData::ExistsAttribute));
    QVERIFY(!data2.hasFlags(QFileSystemMetaData::HiddenAttribute));
#endif
    QVERIFY(!data2.hasFlags(QFileSystemMetaData::LegacyLinkType));

    ATTRCOMPARE(data2.isFile(), QFileSystemMetaData::FileType);
    ATTRCOMPARE(data2.isDirectory(), QFileSystemMetaData::DirectoryType);
    ATTRCOMPARE(data2.isSequential(), QFileSystemMetaData::SequentialType);
    QVERIFY(!data2.wasDeleted());

    if ((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_REG) {
        QVERIFY(data2.isFile());
        QVERIFY(!data2.isDirectory());
        QVERIFY(!data2.isSequential());
        QCOMPARE(data2.size(), qint64(statBuffer.st_size));
    } else if ((statBuffer.st_mode & QT_STAT_MASK) == QT_STAT_DIR) {
        QVERIFY(!data2.isFile());
        QVERIFY(data2.isDirectory());
        QVERIFY(!data2.isSequential());
    } else {
        QVERIFY(!data2.isFile());
        QVERIFY(!data2.isDirectory());
    }

    if (mustHaveFlags & QFileSystemMetaData::Permissions) {
        QFile::Permissions perms = data2.permissions();
        auto mustHavePerms = QFile::Permissions(mustHaveFlags & QFileSystemMetaData::Permissions
                                                & ~QFileSystemMetaData::UserPermissions);
        QCOMPARE(perms & mustHaveFlags, mustHavePerms);
    }
    if (mustNotHaveFlags & QFileSystemMetaData::Permissions) {
        QFile::Permissions perms = data2.permissions();
        QCOMPARE(perms & mustNotHaveFlags, QFile::Permissions());
    }

    QCOMPARE(data2.birthTime(), data.birthTime());
    QCOMPARE(data2.modificationTime(), data.modificationTime());
    QCOMPARE(data2.metadataChangeTime(), data.metadataChangeTime());
    QVERIFY(data2.accessTime() >= data.accessTime());  // we accessed the file...
}

#ifdef Q_OS_UNIX
void tst_QFileSystemEngine::fillMetaData_removedFile()
{
    // create the file execute-only
    QFileSystemEntry entry(m_tempDir + "removedfile.txt");
    int fd = QT_OPEN(entry.nativeFilePath(), QT_OPEN_RDWR | QT_OPEN_CREAT, 0111);
    QVERIFY2(fd != -1, QSystemError::stdString().toUtf8());
    if (unlink(entry.nativeFilePath()) == -1)
        QSKIP("Could not remove open file: " + QSystemError::stdString().toUtf8());

    // write something to it
    QVERIFY2(QT_WRITE(fd, "Hello", 5) == 5, QSystemError::stdString().toUtf8());

    QFileSystemMetaData data;
    QSystemResult<> r = QFileSystemEngine::fillMetaData(fd, data);
    if (r.isError())
        QVERIFY2(!r.isError(), r.toString().toUtf8());

    // some information is always present and some can't be calculated
    QVERIFY(!data.hasFlags(QFileSystemMetaData::HiddenAttribute));
    QVERIFY(!data.hasFlags(QFileSystemMetaData::ExistsAttribute));
    QVERIFY(!data.hasFlags(QFileSystemMetaData::UserPermissions));
    QVERIFY(data.hasFlags(QFileSystemMetaData::SizeAttribute));
    QVERIFY(data.hasFlags(QFileSystemMetaData::WasDeletedAttribute));
    QVERIFY(data.hasFlags(QFileSystemMetaData::PosixStatFlags));

    QVERIFY(data.wasDeleted()); // inode exists but file was erased

    QVERIFY(data.isFile());
    QVERIFY(!data.isDirectory());
    QVERIFY(!data.isSequential());

    QCOMPARE(data.size(), qint64(5));

    // we've just created this file, so we know the identity
    QCOMPARE(data.userId(), uint(getuid()));
    QVERIFY2(isOneOfOurGroups(data.groupId()), QByteArray::number(data.groupId()));

    QCOMPARE(data.permissions() & QFileSystemMetaData::OwnerPermissions, QFileSystemMetaData::OwnerExecutePermission);

    QVERIFY(data.modificationTime().isValid());
    QVERIFY(data.metadataChangeTime().isValid());
    QVERIFY(data.accessTime().isValid());
}
#endif

void tst_QFileSystemEngine::setPermissions_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<uint>("perms");
    QTest::addColumn<QSystemError>("expectedError");
    QTest::addColumn<bool>("tryFchmod");

#ifdef Q_OS_WIN
    QSystemError enoent(ERROR_FILE_NOT_FOUND, QSystemError::NativeError);
    auto errorFor = [](const QString &path) {
        HANDLE h = CreateFile2((const wchar_t*)path.utf16(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ,
                               OPEN_ALWAYS, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            qWarning("Whoa! I succeeded in creating file %s", qUtf8Printable(path));
            CloseHandle(h);
            del(path);
        }
        return QSystemError::lastErrorFor(QSystemError::StandardLibraryError);
    };
#else
    QSystemError enoent(ENOENT, QSystemError::StandardLibraryError);
    auto errorFor = [](const QString &path) {
        int fd = QT_OPEN(QFile::encodeName(path), QT_OPEN_RDWR | QT_OPEN_CREAT);
        if (fd != -1) {
            qWarning("Whoa! I succeeded in creating file %s", qUtf8Printable(path));
            QT_CLOSE(fd);
            del(path);
        }
        return QSystemError::lastErrorFor(QSystemError::StandardLibraryError);
    };
#endif

    QTest::newRow("nonexistent:u+r") << "nonexistent" << uint(QFile::ReadOwner) << enoent << false;

    if (!unreadablePath.isEmpty())
        QTest::newRow("unreadable") << (unreadablePath + "file.txt") << uint(QFile::WriteGroup)
                                    << errorFor(unreadablePath + "file.txt") << false;

    if (!unwritablePath.isEmpty())
    if (access(QFileSystemEngine::rootPath().toLatin1(), W_OK) == -1)
        QTest::newRow("unwritable") << unwritablePath << uint(QFile::WriteOther)
                                    << errorFor(unwritablePath) << true;

#ifdef Q_OS_WIN
    // On Windows, we only have two accessible modes: user read and user write
    // (execute permissions are implied for all directories)
    QTest::newRow("file-readonly") << (m_tempDir + "dummy")
                                   << uint(QFile::ReadOwner)
                                   << QSystemError() << true;
    QTest::newRow("dir-readonly") << (m_tempDir + "subdir")
                                  << uint(QFile::ReadOwner | QFile::ExeOwner)
                                  << QSystemError() << true;
#else
    QTest::newRow("file-124") << (m_tempDir + "dummy")
                              << uint(QFile::ExeOwner | QFile::WriteGroup | QFile::ReadGroup)
                              << QSystemError() << true;
    QTest::newRow("dir-457") << (m_tempDir + "subdir")
                             << uint(QFile::ReadOwner | QFile::ReadGroup | QFile::ExeGroup |
                                     QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)
                             << QSystemError() << true;
#endif
}

void tst_QFileSystemEngine::setPermissions()
{
    QFETCH(QString, name);
    QFETCH(uint, perms);
    QFETCH(QSystemError, expectedError);

    QFileSystemEntry entry(name);
    QFileSystemMetaData data;
    QFile::Permissions permissions(perms);
    QSystemResult<> r = QFileSystemEngine::setPermissions(entry, permissions, &data);

    QSystemError actualError;
    if (r.isError())
        actualError = r.error();
    QCOMPARE(actualError, expectedError);

    QT_STATBUF statBuffer = {};

    if (!r.isError()) {
        QVERIFY(data.hasFlags(QFileSystemMetaData::UserPermissions | QFileSystemMetaData::GroupPermissions
                              | QFileSystemMetaData::OtherPermissions));
        QCOMPARE(data.permissions(), permissions);

        // confirm it worked
        QVERIFY2(QT_STAT(QFile::encodeName(name), &statBuffer) == 0, QSystemError::stdString().toUtf8());
#ifdef Q_OS_UNIX
        QCOMPARE(bool(statBuffer.st_mode & 0001), bool(perms & QFile::ExeOther));
        QCOMPARE(bool(statBuffer.st_mode & 0002), bool(perms & QFile::WriteOther));
        QCOMPARE(bool(statBuffer.st_mode & 0004), bool(perms & QFile::ReadOther));
        QCOMPARE(bool(statBuffer.st_mode & 0010), bool(perms & QFile::ExeGroup));
        QCOMPARE(bool(statBuffer.st_mode & 0020), bool(perms & QFile::WriteGroup));
        QCOMPARE(bool(statBuffer.st_mode & 0040), bool(perms & QFile::ReadGroup));
#endif
        QCOMPARE(bool(statBuffer.st_mode & 0100), bool(perms & QFile::ExeOwner));
        QCOMPARE(bool(statBuffer.st_mode & 0200), bool(perms & QFile::WriteOwner));
        QCOMPARE(bool(statBuffer.st_mode & 0400), bool(perms & QFile::ReadOwner));

        // reset the permissions (ignore errors)
        chmod(QFile::encodeName(name), 0600 | (statBuffer.st_mode & QT_STAT_DIR ? 0100 : 0));
    }

#ifdef Q_OS_UNIX
    // Windows has no concept of fchmod(2)
    QFETCH(bool, tryFchmod);
    if (!tryFchmod)
        return;

    // now try with a handle
    int openflags = QT_OPEN_RDONLY;
#ifdef O_DIRECTORY
    if (statBuffer.st_mode & QT_STAT_DIR)
        openflags |= O_DIRECTORY;
#else
    if (statBuffer.st_mode & QT_STAT_DIR) {
        // don't try to open a directory without O_DIRECTORY
        return;
    }
#endif

    int fd = QT_OPEN(QFile::encodeName(name), openflags);
    QVERIFY2(fd != -1, QSystemError::stdString().toUtf8());

    // automatically close the file for is
    FileDescriptorCloser closeFd = { fd };

    r = QFileSystemEngine::setPermissions(fd, permissions, &data);
    if (r.isError())
        actualError = r.error();
    else
        actualError = QSystemError();
    QCOMPARE(actualError, expectedError);

    if (!r.isError()) {
        QVERIFY(data.hasFlags(QFileSystemMetaData::UserPermissions | QFileSystemMetaData::GroupPermissions
                              | QFileSystemMetaData::OtherPermissions));
        QCOMPARE(data.permissions(), permissions);

        // confirm it worked
        QVERIFY2(QT_STAT(QFile::encodeName(name), &statBuffer) == 0, QSystemError::stdString().toUtf8());
        QCOMPARE(bool(statBuffer.st_mode & 0001), bool(perms & QFile::ExeOther));
        QCOMPARE(bool(statBuffer.st_mode & 0002), bool(perms & QFile::WriteOther));
        QCOMPARE(bool(statBuffer.st_mode & 0004), bool(perms & QFile::ReadOther));
        QCOMPARE(bool(statBuffer.st_mode & 0010), bool(perms & QFile::ExeGroup));
        QCOMPARE(bool(statBuffer.st_mode & 0020), bool(perms & QFile::WriteGroup));
        QCOMPARE(bool(statBuffer.st_mode & 0040), bool(perms & QFile::ReadGroup));
        QCOMPARE(bool(statBuffer.st_mode & 0100), bool(perms & QFile::ExeOwner));
        QCOMPARE(bool(statBuffer.st_mode & 0200), bool(perms & QFile::WriteOwner));
        QCOMPARE(bool(statBuffer.st_mode & 0400), bool(perms & QFile::ReadOwner));

        // reset the permissions (ignore errors)
        fchmod(fd, 0600 | (S_ISDIR(statBuffer.st_mode) ? 0100 : 0));
    }
#endif // Q_OS_UNIX
}

void tst_QFileSystemEngine::setFileTime_data()
{
    QTest::addColumn<int>("what");
    QTest::addColumn<QDateTime>("newDate");

    // Both Unix and Windows can do atime and mtime; Windows can also change
    // birth time. On FAT filesytems, btime is a multiple of 10 ms, mtime must
    // be a multiple of 2 and atime is rounded to the day, so ensure we use
    // that.

    int perturbation = QRandomGenerator::global()->bounded(32);

    QTest::newRow("atime-past")
            << int(QAbstractFileEngine::AccessTime)
            << QDateTime(QDate(1970, 1, 2), QTime(0,0,0)).addDays(perturbation);
    QTest::newRow("atime-today")
            << int(QAbstractFileEngine::AccessTime)
            << QDateTime(QDate::currentDate(), QTime(0,0,0));
    QTest::newRow("atime-futre")
            << int(QAbstractFileEngine::AccessTime)
            << QDateTime(QDate::currentDate().addYears(1 + perturbation), QTime(0,0,0));

    QTest::newRow("mtime-past")
            << int(QAbstractFileEngine::ModificationTime)
            << QDateTime::fromSecsSinceEpoch(1 + perturbation * 2);
    QTest::newRow("mtime-now")
            << int(QAbstractFileEngine::ModificationTime)
            << QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch() * 2 / 2);
    QTest::newRow("mtime-future")
            << int(QAbstractFileEngine::ModificationTime)
            << QDateTime::fromSecsSinceEpoch((QDateTime::currentSecsSinceEpoch() + 7200 + perturbation) * 2 / 2);

#ifdef Q_OS_WIN
    QTest::newRow("btime-past")
            << int(QAbstractFileEngine::BirthTime)
            << QDateTime::fromMSecsSinceEpoch(1 + perturbation * 20);
    QTest::newRow("btime-now")
            << int(QAbstractFileEngine::BirthTime)
            << QDateTime::fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch() * 10 / 10);
    QTest::newRow("btime-future")
            << int(QAbstractFileEngine::BirthTime)
            << QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch() + 7200 + perturbation);
#endif
}

void tst_QFileSystemEngine::setFileTime()
{
    QFETCH(int, what);
    QFETCH(QDateTime, newDate);
    auto whatTime = QAbstractFileEngine::FileTime(what);

    int fd = QT_OPEN(QFile::encodeName(m_tempDir + "dummy"), QT_OPEN_RDWR);
    QVERIFY2(fd != -1, QSystemError::stdString().toUtf8());

    // automatically close the file for is
    FileDescriptorCloser closeFd = { fd };

#ifdef Q_OS_WIN
    HANDLE h = HANDLE(_get_osfhandle(fd));
    QVERIFY2(h != INVALID_HANDLE_VALUE, QSystemError::stdString().toUtf8());
#else
    int h = fd;
#endif

    QSystemError actualError;
    QSystemResult<> r = QFileSystemEngine::setFileTime(h, newDate, whatTime);
    if (r.isError())
        actualError = r.error();
    QCOMPARE(actualError, QSystemError());

    // get the time to compare
    QFileSystemMetaData data;
    r = QFileSystemEngine::fillMetaData(h, data);
    actualError = QSystemError();
    if (r.isError())
        actualError = r.error();
    QCOMPARE(actualError, QSystemError());

    QCOMPARE(data.fileTime(whatTime), newDate);
}

QTEST_MAIN(tst_QFileSystemEngine)
#include <tst_qfilesystemengine.moc>
