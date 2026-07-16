#include "CleanupEngine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

QStringList safeGlobPatterns(const CleanupRule& rule) {
    return rule.patterns.isEmpty() ? QStringList{QStringLiteral("*")} : rule.patterns;
}

bool containsAnyMarker(const QString& path, const QStringList& markers) {
    if (markers.isEmpty()) {
        return true;
    }
    const QString lowered = QDir::toNativeSeparators(path).toLower();
    for (const QString& marker : markers) {
        if (lowered.contains(marker.toLower())) {
            return true;
        }
    }
    return false;
}

bool oldEnough(const QFileInfo& info, int minimumAgeDays) {
    if (minimumAgeDays <= 0 || !info.lastModified().isValid()) {
        return true;
    }
    return info.lastModified().daysTo(QDateTime::currentDateTime()) >= minimumAgeDays;
}

QString uniqueBackupPath(const QString& backupRoot, const QString& source) {
    const QFileInfo info(source);
    const QString normalized = QDir::toNativeSeparators(info.absoluteFilePath()).toLower();
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex().left(16)
    );
    const QString destinationDir = QDir(backupRoot).filePath(digest);
    QDir().mkpath(destinationDir);

    const QString baseName = info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName();
    const QString suffix = info.suffix();
    QString candidate = QDir(destinationDir).filePath(info.fileName());
    int counter = 1;
    while (QFileInfo::exists(candidate)) {
        const QString numbered = suffix.isEmpty()
            ? QStringLiteral("%1-%2").arg(baseName).arg(counter)
            : QStringLiteral("%1-%2.%3").arg(baseName).arg(counter).arg(suffix);
        candidate = QDir(destinationDir).filePath(numbered);
        ++counter;
    }
    return candidate;
}

QString manifestPath(const QString& backupRoot) {
    return QDir(backupRoot).filePath(QStringLiteral("manifest.tsv"));
}

QString escapeManifestField(QString value) {
    value.replace(QLatin1Char('\t'), QStringLiteral(" "));
    value.replace(QLatin1Char('\n'), QStringLiteral(" "));
    return value;
}

void appendBackupManifest(const QString& backupRoot, const QString& source, const QString& backupPath, qint64 size) {
    QFile file(manifestPath(backupRoot));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    const QString id = QString::fromLatin1(QCryptographicHash::hash(
        QStringLiteral("%1|%2|%3").arg(source, backupPath, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)).toUtf8(),
        QCryptographicHash::Sha256
    ).toHex());
    stream << id << '\t'
           << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << '\t'
           << size << '\t'
           << escapeManifestField(source) << '\t'
           << escapeManifestField(backupPath) << '\n';
}

}  // namespace

CleanupEngine::CleanupEngine() = default;

DiskInfo CleanupEngine::diskInfo() const {
#ifdef Q_OS_WIN
    QStorageInfo storage(QStringLiteral("C:/"));
#else
    QStorageInfo storage = QStorageInfo::root();
#endif
    storage.refresh();
    DiskInfo info;
    info.totalBytes = storage.bytesTotal();
    info.freeBytes = storage.bytesAvailable();
    info.usedBytes = qMax<qint64>(0, info.totalBytes - info.freeBytes);
    return info;
}

CleanupScanResult CleanupEngine::scanSystem(const ProgressCallback& progress) {
    return scanRules({}, progress);
}

CleanupScanResult CleanupEngine::scanRules(
    const QSet<QString>& ruleIds,
    const ProgressCallback& progress
) {
    CleanupScanResult result;
    int count = 0;
    QSet<QString> seenFiles;
    for (const CleanupRule& rule : cleanupRules()) {
        if (!ruleIds.isEmpty() && !ruleIds.contains(rule.id)) {
            continue;
        }
        for (const QString& rootPath : rule.paths) {
            if (progress) {
                progress(rootPath, count);
            }
            QStringList files;
            qint64 totalSize = 0;
            collectRuleFiles(rule, rootPath, &files, &totalSize, progress, &count);
            QStringList uniqueFiles;
            qint64 uniqueSize = 0;
            for (const QString& file : files) {
                const QString key = QDir::cleanPath(QFileInfo(file).absoluteFilePath()).toLower();
                if (seenFiles.contains(key)) {
                    continue;
                }
                seenFiles.insert(key);
                uniqueFiles.push_back(file);
                uniqueSize += QFileInfo(file).size();
            }
            files = uniqueFiles;
            totalSize = uniqueSize;
            if (files.isEmpty() && totalSize <= 0) {
                continue;
            }
            const auto appendEntry = [&result, &rule](const QString& path, const QStringList& entryFiles, qint64 size, const QString& title) {
                CleanupEntry entry;
                entry.ruleId = rule.id;
                entry.category = rule.category;
                entry.title = title;
                entry.riskLabel = rule.riskLabel;
                entry.path = path;
                entry.files = entryFiles;
                entry.size = size;
                entry.scanOnly = rule.scanOnly;
                entry.recommended = rule.recommended;
                entry.professional = rule.professional;
                entry.privacySensitive = rule.privacySensitive;
                result.entries.push_back(entry);
                result.totalBytes += size;
                if (!rule.scanOnly && rule.recommended) {
                    result.recommendedBytes += size;
                }
                if (rule.professional) {
                    result.professionalBytes += size;
                }
            };
            if (rule.aggregate) {
                appendEntry(rootPath, files, totalSize, rule.title);
            } else {
                for (const QString& file : files) {
                    const QFileInfo info(file);
                    appendEntry(file, {file}, info.size(), info.fileName().isEmpty() ? rule.title : info.fileName());
                }
            }
        }
    }
    result.scannedCount = count;
    return result;
}

