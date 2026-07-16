#include "CleanupEngine.h"
#include "FileManagementEngine.h"
#include "SoftwareUninstallEngine.h"
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

bool uninstallCommandsUseReliableProcessLaunches() {
    InstalledApplication desktop;
    desktop.uninstallCommand = QStringLiteral("\"C:\\Program Files\\Acme\\uninstall.exe\" /S");
    const UninstallProcessLaunch desktopLaunch = SoftwareUninstallEngine::buildProcessLaunch(desktop);
    if (!require(
            desktopLaunch.program == QStringLiteral("C:\\Program Files\\Acme\\uninstall.exe"),
            QStringLiteral("quoted desktop uninstaller launches directly")
        )
        || !require(desktopLaunch.arguments == QStringList{QStringLiteral("/S")}, QStringLiteral("desktop uninstaller arguments are preserved"))) {
        return false;
    }

    InstalledApplication unquotedDesktop;
    unquotedDesktop.uninstallCommand = QStringLiteral("C:\\Program Files\\Legacy App\\remove.exe /silent");
    const UninstallProcessLaunch unquotedLaunch = SoftwareUninstallEngine::buildProcessLaunch(unquotedDesktop);
    if (!require(unquotedLaunch.program == QStringLiteral("C:\\Program Files\\Legacy App\\remove.exe"), QStringLiteral("legacy unquoted uninstall paths are repaired"))
        || !require(unquotedLaunch.arguments == QStringList{QStringLiteral("/silent")}, QStringLiteral("legacy unquoted uninstall arguments are preserved"))) {
        return false;
    }

    InstalledApplication msi;
    msi.uninstallCommand = QStringLiteral("MsiExec.exe /I{01234567-89AB-CDEF-0123-456789ABCDEF} /qn");
    const UninstallProcessLaunch msiLaunch = SoftwareUninstallEngine::buildProcessLaunch(msi);
    if (!require(msiLaunch.program.compare(QStringLiteral("MsiExec.exe"), Qt::CaseInsensitive) == 0, QStringLiteral("MSI executable is retained"))
        || !require(msiLaunch.arguments.contains(QStringLiteral("/X{01234567-89AB-CDEF-0123-456789ABCDEF}")), QStringLiteral("MSI install mode is converted to uninstall mode"))) {
        return false;
    }

    InstalledApplication store;
    store.storeApp = true;
    store.packageFullName = QStringLiteral("Contoso.App_1.0.0.0_x64__publisher");
    const UninstallProcessLaunch storeLaunch = SoftwareUninstallEngine::buildProcessLaunch(store);
    if (!require(storeLaunch.program.compare(QStringLiteral("powershell.exe"), Qt::CaseInsensitive) == 0, QStringLiteral("Store apps use PowerShell directly"))
        || !require(storeLaunch.arguments.join(QLatin1Char(' ')).contains(QStringLiteral("Remove-AppxPackage")), QStringLiteral("Store app launch removes the selected package"))
        || !require(storeLaunch.arguments.last() == store.packageFullName, QStringLiteral("Store package identity is passed without shell quoting"))) {
        return false;
    }

    InstalledApplication shell;
    shell.uninstallCommand = QStringLiteral("helper.exe /remove && cleanup.exe");
    const UninstallProcessLaunch shellLaunch = SoftwareUninstallEngine::buildProcessLaunch(shell);
    return require(shellLaunch.program.compare(QStringLiteral("cmd.exe"), Qt::CaseInsensitive) == 0, QStringLiteral("shell expressions use cmd.exe"))
        && require(shellLaunch.arguments.contains(QStringLiteral("/C")), QStringLiteral("shell expressions are executed through cmd /C"));
}

