#include "CleanupEngine.h"
#include "FileManagementEngine.h"
#include "SystemCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

namespace {

bool writeFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

bool require(bool condition, const QString& message) {
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }
    return condition;
}

bool chineseUiLiteralsUseUtf8() {
    const QVector<uint> expected = {
        0x43, 0x20, 0x76d8, 0x6e05, 0x7406, 0x5927, 0x5e08,
    };
    return require(
        QStringLiteral("C 盘清理大师").toUcs4() == expected,
        QStringLiteral("Chinese UI literals are compiled as UTF-8")
    );
}

bool restoreDoesNotOverwriteCurrentFile() {
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("temporary directory is available"))) {
        return false;
    }

    const QString source = directory.filePath(QStringLiteral("current.txt"));
    const QString backup = directory.filePath(QStringLiteral("backup.txt"));
    if (!require(writeFile(source, "current"), QStringLiteral("current file is created"))
        || !require(writeFile(backup, "backup"), QStringLiteral("backup file is created"))) {
        return false;
    }

    BackupRecord record;
    record.sourcePath = source;
    record.backupPath = backup;
    QString error;
    const bool restored = CleanupEngine::restoreBackupItem(record, &error);
    return require(!restored, QStringLiteral("restore refuses to overwrite an existing file"))
        && require(readFile(source) == QByteArray("current"), QStringLiteral("existing content is preserved"))
        && require(error.contains(QStringLiteral("已存在")), QStringLiteral("restore explains the conflict"));
}

bool pruningKeepsNewestBackups() {
    QTemporaryDir directory;
    const QString source = directory.filePath(QStringLiteral("source.txt"));
    const QString backupRoot = directory.filePath(QStringLiteral("backups"));
    CleanupEntry entry;
    entry.path = source;
    entry.files = QStringList{source};
    CleanOptions options;
    options.backup = true;
    options.backupRoot = backupRoot;

    for (const QByteArray& content : {QByteArray("v1"), QByteArray("v2"), QByteArray("v3")}) {
        if (!require(writeFile(source, content), QStringLiteral("source version is created"))) {
            return false;
        }
        entry.size = content.size();
        const CleanResult result = CleanupEngine().cleanEntriesDetailed({entry}, options);
        if (!require(result.errors.isEmpty(), QStringLiteral("source version is backed up"))) {
            return false;
        }
        QThread::msleep(5);
    }

    if (!require(CleanupEngine::pruneBackups(backupRoot, 10, 4), QStringLiteral("backup pruning succeeds"))) {
        return false;
    }
    const BackupInfo info = CleanupEngine::backupInfo(backupRoot);
    if (!require(info.backups.size() == 2, QStringLiteral("two newest backups remain"))) {
        return false;
    }
    return require(readFile(info.backups.at(0).backupPath) == QByteArray("v3"), QStringLiteral("newest backup remains"))
        && require(readFile(info.backups.at(1).backupPath) == QByteArray("v2"), QStringLiteral("second-newest backup remains"));
}

bool batchRenamePreservesExtensions() {
    QTemporaryDir directory;
    const QString first = directory.filePath(QStringLiteral("first.txt"));
    const QString second = directory.filePath(QStringLiteral("second.log"));
    if (!require(writeFile(first, "one") && writeFile(second, "two"), QStringLiteral("rename fixtures are created"))) {
        return false;
    }
    const FileOperationResult result = FileManagementEngine().renameFiles({first, second}, QStringLiteral("report"));
    return require(result.errors.isEmpty(), QStringLiteral("batch rename succeeds"))
        && require(QFileInfo::exists(directory.filePath(QStringLiteral("report_001.txt"))), QStringLiteral("first extension is preserved"))
        && require(QFileInfo::exists(directory.filePath(QStringLiteral("report_002.log"))), QStringLiteral("second extension is preserved"));
}