qint64 CleanupEngine::cleanEntries(
    const QVector<CleanupEntry>& entries,
    const CleanOptions& options,
    const ProgressCallback& progress
) const {
    return cleanEntriesDetailed(entries, options, progress).cleanedBytes;
}

CleanResult CleanupEngine::cleanEntriesDetailed(
    const QVector<CleanupEntry>& entries,
    const CleanOptions& options,
    const ProgressCallback& progress
) const {
    CleanResult result;
    int count = 0;
    const QString root = options.backupRoot.isEmpty() ? defaultBackupRoot() : options.backupRoot;
    if (options.backup) {
        pruneBackups(root, 5000, 1024LL * 1024LL * 1024LL);
    }

    for (const CleanupEntry& entry : entries) {
        if (entry.scanOnly && !options.allowScanOnly) {
            result.skippedCount += qMax(1, entry.files.size());
            continue;
        }
        if (entry.ruleId == QStringLiteral("recycle_bin")) {
            ++result.attemptedCount;
            if (options.simulate) {
                result.cleanedBytes += entry.size;
                ++result.deletedCount;
                continue;
            }
            QString error;
            if (emptyRecycleBin(&error)) {
                result.cleanedBytes += entry.size;
                ++result.deletedCount;
            } else {
                ++result.skippedCount;
                result.errors.push_back(error);
            }
            continue;
        }

        for (const QString& path : entry.files) {
            if (progress) {
                progress(path, count);
            }
            ++count;
            ++result.attemptedCount;
            const qint64 size = fileSize(path);
            if (options.simulate) {
                result.cleanedBytes += size;
                ++result.deletedCount;
                continue;
            }
            if (options.backup) {
                QString backupError;
                if (!backupFile(path, root, &backupError)) {
                    ++result.skippedCount;
                    result.errors.push_back(backupError);
                    continue;
                }
            }
            QString error;
            if (deletePath(path, &error)) {
                result.cleanedBytes += size;
                ++result.deletedCount;
            } else {
                ++result.skippedCount;
                result.errors.push_back(error);
            }
        }
    }
    return result;
}