bool storeAppResidualCleanupDoesNotTouchWindowsApps() {
    InstalledApplication store;
    store.name = QStringLiteral("Contoso App");
    store.storeApp = true;
    store.installLocation = QStringLiteral("C:\\Program Files\\WindowsApps\\Contoso.App_1.0.0.0_x64__publisher");
    return require(
        SoftwareUninstallEngine().findResidualPaths(store).isEmpty(),
        QStringLiteral("Store app residual cleanup never removes WindowsApps package directories")
    );
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

bool detailedCleanupCatalogUsesSafeDefaults() {
    const QSet<QString> expectedGroups = {
        QStringLiteral("过期文件"),
        QStringLiteral("系统相关"),
        QStringLiteral("缓存文件"),
        QStringLiteral("应用程序"),
        QStringLiteral("临时文件"),
    };
    const QSet<QString> analysisOnly = {
        QStringLiteral("winsxs_backup"),
        QStringLiteral("old_windows"),
        QStringLiteral("windows_event_logs"),
        QStringLiteral("winsxs_temp"),
        QStringLiteral("defender_protected_data"),
        QStringLiteral("redundant_packages"),
    };
    QSet<QString> groups;
    QSet<QString> ids;
    bool foundPackages = false;
    bool foundRecycleBin = false;
    bool foundAgedInstallerCache = false;
    const QVector<CleanupRule> rules = CleanupEngine::cleanupRules();
    for (const CleanupRule& rule : rules) {
        groups.insert(rule.category);
        if (!require(!ids.contains(rule.id), QStringLiteral("cleanup rule id is unique: %1").arg(rule.id))
            || !require(!rule.description.isEmpty(), QStringLiteral("cleanup rule explains its behavior: %1").arg(rule.id))
            || !require(!rule.paths.isEmpty(), QStringLiteral("cleanup rule has a real scan path: %1").arg(rule.id))) {
            return false;
        }
        ids.insert(rule.id);
        if (analysisOnly.contains(rule.id)
            && (!require(rule.scanOnly, QStringLiteral("dangerous cleanup remains analysis-only: %1").arg(rule.id))
                || !require(!rule.recommended, QStringLiteral("dangerous cleanup is not selected by default: %1").arg(rule.id)))) {
            return false;
        }
        if (rule.id == QStringLiteral("redundant_packages")) {
            foundPackages = true;
            if (!require(rule.scanOnly, QStringLiteral("user packages are analysis-only"))
                || !require(!rule.recommended, QStringLiteral("user packages are not recommended by default"))
                || !require(!rule.aggregate, QStringLiteral("user packages are displayed one file at a time"))) {
                return false;
            }
        }
        if (rule.id == QStringLiteral("recycle_bin")) {
            foundRecycleBin = true;
            if (!require(rule.aggregate, QStringLiteral("recycle bin is an aggregate cleanup operation"))
                || !require(!rule.recommended, QStringLiteral("recycle bin is not selected by default"))) {
                return false;
            }
        }
        if (rule.id == QStringLiteral("installer_cache")) {
            foundAgedInstallerCache = true;
            if (!require(rule.minimumAgeDays == 30, QStringLiteral("installer cache applies the Python Qt age threshold"))) {
                return false;
            }
        }
    }
    return require(rules.size() >= 40, QStringLiteral("cleanup catalog is fine-grained"))
        && require(groups == expectedGroups, QStringLiteral("cleanup rules use the five visible groups"))
        && require(foundPackages, QStringLiteral("package analysis rule exists"))
        && require(foundRecycleBin, QStringLiteral("recycle bin rule exists"))
        && require(foundAgedInstallerCache, QStringLiteral("aged installer cache rule exists"));
}

bool selectedRuleScanDoesNotRunTheWholeCatalog() {
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("cleanup scan temporary directory is available"))) {
        return false;
    }
    const QByteArray previousTemp = qgetenv("TEMP");
    qputenv("TEMP", directory.path().toLocal8Bit());
    const QString fixture = directory.filePath(QStringLiteral("scan-me.tmp"));
    const bool fixtureCreated = writeFile(fixture, "cleanup");
    const CleanupScanResult result = CleanupEngine().scanRules({QStringLiteral("user_temp")});
    if (previousTemp.isNull()) {
        qunsetenv("TEMP");
    } else {
        qputenv("TEMP", previousTemp);
    }
    if (!require(fixtureCreated, QStringLiteral("cleanup scan fixture is created"))) {
        return false;
    }
    QSet<QString> scannedRuleIds;
    for (const CleanupEntry& entry : result.entries) {
        scannedRuleIds.insert(entry.ruleId);
    }
    return require(scannedRuleIds == QSet<QString>{QStringLiteral("user_temp")}, QStringLiteral("only checked cleanup rules are scanned"))
        && require(result.totalBytes == 7, QStringLiteral("selected cleanup rule reports the matching file size"));
}