bool folderScanRetainsOnlyLargestRequestedFiles() {
    QTemporaryDir directory;
    for (int index = 1; index <= 10; ++index) {
        if (!require(writeFile(directory.filePath(QStringLiteral("%1.bin").arg(index)), QByteArray(index, 'x')), QStringLiteral("scan fixture is created"))) {
            return false;
        }
    }
    const FolderUsageScan scan = FileManagementEngine().scanFolderUsageDetailed(directory.path(), 20, 3);
    int countedFiles = 0;
    for (const ExtensionUsageEntry& entry : scan.extensions) {
        countedFiles += entry.fileCount;
    }
    return require(scan.files.size() == 3, QStringLiteral("scan retains the requested number of treemap files"))
        && require(scan.files.at(0).sizeBytes == 10, QStringLiteral("largest file is retained"))
        && require(scan.files.at(2).sizeBytes == 8, QStringLiteral("third-largest file is retained"))
        && require(countedFiles == 10, QStringLiteral("extension totals still include every file"));
}

bool ordinaryMigrationCreatesShortcutAndHandlesQuotes() {
#ifdef Q_OS_WIN
    QTemporaryDir directory;
    const QString sourceDirectory = directory.filePath(QStringLiteral("owner's files"));
    const QString targetDirectory = directory.filePath(QStringLiteral("target"));
    QDir().mkpath(sourceDirectory);
    QDir().mkpath(targetDirectory);
    const QString source = QDir(sourceDirectory).filePath(QStringLiteral("report.txt"));
    if (!require(writeFile(source, "payload"), QStringLiteral("migration fixture is created"))) {
        return false;
    }
    const FileOperationResult result = FileManagementEngine().migrateFilesWithShortcuts({source}, targetDirectory);
    return require(result.errors.isEmpty(), QStringLiteral("ordinary migration succeeds"))
        && require(QFileInfo::exists(QDir(targetDirectory).filePath(QStringLiteral("report.txt"))), QStringLiteral("file is moved"))
        && require(QFileInfo::exists(source + QStringLiteral(".lnk")), QStringLiteral("shortcut is created at the original path"));
#else
    return true;
#endif
}

bool riskyCleanupRulesRequireDeepPerFileSelection() {
    bool foundPackages = false;
    bool foundRecycleBin = false;
    for (const CleanupRule& rule : CleanupEngine::cleanupRules()) {
        if (rule.id == QStringLiteral("redundant_packages")) {
            foundPackages = true;
            if (!require(rule.scanOnly, QStringLiteral("user packages require deep mode"))
                || !require(!rule.recommended, QStringLiteral("user packages are not recommended by default"))
                || !require(!rule.aggregate, QStringLiteral("user packages are displayed one file at a time"))) {
                return false;
            }
        }
        if (rule.id == QStringLiteral("recycle_bin")) {
            foundRecycleBin = true;
            if (!require(rule.aggregate, QStringLiteral("recycle bin is the only aggregate cleanup operation"))) {
                return false;
            }
        }
    }
    return require(foundPackages, QStringLiteral("package cleanup rule exists"))
        && require(foundRecycleBin, QStringLiteral("recycle bin rule exists"));
}

bool criticalOptimizationActionsProvideInverseOperations() {
    const QSet<QString> required = {
        QStringLiteral("windows_update_disable"),
        QStringLiteral("windows_update_enable"),
        QStringLiteral("edge_install"),
        QStringLiteral("edge_remove"),
        QStringLiteral("browser_homepage_fix"),
    };
    QSet<QString> found;
    for (const WindowsOptimizationAction& action : SystemCatalog::advancedControlActions()) {
        if (required.contains(action.id)) {
            found.insert(action.id);
            if (!require(!action.revertCommands.isEmpty(), QStringLiteral("critical optimization has an inverse: %1").arg(action.id))) {
                return false;
            }
        }
    }
    return require(found == required, QStringLiteral("all critical reversible optimizations are present"));
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const bool ok = chineseUiLiteralsUseUtf8()
        && restoreDoesNotOverwriteCurrentFile()
        && pruningKeepsNewestBackups()
        && batchRenamePreservesExtensions()
        && folderScanRetainsOnlyLargestRequestedFiles()
        && ordinaryMigrationCreatesShortcutAndHandlesQuotes()
        && riskyCleanupRulesRequireDeepPerFileSelection()
        && criticalOptimizationActionsProvideInverseOperations();
    return ok ? 0 : 1;
}