QVector<CleanupRule> CleanupEngine::cleanupRules() {
    const QString userProfile = envPath(QStringLiteral("USERPROFILE"), QStringLiteral("C:\\Users\\Administrator"));
    const QString localAppData = envPath(QStringLiteral("LOCALAPPDATA"), winJoin({userProfile, QStringLiteral("AppData"), QStringLiteral("Local")}));
    const QString appData = envPath(QStringLiteral("APPDATA"), winJoin({userProfile, QStringLiteral("AppData"), QStringLiteral("Roaming")}));
    const QString programData = envPath(QStringLiteral("ProgramData"), QStringLiteral("C:\\ProgramData"));
    const QString systemRoot = envPath(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    const QString systemDrive = envPath(QStringLiteral("SystemDrive"), QStringLiteral("C:"));

    QVector<CleanupRule> rules;
    auto add = [&rules](
        const QString& id,
        const QString& category,
        const QString& title,
        const QString& description,
        const QString& risk,
        const QStringList& paths,
        const QStringList& patterns = {},
        const QStringList& pathContains = {},
        bool recommended = true,
        bool scanOnly = false,
        bool aggregate = true,
        int minimumAgeDays = 0,
        bool privacySensitive = false
    ) {
        CleanupRule rule;
        rule.id = id;
        rule.category = category;
        rule.title = title;
        rule.description = description;
        rule.riskLabel = risk;
        rule.paths = paths;
        rule.patterns = patterns;
        rule.pathContains = pathContains;
        rule.minimumAgeDays = minimumAgeDays;
        rule.scanOnly = scanOnly;
        rule.aggregate = aggregate;
        rule.recommended = recommended;
        rule.professional = true;
        rule.privacySensitive = privacySensitive;
        rules.push_back(rule);
    };

    add(
        QStringLiteral("winsxs_backup"), QStringLiteral("过期文件"), QStringLiteral("WinSxS 备份组件（仅分析）"),
        QStringLiteral("统计组件存储中的备份文件。该目录必须由 DISM/CBS 维护，本工具不会直接删除。"),
        QStringLiteral("高风险"), {winJoin({systemRoot, QStringLiteral("WinSxS"), QStringLiteral("Backup")})}, {}, {}, false, true
    );
    add(
        QStringLiteral("old_windows"), QStringLiteral("过期文件"), QStringLiteral("旧 Windows 安装文件（仅分析）"),
        QStringLiteral("Windows.old 与系统升级残留可能用于版本回退，只统计空间并建议通过系统存储设置清理。"),
        QStringLiteral("高风险"), {winJoin({systemDrive, QStringLiteral("Windows.old")}), winJoin({systemDrive, QStringLiteral("$Windows.~BT")}), winJoin({systemDrive, QStringLiteral("$Windows.~WS")})},
        {}, {}, false, true
    );
    add(
        QStringLiteral("service_pack_backups"), QStringLiteral("过期文件"), QStringLiteral("旧服务包卸载备份"),
        QStringLiteral("旧版 Windows 服务包留下的卸载备份，现代系统通常不存在。"),
        QStringLiteral("谨慎"), {winJoin({systemRoot, QStringLiteral("$NtServicePackUninstall$")}), winJoin({systemRoot, QStringLiteral("$hf_mig$")})},
        {}, {}, false
    );
    add(
        QStringLiteral("windows_update_cache"), QStringLiteral("过期文件"), QStringLiteral("Windows 更新下载缓存"),
        QStringLiteral("已下载的更新包与临时文件。正在安装更新时可能被占用，默认不勾选。"),
        QStringLiteral("谨慎"), {winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("Download")}), winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("Temp")})},
        {}, {}, false
    );
    add(
        QStringLiteral("delivery_optimization"), QStringLiteral("过期文件"), QStringLiteral("Windows 传递优化缓存"),
        QStringLiteral("Windows 更新在局域网或互联网分发时保存的下载缓存，可由系统重新生成。"),
        QStringLiteral("安全"), {winJoin({systemRoot, QStringLiteral("ServiceProfiles"), QStringLiteral("NetworkService"), QStringLiteral("AppData"), QStringLiteral("Local"), QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("DeliveryOptimization"), QStringLiteral("Cache")}), winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("DeliveryOptimization"), QStringLiteral("Cache")})}
    );
    add(
        QStringLiteral("backup_temp"), QStringLiteral("过期文件"), QStringLiteral("30 天前的备份临时文件"),
        QStringLiteral("Windows Backup 产生的日志和临时文件，仅匹配 30 天前的内容。"),
        QStringLiteral("安全"), {winJoin({systemRoot, QStringLiteral("Temp"), QStringLiteral("WindowsBackup")}), winJoin({systemRoot, QStringLiteral("Logs"), QStringLiteral("WindowsBackup")}), winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WindowsBackup")})},
        {}, {}, true, false, true, 30
    );
    add(
        QStringLiteral("installer_cache"), QStringLiteral("过期文件"), QStringLiteral("30 天前的安装程序缓存"),
        QStringLiteral("只匹配安装缓存目录中的临时、日志和旧文件；不直接删除有效 MSI/MSP 安装源。"),
        QStringLiteral("谨慎"), {winJoin({systemRoot, QStringLiteral("Installer"), QStringLiteral("Temp")}), winJoin({programData, QStringLiteral("Package Cache"), QStringLiteral("Temp")}), winJoin({localAppData, QStringLiteral("Package Cache")}), winJoin({localAppData, QStringLiteral("Temp"), QStringLiteral("Downloaded Installations")})},
        {QStringLiteral("*.tmp"), QStringLiteral("*.temp"), QStringLiteral("*.msi.cache"), QStringLiteral("*.msp.cache"), QStringLiteral("*.exe.cache"), QStringLiteral("*.log"), QStringLiteral("*.old")},
        {}, false, false, true, 30
    );

    add(
        QStringLiteral("error_reports"), QStringLiteral("系统相关"), QStringLiteral("Windows 错误报告"),
        QStringLiteral("系统和应用崩溃后生成的 WER 报告，删除后只会失去旧故障诊断记录。"),
        QStringLiteral("安全"), {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER")}), winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER")})}
    );
    add(
        QStringLiteral("windows_event_logs"), QStringLiteral("系统相关"), QStringLiteral("Windows 事件日志（仅分析）"),
        QStringLiteral("事件日志用于系统审计和故障排查。本工具只统计，不直接删除或清空事件通道。"),
        QStringLiteral("高风险"), {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("winevt"), QStringLiteral("Logs")})},
        {QStringLiteral("*.evtx")}, {}, false, true
    );
    add(
        QStringLiteral("setup_logs"), QStringLiteral("系统相关"), QStringLiteral("Windows 安装与设备日志"),
        QStringLiteral("系统安装、升级和设备安装留下的日志文件。"),
        QStringLiteral("安全"), {winJoin({systemRoot, QStringLiteral("Panther")}), winJoin({systemRoot, QStringLiteral("INF")}), winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("LogFiles"), QStringLiteral("setupapi")})},
        {QStringLiteral("*.log"), QStringLiteral("*.etl"), QStringLiteral("*.tmp")}
    );
    add(
        QStringLiteral("system_logs"), QStringLiteral("系统相关"), QStringLiteral("Windows 系统日志"),
        QStringLiteral("Windows Logs 与 debug 目录中的旧日志、跟踪和临时文件。"),
        QStringLiteral("安全"), {winJoin({systemRoot, QStringLiteral("Logs")}), winJoin({systemRoot, QStringLiteral("debug")})},
        {QStringLiteral("*.log"), QStringLiteral("*.etl"), QStringLiteral("*.tmp"), QStringLiteral("*.dmp")}
    );
    add(
        QStringLiteral("memory_dumps"), QStringLiteral("系统相关"), QStringLiteral("系统内存转储文件"),
        QStringLiteral("蓝屏和系统故障生成的 MEMORY.DMP 与 Minidump，适合在问题排查完成后清理。"),
        QStringLiteral("安全"), {winJoin({systemRoot, QStringLiteral("Minidump")}), winJoin({systemRoot, QStringLiteral("MEMORY.DMP")})},
        {QStringLiteral("*.dmp"), QStringLiteral("MEMORY.DMP")}
    );
    add(
        QStringLiteral("app_crash"), QStringLiteral("系统相关"), QStringLiteral("应用程序崩溃转储"),
        QStringLiteral("应用崩溃后写入当前用户 CrashDumps 目录的转储文件。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("CrashDumps")})}, {QStringLiteral("*.dmp")}
    );
    add(
        QStringLiteral("update_temp"), QStringLiteral("系统相关"), QStringLiteral("Windows 更新临时文件"),
        QStringLiteral("更新完成后遗留的重启事件缓存、Temp 和 TrustedInstaller 临时文件。"),
        QStringLiteral("谨慎"), {winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("PostRebootEventCache")}), winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("Temp")}), winJoin({systemRoot, QStringLiteral("Temp"), QStringLiteral("TrustedInstaller")})},
        {}, {}, false
    );
    add(
        QStringLiteral("winsxs_temp"), QStringLiteral("系统相关"), QStringLiteral("WinSxS 临时文件（仅分析）"),
        QStringLiteral("组件存储临时目录由 TrustedInstaller/CBS 管理，只统计占用，不执行直接删除。"),
        QStringLiteral("高风险"), {winJoin({systemRoot, QStringLiteral("WinSxS"), QStringLiteral("Temp")})}, {}, {}, false, true
    );
    add(
        QStringLiteral("driver_temp"), QStringLiteral("系统相关"), QStringLiteral("驱动安装临时文件"),
        QStringLiteral("驱动安装留下的 OLD 与 DriverStore Temp 内容，更新驱动期间不应清理。"),
        QStringLiteral("谨慎"), {winJoin({systemRoot, QStringLiteral("inf"), QStringLiteral("OLD")}), winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("DriverStore"), QStringLiteral("Temp")})},
        {}, {}, false
    );
    add(
        QStringLiteral("search_index_temp"), QStringLiteral("系统相关"), QStringLiteral("Windows 搜索索引临时文件"),
        QStringLiteral("仅匹配搜索索引目录中的 tmp、old、bak 和 log 文件，不删除主索引数据库。"),
        QStringLiteral("安全"), {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Search"), QStringLiteral("Data"), QStringLiteral("Temp")}), winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Search"), QStringLiteral("Data"), QStringLiteral("Applications"), QStringLiteral("Windows")})},
        {QStringLiteral("*.tmp"), QStringLiteral("*.old"), QStringLiteral("*.bak"), QStringLiteral("*.log")}
    );
    add(
        QStringLiteral("font_cache"), QStringLiteral("系统相关"), QStringLiteral("Windows 字体缓存"),
        QStringLiteral("字体缓存损坏时可清理并由系统重建；正常情况下默认不选。"),
        QStringLiteral("谨慎"), {winJoin({systemRoot, QStringLiteral("ServiceProfiles"), QStringLiteral("LocalService"), QStringLiteral("AppData"), QStringLiteral("Local"), QStringLiteral("FontCache")}), winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("FNTCACHE.DAT")})},
        {}, {}, false
    );
    add(
        QStringLiteral("defender_support"), QStringLiteral("系统相关"), QStringLiteral("Defender 支持日志"),
        QStringLiteral("Microsoft Defender Support 目录中的诊断日志，不包含隔离文件。"),
        QStringLiteral("谨慎"), {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows Defender"), QStringLiteral("Support")})},
        {}, {}, false
    );
    add(
        QStringLiteral("defender_protected_data"), QStringLiteral("系统相关"), QStringLiteral("Defender 历史与隔离区（仅分析）"),
        QStringLiteral("保护历史和隔离区可能包含安全审计证据或可恢复文件，只统计空间。"),
        QStringLiteral("高风险"), {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows Defender"), QStringLiteral("Scans"), QStringLiteral("History")}), winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows Defender"), QStringLiteral("Quarantine")})},
        {}, {}, false, true
    );
    add(
        QStringLiteral("printer_queue"), QStringLiteral("系统相关"), QStringLiteral("打印队列临时文件"),
        QStringLiteral("打印后台处理目录中的任务文件。清理前必须确认没有待打印任务。"),
        QStringLiteral("谨慎"), {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("spool"), QStringLiteral("PRINTERS")}), winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("spool"), QStringLiteral("SERVERS")})},
        {}, {}, false
    );

    add(
        QStringLiteral("user_temp"), QStringLiteral("缓存文件"), QStringLiteral("当前用户临时文件"),
        QStringLiteral("TEMP 目录中的应用临时文件，正在使用的文件会因占用而自动跳过。"),
        QStringLiteral("安全"), {envPath(QStringLiteral("TEMP"), winJoin({localAppData, QStringLiteral("Temp")}))}
    );
    add(
        QStringLiteral("windows_temp"), QStringLiteral("缓存文件"), QStringLiteral("Windows 临时文件"),
        QStringLiteral("Windows Temp 目录中的系统临时文件，清理前会逐文件备份。"),
        QStringLiteral("安全"), {winJoin({systemRoot, QStringLiteral("Temp")})}
    );
    add(
        QStringLiteral("prefetch"), QStringLiteral("缓存文件"), QStringLiteral("Windows 预读取文件"),
        QStringLiteral("系统用于加速程序启动的 Prefetch 缓存。可重建，但正常情况下不建议频繁清理。"),
        QStringLiteral("可选"), {winJoin({systemRoot, QStringLiteral("Prefetch")})}, {QStringLiteral("*.pf")}, {}, false
    );
    add(
        QStringLiteral("thumbnail_cache"), QStringLiteral("缓存文件"), QStringLiteral("缩略图与图标缓存"),
        QStringLiteral("资源管理器生成的缩略图和图标数据库，删除后会自动重建。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Explorer")})},
        {QStringLiteral("thumbcache_*.db"), QStringLiteral("iconcache_*.db")}
    );
    add(
        QStringLiteral("wininet_cache"), QStringLiteral("缓存文件"), QStringLiteral("WinINet 网页缓存"),
        QStringLiteral("Internet Explorer 和部分系统组件使用的网页缓存，不包含浏览器账号数据。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("INetCache")})}
    );
    add(
        QStringLiteral("store_cache"), QStringLiteral("缓存文件"), QStringLiteral("Microsoft Store 缓存"),
        QStringLiteral("商店应用的 LocalCache 与 TempState，可由 Microsoft Store 重新生成。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Packages"), QStringLiteral("Microsoft.WindowsStore_8wekyb3d8bbwe"), QStringLiteral("LocalCache")}), winJoin({localAppData, QStringLiteral("Packages"), QStringLiteral("Microsoft.WindowsStore_8wekyb3d8bbwe"), QStringLiteral("TempState")})}
    );
    add(
        QStringLiteral("notification_cache"), QStringLiteral("缓存文件"), QStringLiteral("Windows 通知缓存"),
        QStringLiteral("通知中心保存的本地缓存，清理后旧通知记录可能消失。"),
        QStringLiteral("谨慎"), {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Notifications")}), winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("ActionCenterCache")})},
        {}, {}, false
    );
    add(
        QStringLiteral("recent_items"), QStringLiteral("缓存文件"), QStringLiteral("最近使用记录"),
        QStringLiteral("Windows 和 Office 的最近项目快捷方式，属于隐私痕迹，默认不选。"),
        QStringLiteral("隐私"), {winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Recent")}), winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Office"), QStringLiteral("Recent")})},
        {}, {}, false, false, true, 0, true
    );

    add(
        QStringLiteral("edge_cache"), QStringLiteral("应用程序"), QStringLiteral("Microsoft Edge 缓存"),
        QStringLiteral("Edge 各用户配置中的 Cache、Code Cache、GPUCache 和 Service Worker 缓存；建议先关闭浏览器。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data")})},
        {}, {QStringLiteral("\\cache\\"), QStringLiteral("\\code cache\\"), QStringLiteral("\\gpucache\\"), QStringLiteral("\\service worker\\cache")}
    );
    add(
        QStringLiteral("chrome_cache"), QStringLiteral("应用程序"), QStringLiteral("Google Chrome 缓存"),
        QStringLiteral("Chrome 各用户配置中的网页、代码和 GPU 缓存；建议先关闭浏览器。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data")})},
        {}, {QStringLiteral("\\cache\\"), QStringLiteral("\\code cache\\"), QStringLiteral("\\gpucache\\"), QStringLiteral("\\service worker\\cache")}
    );
    add(
        QStringLiteral("firefox_cache"), QStringLiteral("应用程序"), QStringLiteral("Mozilla Firefox 缓存"),
        QStringLiteral("Firefox 配置目录中的 cache2 网页缓存，不包含书签和账号配置。"),
        QStringLiteral("安全"), {winJoin({appData, QStringLiteral("Mozilla"), QStringLiteral("Firefox"), QStringLiteral("Profiles")})},
        {}, {QStringLiteral("\\cache2\\")}
    );
    add(
        QStringLiteral("qq_browser_cache"), QStringLiteral("应用程序"), QStringLiteral("QQ 浏览器缓存"),
        QStringLiteral("QQ 浏览器的网页、代码和 GPU 缓存，不扫描收藏夹与账号数据。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Tencent"), QStringLiteral("QQBrowser")}), winJoin({appData, QStringLiteral("Tencent"), QStringLiteral("QQBrowser")})},
        {}, {QStringLiteral("\\cache\\"), QStringLiteral("\\code cache\\"), QStringLiteral("\\gpucache\\")}
    );
    add(
        QStringLiteral("teams_cache"), QStringLiteral("应用程序"), QStringLiteral("Microsoft Teams 缓存"),
        QStringLiteral("Teams 本地 Cache 内容，清理前应退出 Teams。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Teams"), QStringLiteral("Cache")}), winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Teams"), QStringLiteral("Cache")})}
    );
    add(
        QStringLiteral("slack_cache"), QStringLiteral("应用程序"), QStringLiteral("Slack 缓存"),
        QStringLiteral("Slack 漫游配置中的 Cache 内容，不包含工作区登录配置。"),
        QStringLiteral("安全"), {winJoin({appData, QStringLiteral("Slack"), QStringLiteral("Cache")})}
    );
    add(
        QStringLiteral("discord_cache"), QStringLiteral("应用程序"), QStringLiteral("Discord 缓存"),
        QStringLiteral("Discord 漫游配置中的 Cache 内容，不包含账号令牌文件。"),
        QStringLiteral("安全"), {winJoin({appData, QStringLiteral("discord"), QStringLiteral("Cache")})}
    );
    add(
        QStringLiteral("media_cache"), QStringLiteral("应用程序"), QStringLiteral("媒体播放器缓存"),
        QStringLiteral("VLC 封面、Spotify Storage/cache 和 Windows Media Player 本地缓存。"),
        QStringLiteral("可选"), {winJoin({appData, QStringLiteral("vlc"), QStringLiteral("art")}), winJoin({localAppData, QStringLiteral("Spotify"), QStringLiteral("Storage")}), winJoin({appData, QStringLiteral("Spotify"), QStringLiteral("cache")})},
        {}, {}, false
    );
    add(
        QStringLiteral("onedrive_logs"), QStringLiteral("应用程序"), QStringLiteral("OneDrive 日志缓存"),
        QStringLiteral("OneDrive 同步客户端的日志文件，不删除同步数据。"),
        QStringLiteral("安全"), {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("OneDrive"), QStringLiteral("logs")}), winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("OneDrive"), QStringLiteral("settings"), QStringLiteral("Personal"), QStringLiteral("logs")})},
        {QStringLiteral("*.log"), QStringLiteral("*.etl"), QStringLiteral("*.old")}
    );
    add(
        QStringLiteral("app_logs"), QStringLiteral("应用程序"), QStringLiteral("30 天前的应用日志"),
        QStringLiteral("Teams、Slack、Discord 与 Office 的旧日志文件。"),
        QStringLiteral("安全"), {winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Teams"), QStringLiteral("logs")}), winJoin({appData, QStringLiteral("Slack"), QStringLiteral("logs")}), winJoin({appData, QStringLiteral("discord"), QStringLiteral("logs")}), winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Office")})},
        {QStringLiteral("*.log"), QStringLiteral("logs.txt")}, {}, true, false, true, 30
    );

    add(
        QStringLiteral("recycle_bin"), QStringLiteral("临时文件"), QStringLiteral("回收站"),
        QStringLiteral("清空 C 盘回收站。该操作无法通过本工具备份还原，默认不选。"),
        QStringLiteral("谨慎"), {winJoin({systemDrive, QStringLiteral("$Recycle.Bin")})}, {}, {}, false
    );
    add(
        QStringLiteral("download_residuals"), QStringLiteral("临时文件"), QStringLiteral("下载未完成残留"),
        QStringLiteral("下载目录中超过 7 天的 tmp、part、crdownload 和 download 临时文件。"),
        QStringLiteral("安全"), {winJoin({userProfile, QStringLiteral("Downloads")})},
        {QStringLiteral("*.tmp"), QStringLiteral("*.temp"), QStringLiteral("*.part"), QStringLiteral("*.crdownload"), QStringLiteral("*.download")},
        {}, true, false, true, 7
    );
    add(
        QStringLiteral("redundant_packages"), QStringLiteral("临时文件"), QStringLiteral("安装包、压缩包与镜像（仅分析）"),
        QStringLiteral("扫描下载、桌面和文档目录中的安装包、压缩包与 ISO。它们可能是用户文件，只逐项展示而不自动删除。"),
        QStringLiteral("高风险"), {winJoin({userProfile, QStringLiteral("Downloads")}), winJoin({userProfile, QStringLiteral("Desktop")}), winJoin({userProfile, QStringLiteral("Documents")})},
        {QStringLiteral("*.exe"), QStringLiteral("*.msi"), QStringLiteral("*.zip"), QStringLiteral("*.rar"), QStringLiteral("*.7z"), QStringLiteral("*.iso")},
        {}, false, true, false
    );
    return rules;
}