bool agedCleanupRulesIgnoreRecentFiles() {
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("aged cleanup temporary directory is available"))) {
        return false;
    }
    const QString downloads = directory.filePath(QStringLiteral("Downloads"));
    QDir().mkpath(downloads);
    const QString oldPath = QDir(downloads).filePath(QStringLiteral("old.part"));
    const QString recentPath = QDir(downloads).filePath(QStringLiteral("recent.part"));
    if (!require(writeFile(oldPath, "old") && writeFile(recentPath, "recent"), QStringLiteral("aged cleanup fixtures are created"))) {
        return false;
    }
    QFile oldFile(oldPath);
    if (!require(oldFile.open(QIODevice::ReadWrite), QStringLiteral("aged cleanup fixture can be opened"))
        || !require(oldFile.setFileTime(QDateTime::currentDateTime().addDays(-8), QFileDevice::FileModificationTime), QStringLiteral("aged cleanup fixture timestamp is set"))) {
        return false;
    }
    oldFile.close();

    const QByteArray previousProfile = qgetenv("USERPROFILE");
    qputenv("USERPROFILE", directory.path().toLocal8Bit());
    const CleanupScanResult result = CleanupEngine().scanRules({QStringLiteral("download_residuals")});
    if (previousProfile.isNull()) {
        qunsetenv("USERPROFILE");
    } else {
        qputenv("USERPROFILE", previousProfile);
    }

    QStringList matches;
    for (const CleanupEntry& entry : result.entries) {
        matches.append(entry.files);
    }
    return require(matches.contains(oldPath), QStringLiteral("old download residue is included"))
        && require(!matches.contains(recentPath), QStringLiteral("recent download residue is excluded"));
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

bool safeOptimizationCatalogIsCategorizedAndReversible() {
    const QSet<QString> required = {
        QStringLiteral("office_explorer_extensions"),
        QStringLiteral("office_quick_access_history"),
        QStringLiteral("office_advertising_id"),
        QStringLiteral("office_game_capture"),
        QStringLiteral("office_edge_background"),
    };
    QSet<QString> found;
    QSet<QString> ids;
    for (const WindowsOptimizationAction& action : SystemCatalog::officeOptimizationActions()) {
        if (!require(!ids.contains(action.id), QStringLiteral("office optimization id is unique: %1").arg(action.id))
            || !require(!action.category.isEmpty(), QStringLiteral("office optimization is categorized: %1").arg(action.id))
            || !require(!action.commands.isEmpty(), QStringLiteral("office optimization has a real operation: %1").arg(action.id))) {
            return false;
        }
        ids.insert(action.id);
        if (required.contains(action.id)) {
            found.insert(action.id);
            if (!require(!action.revertCommands.isEmpty(), QStringLiteral("new optimization is reversible: %1").arg(action.id))) {
                return false;
            }
        }
    }
    return require(found == required, QStringLiteral("all independently implemented optimization categories are present"));
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const bool ok = chineseUiLiteralsUseUtf8()
        && restoreDoesNotOverwriteCurrentFile()
        && pruningKeepsNewestBackups()
        && batchRenamePreservesExtensions()
        && folderScanRetainsOnlyLargestRequestedFiles()
        && uninstallCommandsUseReliableProcessLaunches()
        && storeAppResidualCleanupDoesNotTouchWindowsApps()
        && ordinaryMigrationCreatesShortcutAndHandlesQuotes()
        && detailedCleanupCatalogUsesSafeDefaults()
        && selectedRuleScanDoesNotRunTheWholeCatalog()
        && agedCleanupRulesIgnoreRecentFiles()
        && criticalOptimizationActionsProvideInverseOperations()
        && safeOptimizationCatalogIsCategorizedAndReversible();
    return ok ? 0 : 1;
}