QString CleanupEngine::formatSize(qint64 bytes) {
    double value = static_cast<double>(bytes);
    const QStringList units = {QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"), QStringLiteral("TB")};
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < units.size() - 1) {
        value /= 1024.0;
        ++unitIndex;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unitIndex == 0 ? 0 : 2).arg(units.at(unitIndex));
}

bool CleanupEngine::deletePath(const QString& path, QString* error) {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;
    }
    const bool ok = info.isDir() && !info.isSymLink()
        ? QDir(path).removeRecursively()
        : QFile::remove(path);
    if (!ok && error) {
        *error = QStringLiteral("删除失败: %1").arg(path);
    }
    return ok;
}

QString CleanupEngine::backupRoot() {
    return defaultBackupRoot();
}

BackupInfo CleanupEngine::backupInfo(const QString& root) {
    BackupInfo info;
    info.backupRoot = root.isEmpty() ? defaultBackupRoot() : root;
    QFile file(manifestPath(info.backupRoot));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QStringList parts = stream.readLine().split(QLatin1Char('\t'));
            if (parts.size() < 5) {
                continue;
            }
            BackupRecord record;
            record.id = parts.at(0);
            record.createdAt = QDateTime::fromString(parts.at(1), Qt::ISODateWithMs);
            record.size = parts.at(2).toLongLong();
            record.sourcePath = parts.at(3);
            record.backupPath = parts.at(4);
            if (QFileInfo::exists(record.backupPath)) {
                info.totalBytes += record.size;
                info.backups.push_back(record);
            }
        }
    }
    std::sort(info.backups.begin(), info.backups.end(), [](const BackupRecord& a, const BackupRecord& b) {
        return a.createdAt > b.createdAt;
    });
    return info;
}

bool CleanupEngine::restoreBackupItem(const BackupRecord& record, QString* error) {
    if (!QFileInfo::exists(record.backupPath)) {
        if (error) {
            *error = QStringLiteral("备份文件不存在: %1").arg(record.backupPath);
        }
        return false;
    }
    if (QFileInfo::exists(record.sourcePath)) {
        if (error) {
            *error = QStringLiteral("原路径已存在当前文件，为避免覆盖已跳过: %1").arg(record.sourcePath);
        }
        return false;
    }
    if (!QDir().mkpath(QFileInfo(record.sourcePath).absolutePath())) {
        if (error) {
            *error = QStringLiteral("创建恢复目录失败: %1").arg(QFileInfo(record.sourcePath).absolutePath());
        }
        return false;
    }
    if (!QFile::copy(record.backupPath, record.sourcePath)) {
        if (error) {
            *error = QStringLiteral("恢复失败: %1 -> %2").arg(record.backupPath, record.sourcePath);
        }
        return false;
    }
    return true;
}

bool CleanupEngine::deleteBackupItem(const BackupRecord& record, QString* error) {
    const QFileInfo info(record.backupPath);
    if (!info.exists()) {
        return true;
    }
    if (!QFile::remove(record.backupPath)) {
        if (error) {
            *error = QStringLiteral("删除备份失败: %1").arg(record.backupPath);
        }
        return false;
    }
    QDir parent(info.absolutePath());
    if (parent.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()) {
        parent.removeRecursively();
    }
    return true;
}

bool CleanupEngine::pruneBackups(const QString& root, int maxBackups, qint64 maxBytes) {
    const BackupInfo info = backupInfo(root);
    bool ok = true;
    qint64 remainingBytes = info.totalBytes;
    int remainingCount = info.backups.size();
    for (int i = info.backups.size() - 1;
         i >= 0 && (remainingCount > qMax(0, maxBackups) || remainingBytes > qMax<qint64>(0, maxBytes));
         --i) {
        QString error;
        if (deleteBackupItem(info.backups.at(i), &error)) {
            remainingBytes -= info.backups.at(i).size;
            --remainingCount;
        } else {
            ok = false;
        }
    }
    return ok;
}

QString CleanupEngine::envPath(const QString& name, const QString& fallback) {
    const QByteArray key = name.toLocal8Bit();
    const QString value = qEnvironmentVariable(key.constData());
    return value.isEmpty() ? fallback : QDir::toNativeSeparators(value);
}

QString CleanupEngine::winJoin(std::initializer_list<QString> parts) {
    QStringList cleaned;
    for (const QString& part : parts) {
        if (!part.isEmpty()) {
            cleaned.push_back(part);
        }
    }
    QString joined = cleaned.join(QStringLiteral("\\"));
    joined.replace(QRegularExpression(QStringLiteral("\\\\+")), QStringLiteral("\\"));
    return joined;
}

bool CleanupEngine::pathMatches(const QString& path, const CleanupRule& rule) {
    if (!containsAnyMarker(path, rule.pathContains)) {
        return false;
    }
    if (rule.patterns.isEmpty()) {
        return true;
    }
    const QString fileName = QFileInfo(path).fileName();
    for (const QString& pattern : rule.patterns) {
        if (QDir::match(pattern, fileName)) {
            return true;
        }
    }
    return false;
}

qint64 CleanupEngine::fileSize(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        return 0;
    }
    if (info.isFile()) {
        return info.size();
    }
    qint64 total = 0;
    QDirIterator iterator(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
    }
    return total;
}

QString CleanupEngine::defaultBackupRoot() {
#ifdef Q_OS_WIN
    const QString systemDrive = QDir::cleanPath(qEnvironmentVariable("SystemDrive", "C:") + QLatin1Char('/')).toUpper();
    for (QStorageInfo storage : QStorageInfo::mountedVolumes()) {
        storage.refresh();
        const QString root = QDir::cleanPath(storage.rootPath()).toUpper();
        if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()
            || root == systemDrive || storage.bytesAvailable() < 256LL * 1024LL * 1024LL) {
            continue;
        }
        const QString nativeRoot = QDir::toNativeSeparators(storage.rootPath());
        if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(nativeRoot.utf16())) != DRIVE_FIXED) {
            continue;
        }
        const QString candidate = QDir(storage.rootPath()).filePath(QStringLiteral("C_DiskGlow_Backups"));
        if (QDir().mkpath(candidate)) {
            return candidate;
        }
    }
#endif
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base.isEmpty() ? QDir::homePath() : base).filePath(QStringLiteral("backups"));
}

void CleanupEngine::collectRuleFiles(
    const CleanupRule& rule,
    const QString& rootPath,
    QStringList* files,
    qint64* totalSize,
    const ProgressCallback& progress,
    int* count
) {
    const QFileInfo root(rootPath);
    if (!root.exists()) {
        return;
    }
    if (root.isFile()) {
        if (pathMatches(rootPath, rule) && oldEnough(root, rule.minimumAgeDays)) {
            files->push_back(rootPath);
            *totalSize += root.size();
            ++(*count);
        }
        return;
    }
    QDirIterator iterator(
        rootPath,
        safeGlobPatterns(rule),
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
        QDirIterator::Subdirectories
    );
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!pathMatches(path, rule) || !oldEnough(iterator.fileInfo(), rule.minimumAgeDays)) {
            continue;
        }
        files->push_back(path);
        *totalSize += iterator.fileInfo().size();
        ++(*count);
        if (progress && *count % 100 == 0) {
            progress(path, *count);
        }
    }
}

bool CleanupEngine::emptyRecycleBin(QString* error) {
#ifdef Q_OS_WIN
    const HRESULT result = SHEmptyRecycleBinW(
        nullptr,
        L"C:\\",
        SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND
    );
    if (SUCCEEDED(result)) {
        return true;
    }
    if (error) {
        *error = QStringLiteral("清空回收站失败，错误码: 0x%1").arg(static_cast<qulonglong>(result), 0, 16);
    }
    return false;
#else
    Q_UNUSED(error);
    return true;
#endif
}

bool CleanupEngine::backupFile(const QString& source, const QString& backupRoot, QString* error) {
    const QFileInfo info(source);
    if (!info.exists() || !info.isFile()) {
        return true;
    }
    if (!QDir().mkpath(backupRoot)) {
        if (error) {
            *error = QStringLiteral("创建备份目录失败: %1").arg(backupRoot);
        }
        return false;
    }
    const QString destination = uniqueBackupPath(backupRoot, source);
    if (!QFile::copy(source, destination)) {
        if (error) {
            *error = QStringLiteral("备份失败，已跳过删除: %1").arg(source);
        }
        return false;
    }
    appendBackupManifest(backupRoot, info.absoluteFilePath(), destination, info.size());
    return true;
}
