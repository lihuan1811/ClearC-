#include "CleanupEngine.h"
#include "DismRuleScanner.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

namespace {

QStringList safeGlobPatterns(const CleanupRule& rule) {
    if (!rule.patterns.isEmpty()) {
        return rule.patterns;
    }
    return QStringList{QStringLiteral("*")};
}

bool containsAnyMarker(const QString& normalizedPath, const QStringList& markers) {
    if (markers.isEmpty()) {
        return true;
    }

    const QString lowered = normalizedPath.toLower();
    for (const QString& marker : markers) {
        if (lowered.contains(marker.toLower())) {
            return true;
        }
    }
    return false;
}

QString uniqueBackupPath(const QString& backupRoot, const QString& source) {
    const QFileInfo info(source);
    const QString normalizedPath = QDir::toNativeSeparators(info.absoluteFilePath()).toLower();
    const QString pathDigest = QString::fromLatin1(
        QCryptographicHash::hash(normalizedPath.toUtf8(), QCryptographicHash::Sha256).toHex().left(16)
    );
    const QString destinationDir = QDir(backupRoot).filePath(pathDigest);
    QDir().mkpath(destinationDir);

    const QString baseName = info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName();
    const QString suffix = info.suffix();
    QString candidate = QDir(destinationDir).filePath(info.fileName());
    int counter = 1;
    while (QFileInfo::exists(candidate)) {
        const QString numberedName = suffix.isEmpty()
            ? QStringLiteral("%1-%2").arg(baseName).arg(counter)
            : QStringLiteral("%1-%2.%3").arg(baseName).arg(counter).arg(suffix);
        candidate = QDir(destinationDir).filePath(numberedName);
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

bool CleanupEngine::ruleMatchesScanScope(const CleanupRule& rule, ScanScope scope) {
    if (scope == ScanScope::All) {
        return true;
    }
    const bool qqRule = rule.id.startsWith(QStringLiteral("qq_"));
    const bool wechatRule = rule.id.startsWith(QStringLiteral("wechat_"));
    if (scope == ScanScope::QQ) {
        return qqRule;
    }
    if (scope == ScanScope::WeChat) {
        return wechatRule;
    }
    return !qqRule && !wechatRule;
}

CleanupScanResult CleanupEngine::scanSystem(const ProgressCallback& progress, ScanScope scope) {
    CleanupScanResult result;
    int count = 0;

    for (const CleanupRule& rule : cleanupRules()) {
        if (!ruleMatchesScanScope(rule, scope)) {
            continue;
        }
        for (const QString& rootPath : rule.paths) {
            if (progress) {
                progress(rootPath, count);
            }

            QStringList files;
            qint64 totalSize = 0;
            collectRuleFiles(rule, rootPath, &files, &totalSize, progress, &count);
            if (files.isEmpty() && totalSize <= 0) {
                continue;
            }

            CleanupEntry entry;
            entry.ruleId = rule.id;
            entry.title = rule.title;
            entry.path = rootPath;
            entry.files = files;
            entry.size = totalSize;
            entry.scanOnly = rule.scanOnly;
            entry.recommended = rule.recommended;
            entry.professional = rule.professional;
            entry.privacySensitive = rule.privacySensitive;
            result.entries.push_back(entry);
            result.totalBytes += totalSize;
            if (!rule.scanOnly && rule.recommended) {
                result.recommendedBytes += totalSize;
            }
            if (rule.professional) {
                result.professionalBytes += totalSize;
            }
        }
    }

    if (scope == ScanScope::All || scope == ScanScope::CDrive) {
        for (const DismRuleEntry& dismEntry : DismRuleScanner().scan()) {
            CleanupEntry entry;
            entry.ruleId = QStringLiteral("dismpp_rules");
            entry.title = QStringLiteral("Dism++规则 - %1").arg(dismEntry.ruleName);
            entry.path = dismEntry.path;
            entry.files = {dismEntry.path};
            entry.size = dismEntry.size;
            entry.scanOnly = false;
            entry.recommended = false;
            entry.professional = true;
            result.entries.push_back(entry);
            result.totalBytes += entry.size;
            result.professionalBytes += entry.size;
            ++count;
        }
    }

    result.scannedCount = count;
    return result;
}

QVector<CleanupEntry> CleanupEngine::entriesForMode(
    const QVector<CleanupEntry>& entries,
    CleanMode mode
) const {
    QVector<CleanupEntry> selected;
    for (const CleanupEntry& entry : entries) {
        if (mode == CleanMode::Recommended) {
            if (!entry.scanOnly && entry.recommended) {
                selected.push_back(entry);
            }
            continue;
        }
        if (mode == CleanMode::Professional) {
            if (entry.professional) {
                selected.push_back(entry);
            }
            continue;
        }
        if (mode == CleanMode::SelectAll) {
            if (!entry.privacySensitive) {
                selected.push_back(entry);
            }
        }
    }
    return selected;
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
    const QString backupRoot = options.backupRoot.isEmpty() ? defaultBackupRoot() : options.backupRoot;

    for (const CleanupEntry& entry : entries) {
        if (entry.scanOnly && !options.allowScanOnly) {
            result.skippedCount += qMax(1, entry.files.size());
            continue;
        }

        if (entry.ruleId == QStringLiteral("recycle")) {
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

        if (entry.files.isEmpty()) {
            ++result.skippedCount;
            continue;
        }

        const QStringList targets = entry.files;
        for (const QString& path : targets) {
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
                if (!backupFile(path, backupRoot, &backupError)) {
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
    const QString localLow = winJoin({userProfile, QStringLiteral("AppData"), QStringLiteral("LocalLow")});
    const QString programData = envPath(QStringLiteral("ProgramData"), QStringLiteral("C:\\ProgramData"));
    const QString programFiles = envPath(QStringLiteral("ProgramFiles"), QStringLiteral("C:\\Program Files"));
    const QString programFilesX86 = envPath(QStringLiteral("ProgramFiles(x86)"), QStringLiteral("C:\\Program Files (x86)"));
    const QString systemRoot = envPath(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    const QString systemDrive = envPath(QStringLiteral("SystemDrive"), QStringLiteral("C:"));
    const QString networkServiceLocal = winJoin({systemRoot, QStringLiteral("ServiceProfiles"), QStringLiteral("NetworkService"), QStringLiteral("AppData"), QStringLiteral("Local")});
    const QString localServiceLocal = winJoin({systemRoot, QStringLiteral("ServiceProfiles"), QStringLiteral("LocalService"), QStringLiteral("AppData"), QStringLiteral("Local")});
    const QString systemProfileLocal = winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("config"), QStringLiteral("systemprofile"), QStringLiteral("AppData"), QStringLiteral("Local")});

    QVector<CleanupRule> rules;
    auto add = [&rules](
        const QString& id,
        const QString& title,
        const QStringList& paths,
        bool scanOnly,
        const QStringList& patterns = {},
        const QStringList& pathContains = {},
        bool recommended = true,
        bool professional = true,
        bool privacySensitive = false
    ) {
        CleanupRule rule;
        rule.id = id;
        rule.title = title;
        rule.paths = paths;
        rule.patterns = patterns;
        rule.pathContains = pathContains;
        rule.scanOnly = scanOnly;
        rule.recommended = recommended;
        rule.professional = professional;
        rule.privacySensitive = privacySensitive;
        rules.push_back(rule);
    };

    add(QStringLiteral("temp_files"), QStringLiteral("系统临时文件"),
        {envPath(QStringLiteral("TEMP"), winJoin({localAppData, QStringLiteral("Temp")})), winJoin({systemRoot, QStringLiteral("Temp")})},
        false, {QStringLiteral("*.tmp"), QStringLiteral("*.temp"), QStringLiteral("*.log"), QStringLiteral("*")});
    add(QStringLiteral("recycle"), QStringLiteral("回收站"),
        {winJoin({systemDrive, QStringLiteral("$Recycle.Bin")})}, false);
    add(QStringLiteral("chrome_cache"), QStringLiteral("Chrome 缓存"),
        {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Cache")}),
         winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Code Cache")}),
         winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("GPUCache")})},
        false);
    add(QStringLiteral("edge_cache"), QStringLiteral("Edge 缓存"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Cache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Code Cache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("GPUCache")})},
        false);
    add(QStringLiteral("firefox_cache"), QStringLiteral("Firefox 缓存"),
        {winJoin({appData, QStringLiteral("Mozilla"), QStringLiteral("Firefox"), QStringLiteral("Profiles")})},
        false, {}, {QStringLiteral("\\cache2\\entries\\")});
    add(QStringLiteral("browser_cookies"), QStringLiteral("浏览器 Cookie"),
        {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Network"), QStringLiteral("Cookies")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Network"), QStringLiteral("Cookies")}),
         winJoin({appData, QStringLiteral("Mozilla"), QStringLiteral("Firefox"), QStringLiteral("Profiles")})},
        true, {QStringLiteral("Cookies"), QStringLiteral("cookies.sqlite")}, {}, false, true, true);
    add(QStringLiteral("browser_history"), QStringLiteral("浏览器历史记录"),
        {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("History")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("History")}),
         winJoin({appData, QStringLiteral("Mozilla"), QStringLiteral("Firefox"), QStringLiteral("Profiles")})},
        true, {QStringLiteral("History"), QStringLiteral("places.sqlite")}, {}, false, true, true);
    add(QStringLiteral("browser_passwords"), QStringLiteral("浏览器保存密码"),
        {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Login Data")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Login Data")}),
         winJoin({appData, QStringLiteral("Mozilla"), QStringLiteral("Firefox"), QStringLiteral("Profiles")})},
        true, {QStringLiteral("Login Data"), QStringLiteral("logins.json"), QStringLiteral("key4.db")}, {}, false, true, true);
    add(QStringLiteral("browser_extensions"), QStringLiteral("浏览器插件与扩展数据"),
        {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Extensions")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Extensions")}),
         winJoin({appData, QStringLiteral("Mozilla"), QStringLiteral("Firefox"), QStringLiteral("Profiles")})},
        true, {QStringLiteral("extensions.json"), QStringLiteral("addons.json"), QStringLiteral("*.xpi")}, {}, false, true, true);
    add(QStringLiteral("chrome_update_cache"), QStringLiteral("Chrome 更新缓存"),
        {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Update")})}, false);
    add(QStringLiteral("edge_update_cache"), QStringLiteral("Edge 更新缓存"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("EdgeUpdate")})}, false);
    add(QStringLiteral("prefetch"), QStringLiteral("Windows 预读取文件"),
        {winJoin({systemRoot, QStringLiteral("Prefetch")})}, false, {QStringLiteral("*.pf")});
    add(QStringLiteral("system_logs"), QStringLiteral("系统日志"),
        {winJoin({systemRoot, QStringLiteral("Logs")}), winJoin({systemRoot, QStringLiteral("debug")})},
        false, {QStringLiteral("*.log"), QStringLiteral("*.etl"), QStringLiteral("*.dmp")});
    add(QStringLiteral("thumbnails"), QStringLiteral("缩略图缓存"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Explorer")})},
        false, {QStringLiteral("thumbcache_*.db"), QStringLiteral("iconcache_*.db")});
    add(QStringLiteral("old_windows"), QStringLiteral("旧 Windows 文件"),
        {winJoin({systemDrive, QStringLiteral("Windows.old")}), winJoin({systemDrive, QStringLiteral("$Windows.~BT")}), winJoin({systemDrive, QStringLiteral("$Windows.~WS")})},
        true);
    add(QStringLiteral("error_reports"), QStringLiteral("Windows 错误报告"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER")})},
        false);
    add(QStringLiteral("service_packs"), QStringLiteral("服务包备份"),
        {winJoin({systemRoot, QStringLiteral("$NtServicePackUninstall$")}), winJoin({systemRoot, QStringLiteral("$hf_mig$")})},
        true);
    add(QStringLiteral("memory_dumps"), QStringLiteral("内存转储文件"),
        {winJoin({systemRoot, QStringLiteral("Minidump")}), winJoin({systemRoot, QStringLiteral("MEMORY.DMP")}), winJoin({systemRoot, QStringLiteral("memory.dmp")})},
        false);
    add(QStringLiteral("font_cache"), QStringLiteral("字体缓存"),
        {winJoin({localServiceLocal, QStringLiteral("FontCache")}), winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("FNTCACHE.DAT")})},
        false);
    add(QStringLiteral("disk_cleanup"), QStringLiteral("磁盘清理备份"),
        {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("LogFiles"), QStringLiteral("setupapi")}),
         winJoin({systemRoot, QStringLiteral("Temp"), QStringLiteral("CheckSur")}),
         winJoin({systemRoot, QStringLiteral("Logs"), QStringLiteral("CBS")})},
        false);
    add(QStringLiteral("windows_update_download"), QStringLiteral("Windows 更新下载缓存"),
        {winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("Download")}),
         winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("DataStore")})},
        false);
    add(QStringLiteral("update_temp"), QStringLiteral("Windows 更新临时文件"),
        {winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("PostRebootEventCache")}),
         winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("Temp")}),
         winJoin({systemRoot, QStringLiteral("WinSxS"), QStringLiteral("Temp")}),
         winJoin({systemRoot, QStringLiteral("Temp"), QStringLiteral("TrustedInstaller")})},
        false);
    add(QStringLiteral("delivery_opt"), QStringLiteral("Windows 传递优化缓存"),
        {winJoin({networkServiceLocal, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("DeliveryOptimization"), QStringLiteral("Cache")}),
         winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("DeliveryOptimization"), QStringLiteral("Cache")})},
        false);
    add(QStringLiteral("downloads"), QStringLiteral("下载文件夹"),
        {winJoin({userProfile, QStringLiteral("Downloads")})}, true);
    add(QStringLiteral("installer_cache"), QStringLiteral("安装程序缓存"),
        {winJoin({systemRoot, QStringLiteral("Installer"), QStringLiteral("Temp")}),
         winJoin({programData, QStringLiteral("Package Cache"), QStringLiteral("Temp")}),
         winJoin({systemRoot, QStringLiteral("Downloaded Program Files"), QStringLiteral("Temp")}),
         winJoin({localAppData, QStringLiteral("Package Cache")}),
         winJoin({localAppData, QStringLiteral("Temp"), QStringLiteral("Downloaded Installations")}),
         winJoin({systemRoot, QStringLiteral("Installer")})},
        false, {QStringLiteral("*.tmp"), QStringLiteral("*.temp"), QStringLiteral("*.msi.cache"), QStringLiteral("*.msp.cache"), QStringLiteral("*.exe.cache"), QStringLiteral("*.log"), QStringLiteral("*.old")});
    add(QStringLiteral("app_cache"), QStringLiteral("应用程序缓存"),
        {winJoin({appData, QStringLiteral("Adobe"), QStringLiteral("Common")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Office"), QStringLiteral("Recent")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Office"), QStringLiteral("OTele")}),
         winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("DriveFS")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Teams"), QStringLiteral("Cache")}),
         winJoin({appData, QStringLiteral("Slack"), QStringLiteral("Cache")}),
         winJoin({appData, QStringLiteral("discord"), QStringLiteral("Cache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("INetCache"), QStringLiteral("IE")})},
        false);
    add(QStringLiteral("media_cache"), QStringLiteral("媒体播放器缓存"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Media Player")}),
         winJoin({appData, QStringLiteral("vlc"), QStringLiteral("art")}),
         winJoin({localAppData, QStringLiteral("Spotify"), QStringLiteral("Storage")}),
         winJoin({appData, QStringLiteral("Spotify"), QStringLiteral("cache")})},
        false);
    add(QStringLiteral("backup_temp"), QStringLiteral("备份临时文件"),
        {winJoin({systemRoot, QStringLiteral("Temp"), QStringLiteral("WindowsBackup")}),
         winJoin({systemRoot, QStringLiteral("Logs"), QStringLiteral("WindowsBackup")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WindowsBackup")})},
        false);
    add(QStringLiteral("driver_backup"), QStringLiteral("驱动备份"),
        {winJoin({systemRoot, QStringLiteral("inf"), QStringLiteral("OLD")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("DriverStore"), QStringLiteral("Temp")})},
        false);
    add(QStringLiteral("app_crash"), QStringLiteral("应用程序崩溃转储"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER"), QStringLiteral("ReportArchive")}),
         winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER"), QStringLiteral("ReportQueue")}),
         winJoin({localAppData, QStringLiteral("CrashDumps")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER"), QStringLiteral("ReportArchive")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WER"), QStringLiteral("ReportQueue")})},
        false);
    add(QStringLiteral("app_logs"), QStringLiteral("应用程序日志"),
        {winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Teams"), QStringLiteral("logs.txt")}),
         winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Teams"), QStringLiteral("logs")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Office")}),
         winJoin({appData, QStringLiteral("Slack"), QStringLiteral("logs")}),
         winJoin({appData, QStringLiteral("discord"), QStringLiteral("logs")})},
        false, {QStringLiteral("*.log"), QStringLiteral("logs.txt")});
    add(QStringLiteral("recent_items"), QStringLiteral("最近使用记录"),
        {winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Recent")}),
         winJoin({appData, QStringLiteral("Microsoft"), QStringLiteral("Office"), QStringLiteral("Recent")})},
        true, {QStringLiteral("*.lnk"), QStringLiteral("*.automaticDestinations-ms"), QStringLiteral("*.customDestinations-ms")}, {}, false, true, true);
    add(QStringLiteral("notification"), QStringLiteral("Windows 通知缓存"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Notifications")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("ActionCenterCache")})},
        false);
    add(QStringLiteral("dns_cache"), QStringLiteral("DNS 缓存文件"),
        {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("dnsrslvr.log")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("dns"), QStringLiteral("cache.dns")})},
        true);
    add(QStringLiteral("printer_temp"), QStringLiteral("打印机临时文件"),
        {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("spool"), QStringLiteral("PRINTERS")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("spool"), QStringLiteral("SERVERS")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("spool"), QStringLiteral("drivers"), QStringLiteral("color")})},
        false);
    add(QStringLiteral("device_temp"), QStringLiteral("设备安装临时文件"),
        {winJoin({systemRoot, QStringLiteral("INF"), QStringLiteral("setupapi.dev.log")}),
         winJoin({systemRoot, QStringLiteral("INF"), QStringLiteral("setupapi.log")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("LogFiles"), QStringLiteral("setupapi")})},
        false);
    add(QStringLiteral("store_cache"), QStringLiteral("Windows Store 缓存"),
        {winJoin({localAppData, QStringLiteral("Packages"), QStringLiteral("Microsoft.WindowsStore_8wekyb3d8bbwe"), QStringLiteral("LocalCache")}),
         winJoin({localAppData, QStringLiteral("Packages"), QStringLiteral("Microsoft.WindowsStore_8wekyb3d8bbwe"), QStringLiteral("LocalState")}),
         winJoin({localAppData, QStringLiteral("Packages"), QStringLiteral("Microsoft.WindowsStore_8wekyb3d8bbwe"), QStringLiteral("TempState")})},
        false);
    add(QStringLiteral("onedrive_cache"), QStringLiteral("OneDrive 缓存"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("OneDrive"), QStringLiteral("logs")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("OneDrive"), QStringLiteral("settings"), QStringLiteral("Personal"), QStringLiteral("logs")})},
        false);
    add(QStringLiteral("edge_webview_cache"), QStringLiteral("Edge/WebView 缓存"),
        {winJoin({localAppData, QStringLiteral("GameViewer"), QStringLiteral("webviewcache"), QStringLiteral("EBWebView")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("DawnGraphiteCache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("DawnWebGPUCache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("GrShaderCache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("ShaderCache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Service Worker")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Shared Dictionary")})},
        true);
    add(QStringLiteral("edge_profile_state"), QStringLiteral("Edge 用户状态"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("BrowserMetrics")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Crashpad")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("History")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Login Data")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Network")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Preferences")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Secure Preferences")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Sessions")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Session Storage")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Shortcuts")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Sync Data")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Visited Links")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Default"), QStringLiteral("Web Data")})},
        true, {}, {}, false, true, true);
    add(QStringLiteral("edge_component_updates"), QStringLiteral("Edge 组件更新残留"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("EADPData Component")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data"), QStringLiteral("Typosquatting")})},
        true);
    add(QStringLiteral("edgecore_old_versions"), QStringLiteral("EdgeCore 旧版本"),
        {winJoin({programFilesX86, QStringLiteral("Microsoft"), QStringLiteral("EdgeCore")})}, true);
    add(QStringLiteral("panther_setup_logs"), QStringLiteral("Panther 安装日志"),
        {winJoin({systemRoot, QStringLiteral("Panther")})}, true,
        {QStringLiteral("*.dir"), QStringLiteral("*.etl"), QStringLiteral("*.log"), QStringLiteral("*.que"), QStringLiteral("*.uaq"), QStringLiteral("*.xml"), QStringLiteral("setupinfo")});
    add(QStringLiteral("service_profile_temp"), QStringLiteral("系统服务临时文件"),
        {winJoin({systemRoot, QStringLiteral("SystemTemp")}), winJoin({networkServiceLocal, QStringLiteral("Temp")}), winJoin({localServiceLocal, QStringLiteral("Temp")})},
        true);
    add(QStringLiteral("drvpath_driver_packages"), QStringLiteral("DrvPath 驱动残留"),
        {winJoin({systemDrive, QStringLiteral("DrvPath")})}, true,
        {QStringLiteral("*.7z"), QStringLiteral("*.zip"), QStringLiteral("*.rar"), QStringLiteral("*.cab"), QStringLiteral("*.log")});
    add(QStringLiteral("intel_logs"), QStringLiteral("Intel 日志"),
        {winJoin({systemDrive, QStringLiteral("Intel"), QStringLiteral("Logs")})}, true, {QStringLiteral("*.log")});
    add(QStringLiteral("explorer_runtime_cache"), QStringLiteral("Explorer 运行缓存"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Caches")}),
         winJoin({localAppData, QStringLiteral("IconCache.db")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Caches")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Explorer")}),
         winJoin({localServiceLocal, QStringLiteral("FontCache")})},
        true);
    add(QStringLiteral("legacy_ie_cache"), QStringLiteral("IE/系统 Web 缓存"),
        {winJoin({localLow, QStringLiteral("Microsoft"), QStringLiteral("CryptnetUrlCache"), QStringLiteral("Content")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Internet Explorer"), QStringLiteral("DOMStore")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Internet Explorer"), QStringLiteral("CacheStorage")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("AppCache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("INetCache")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("INetCookies")}),
         winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WebCache")}),
         winJoin({systemProfileLocal, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("INetCache")}),
         winJoin({systemProfileLocal, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("WebCache")}),
         winJoin({systemProfileLocal, QStringLiteral("Low"), QStringLiteral("Microsoft"), QStringLiteral("CryptnetUrlCache")})},
        true);
    add(QStringLiteral("system_event_logs"), QStringLiteral("系统事件日志"),
        {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("winevt"), QStringLiteral("Logs")})},
        true, {QStringLiteral("*.evtx")}, {}, false, true);
    add(QStringLiteral("appx_package_cache"), QStringLiteral("AppData Packages 缓存"),
        {winJoin({localAppData, QStringLiteral("Packages")})}, true, {}, {
            QStringLiteral("\\microsoft.microsoftofficehub_"),
            QStringLiteral("\\microsoft.microsoftpcmanager_"),
            QStringLiteral("\\microsoft.windows.search_"),
            QStringLiteral("\\microsoft.windowsstore_"),
            QStringLiteral("\\microsoft.windows.client.cbs_"),
            QStringLiteral("\\microsoft.windowscommunicationsapps_"),
            QStringLiteral("\\microsoft.windows.contentdeliverymanager_"),
            QStringLiteral("\\microsoft.windows.photos_"),
            QStringLiteral("\\microsoft.skypeapp_"),
        });
    add(QStringLiteral("search_index"), QStringLiteral("搜索索引临时文件"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Search"), QStringLiteral("Data"), QStringLiteral("Temp")}),
         winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Search"), QStringLiteral("Data"), QStringLiteral("Applications"), QStringLiteral("Windows")}),
         winJoin({localServiceLocal, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Search")})},
        false, {QStringLiteral("*.tmp"), QStringLiteral("*.old"), QStringLiteral("*.bak"), QStringLiteral("*.log")});
    add(QStringLiteral("third_party_app_logs"), QStringLiteral("第三方程序日志"),
        {winJoin({programData, QStringLiteral("NVIDIA Corporation")}),
         winJoin({programData, QStringLiteral("NVIDIA")}),
         winJoin({programData, QStringLiteral("Windows Master Store")}),
         winJoin({localAppData, QStringLiteral("GameViewer"), QStringLiteral("webviewcache"), QStringLiteral("EBWebView")})},
        true, {QStringLiteral("*.log")});
    add(QStringLiteral("windows_extra_logs"), QStringLiteral("Windows 额外日志"),
        {winJoin({systemRoot, QStringLiteral("DtcInstall.log")}),
         winJoin({systemRoot, QStringLiteral("PFRO.log")}),
         winJoin({systemRoot, QStringLiteral("setupact.log")}),
         winJoin({systemRoot, QStringLiteral("setuperr.log")}),
         winJoin({systemRoot, QStringLiteral("Microsoft.NET"), QStringLiteral("Framework64"), QStringLiteral("v4.0.30319"), QStringLiteral("ngen.log")}),
         winJoin({systemRoot, QStringLiteral("Microsoft.NET"), QStringLiteral("Framework"), QStringLiteral("v4.0.30319"), QStringLiteral("ngen.log")}),
         winJoin({systemRoot, QStringLiteral("Performance"), QStringLiteral("WinSAT")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("MsDtc"), QStringLiteral("MSDTC.LOG")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("config"), QStringLiteral("BCD-Template.LOG")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("sru")})},
        true, {QStringLiteral("*.log"), QStringLiteral("*.LOG")});
    add(QStringLiteral("sleepstudy_wdi_traces"), QStringLiteral("SleepStudy/WDI 跟踪"),
        {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("SleepStudy")}),
         winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("WDI"), QStringLiteral("LogFiles")})},
        true, {QStringLiteral("*.etl")});
    add(QStringLiteral("windowsapps_cleanup_candidates"), QStringLiteral("WindowsApps 精简候选"),
        {winJoin({programFiles, QStringLiteral("WindowsApps")})}, true, {}, {
            QStringLiteral("\\deleted\\"),
            QStringLiteral("\\deletedalluserpackages\\"),
            QStringLiteral("\\microsoft.gethelp_"),
        });
    add(QStringLiteral("windows_update_lcu_backup"), QStringLiteral("LCU 更新备份"),
        {winJoin({systemRoot, QStringLiteral("servicing"), QStringLiteral("LCU")})}, true);
    add(QStringLiteral("windows_update_signature_cache"), QStringLiteral("catroot2 签名缓存"),
        {winJoin({systemRoot, QStringLiteral("System32"), QStringLiteral("catroot2")})}, true);
    add(QStringLiteral("windows_search_index_cache"), QStringLiteral("Windows Search 索引缓存"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Search"), QStringLiteral("Data"), QStringLiteral("Applications"), QStringLiteral("Windows")})}, true);
    add(QStringLiteral("defender_definition_backup"), QStringLiteral("Windows Defender 定义备份"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows Defender"), QStringLiteral("Definition Updates"), QStringLiteral("Backup")})}, true);
    add(QStringLiteral("defender_support_logs"), QStringLiteral("Windows Defender 支持日志"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows Defender"), QStringLiteral("Support")})}, true);
    add(QStringLiteral("defender_history"), QStringLiteral("Windows Defender 历史记录"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows Defender"), QStringLiteral("Scans"), QStringLiteral("History")})}, true);
    add(QStringLiteral("defender_quarantine"), QStringLiteral("Windows Defender 隔离区"),
        {winJoin({programData, QStringLiteral("Microsoft"), QStringLiteral("Windows Defender"), QStringLiteral("Quarantine")})}, true);
    add(QStringLiteral("winsxs_backup"), QStringLiteral("WinSxS Backup"),
        {winJoin({systemRoot, QStringLiteral("WinSxS"), QStringLiteral("Backup")})}, true);
    add(QStringLiteral("winsxs_catalogs"), QStringLiteral("WinSxS Catalogs"),
        {winJoin({systemRoot, QStringLiteral("WinSxS"), QStringLiteral("Catalogs")})}, true);
    add(QStringLiteral("winsxs_onedrive_setup"), QStringLiteral("WinSxS OneDrive 组件"),
        {winJoin({systemRoot, QStringLiteral("WinSxS")})}, true, {}, {QStringLiteral("onedrive-setup")});
    add(QStringLiteral("winsxs_component_store"), QStringLiteral("WinSxS 组件存储"),
        {winJoin({systemRoot, QStringLiteral("WinSxS")})}, true, {}, {
            QStringLiteral("\\amd64_"),
            QStringLiteral("\\wow64_"),
            QStringLiteral("\\x86_"),
        });
    add(QStringLiteral("vscode_cache"), QStringLiteral("VS Code 缓存"),
        {winJoin({appData, QStringLiteral("Code"), QStringLiteral("Cache")}),
         winJoin({appData, QStringLiteral("Code"), QStringLiteral("CachedData")}),
         winJoin({appData, QStringLiteral("Code"), QStringLiteral("GPUCache")}),
         winJoin({appData, QStringLiteral("Code"), QStringLiteral("Service Worker"), QStringLiteral("CacheStorage")}),
         winJoin({appData, QStringLiteral("Code"), QStringLiteral("logs")})},
        false);
    add(QStringLiteral("cursor_cache"), QStringLiteral("Cursor 缓存"),
        {winJoin({appData, QStringLiteral("Cursor"), QStringLiteral("Cache")}),
         winJoin({appData, QStringLiteral("Cursor"), QStringLiteral("CachedData")}),
         winJoin({appData, QStringLiteral("Cursor"), QStringLiteral("GPUCache")}),
         winJoin({appData, QStringLiteral("Cursor"), QStringLiteral("Service Worker"), QStringLiteral("CacheStorage")}),
         winJoin({appData, QStringLiteral("Cursor"), QStringLiteral("logs")})},
        false);
    add(QStringLiteral("discord_cache"), QStringLiteral("Discord 缓存"),
        {winJoin({appData, QStringLiteral("discord"), QStringLiteral("Cache")}),
         winJoin({appData, QStringLiteral("discord"), QStringLiteral("Code Cache")}),
         winJoin({appData, QStringLiteral("discord"), QStringLiteral("GPUCache")}),
         winJoin({appData, QStringLiteral("discord"), QStringLiteral("Service Worker"), QStringLiteral("CacheStorage")})},
        false);
    add(QStringLiteral("steam_web_cache"), QStringLiteral("Steam 网页缓存"),
        {winJoin({localAppData, QStringLiteral("Steam"), QStringLiteral("htmlcache"), QStringLiteral("Cache")}),
         winJoin({localAppData, QStringLiteral("Steam"), QStringLiteral("htmlcache"), QStringLiteral("Code Cache")}),
         winJoin({localAppData, QStringLiteral("Steam"), QStringLiteral("htmlcache"), QStringLiteral("GPUCache")})},
        false);
    add(QStringLiteral("slack_cache"), QStringLiteral("Slack 缓存"),
        {winJoin({appData, QStringLiteral("Slack"), QStringLiteral("Cache")}),
         winJoin({appData, QStringLiteral("Slack"), QStringLiteral("Code Cache")}),
         winJoin({appData, QStringLiteral("Slack"), QStringLiteral("GPUCache")}),
         winJoin({appData, QStringLiteral("Slack"), QStringLiteral("Service Worker"), QStringLiteral("CacheStorage")}),
         winJoin({appData, QStringLiteral("Slack"), QStringLiteral("logs")})},
        false);
    add(QStringLiteral("notion_cache"), QStringLiteral("Notion 缓存"),
        {winJoin({appData, QStringLiteral("Notion"), QStringLiteral("Cache")}),
         winJoin({appData, QStringLiteral("Notion"), QStringLiteral("Code Cache")}),
         winJoin({appData, QStringLiteral("Notion"), QStringLiteral("GPUCache")}),
         winJoin({appData, QStringLiteral("Notion"), QStringLiteral("Service Worker"), QStringLiteral("CacheStorage")})},
        false);
    add(QStringLiteral("obs_cache"), QStringLiteral("OBS Studio 缓存"),
        {winJoin({appData, QStringLiteral("obs-studio"), QStringLiteral("logs")}),
         winJoin({appData, QStringLiteral("obs-studio"), QStringLiteral("crashes")}),
         winJoin({appData, QStringLiteral("obs-studio"), QStringLiteral("plugin_config"), QStringLiteral("obs-browser"), QStringLiteral("Cache")})},
        false);
    add(QStringLiteral("c_drive_installers_archives"), QStringLiteral("C盘大型安装包/压缩包/镜像"),
        {winJoin({userProfile, QStringLiteral("Downloads")}),
         winJoin({userProfile, QStringLiteral("Desktop")}),
         systemDrive + QStringLiteral("\\")},
        true, {QStringLiteral("*.exe"), QStringLiteral("*.msi"), QStringLiteral("*.zip"), QStringLiteral("*.rar"), QStringLiteral("*.7z"), QStringLiteral("*.iso")}, {}, false, true);
    add(QStringLiteral("wechat_special_clean"), QStringLiteral("微信专清"),
        accountCacheDirs(
            {winJoin({userProfile, QStringLiteral("Documents"), QStringLiteral("WeChat Files")}),
             winJoin({userProfile, QStringLiteral("Documents"), QStringLiteral("xwechat_files")}),
             winJoin({userProfile, QStringLiteral("Documents"), QStringLiteral("WXWork")}),
             winJoin({localAppData, QStringLiteral("Tencent"), QStringLiteral("WeChat")}),
             winJoin({appData, QStringLiteral("Tencent"), QStringLiteral("WeChat")})},
            {QStringLiteral("FileStorage\\Cache"),
             QStringLiteral("FileStorage\\Temp"),
             QStringLiteral("FileStorage\\Image"),
             QStringLiteral("FileStorage\\Video"),
             QStringLiteral("FileStorage\\File"),
             QStringLiteral("FileStorage\\Applet"),
             QStringLiteral("Cache"),
             QStringLiteral("cache")}
        ),
        false);
    add(QStringLiteral("qq_special_clean"), QStringLiteral("QQ专清"),
        accountCacheDirs(
            {winJoin({userProfile, QStringLiteral("Documents"), QStringLiteral("Tencent Files")}),
             winJoin({appData, QStringLiteral("Tencent"), QStringLiteral("QQ")}),
             winJoin({localAppData, QStringLiteral("Tencent"), QStringLiteral("QQ")}),
             winJoin({localAppData, QStringLiteral("Tencent"), QStringLiteral("QQNT")})},
            {QStringLiteral("Image"),
             QStringLiteral("image"),
             QStringLiteral("Video"),
             QStringLiteral("video"),
             QStringLiteral("ShortVideo"),
             QStringLiteral("shortvideo"),
             QStringLiteral("FileRecv"),
             QStringLiteral("filerecv"),
             QStringLiteral("Cache"),
             QStringLiteral("cache"),
             QStringLiteral("Temp"),
             QStringLiteral("temp")}
        ),
        false);

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

QVector<FileEntry> CleanupEngine::scanLargeFilesAsync(const QString& root, qint64 minSize, int maxFiles) {
    QVector<FileEntry> files;
    QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    int scanned = 0;
    while (iterator.hasNext() && scanned < maxFiles) {
        const QString path = iterator.next();
        ++scanned;
        const QFileInfo info(path);
        if (info.size() < minSize) {
            continue;
        }
        FileEntry entry;
        entry.name = info.fileName();
        entry.path = path;
        entry.size = info.size();
        files.push_back(entry);
    }
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.size > b.size;
    });
    return files;
}

QVector<QVector<FileEntry>> CleanupEngine::scanDuplicateFilesAsync(const QString& root, int maxFiles) {
    QHash<qint64, QVector<FileEntry>> bySize;
    QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    int scanned = 0;
    while (iterator.hasNext() && scanned < maxFiles) {
        const QString path = iterator.next();
        ++scanned;
        const QFileInfo info(path);
        if (info.size() <= 0) {
            continue;
        }
        FileEntry entry;
        entry.name = info.fileName();
        entry.path = path;
        entry.size = info.size();
        bySize[entry.size].push_back(entry);
    }

    QVector<QVector<FileEntry>> groups;
    for (auto it = bySize.begin(); it != bySize.end(); ++it) {
        if (it.value().size() < 2) {
            continue;
        }
        QHash<QString, QVector<FileEntry>> byDigest;
        for (FileEntry entry : it.value()) {
            entry.digest = fileDigest(entry.path);
            if (!entry.digest.isEmpty()) {
                byDigest[entry.digest].push_back(entry);
            }
        }
        for (auto digestIt = byDigest.begin(); digestIt != byDigest.end(); ++digestIt) {
            if (digestIt.value().size() > 1) {
                groups.push_back(digestIt.value());
            }
        }
    }
    return groups;
}

bool CleanupEngine::deletePath(const QString& path, QString* error) {
    QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;
    }
    bool ok = false;
    if (info.isDir() && !info.isSymLink()) {
        ok = QDir(path).removeRecursively();
    } else {
        ok = QFile::remove(path);
    }
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
            const QString line = stream.readLine();
            const QStringList parts = line.split(QLatin1Char('\t'));
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
    QDir().mkpath(QFileInfo(record.sourcePath).absolutePath());
    if (QFile::exists(record.sourcePath)) {
        QFile::remove(record.sourcePath);
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
    QFileInfo info(record.backupPath);
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
    BackupInfo info = backupInfo(root);
    bool ok = true;
    qint64 remainingBytes = info.totalBytes;
    for (int i = 0; i < info.backups.size(); ++i) {
        const bool overCount = i >= maxBackups;
        const bool overBytes = remainingBytes > maxBytes;
        if (!overCount && !overBytes) {
            continue;
        }
        QString error;
        if (deleteBackupItem(info.backups.at(i), &error)) {
            remainingBytes -= info.backups.at(i).size;
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
        if (part.isEmpty()) {
            continue;
        }
        cleaned.push_back(part);
    }
    QString joined = cleaned.join(QStringLiteral("\\"));
    joined.replace(QRegularExpression(QStringLiteral("\\\\+")), QStringLiteral("\\"));
    return joined;
}

QStringList CleanupEngine::accountCacheDirs(const QStringList& baseDirs, const QStringList& subDirs) {
    QStringList resolved;
    QSet<QString> seen;

    auto appendIfDir = [&resolved, &seen](const QString& candidate) {
        if (candidate.isEmpty()) {
            return;
        }
        const QFileInfo info(candidate);
        if (!info.isDir()) {
            return;
        }
        const QString path = QDir::toNativeSeparators(info.absoluteFilePath());
        const QString key = path.toLower();
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        resolved.push_back(path);
    };

    auto childPath = [](const QString& base, const QString& sub) {
        return QDir::toNativeSeparators(QDir(base).filePath(QDir::fromNativeSeparators(sub)));
    };

    for (const QString& base : baseDirs) {
        const QFileInfo baseInfo(base);
        if (!baseInfo.isDir()) {
            continue;
        }

        for (const QString& sub : subDirs) {
            appendIfDir(childPath(baseInfo.absoluteFilePath(), sub));
        }

        const QFileInfoList accounts = QDir(baseInfo.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& account : accounts) {
            for (const QString& sub : subDirs) {
                appendIfDir(childPath(account.absoluteFilePath(), sub));
            }
        }
    }

    return resolved;
}

bool CleanupEngine::pathMatches(const QString& path, const CleanupRule& rule) {
    const QString normalized = QDir::toNativeSeparators(path);
    if (!containsAnyMarker(normalized, rule.pathContains)) {
        return false;
    }
    if (rule.patterns.isEmpty()) {
        return true;
    }

    const QString fileName = QFileInfo(path).fileName();
    for (const QString& pattern : rule.patterns) {
        if (QDir::match(pattern, fileName) || fileName.contains(pattern, Qt::CaseInsensitive)) {
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

QString CleanupEngine::fileDigest(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString CleanupEngine::defaultBackupRoot() {
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
    QFileInfo root(rootPath);
    if (!root.exists()) {
        return;
    }

    if (root.isFile()) {
        if (pathMatches(rootPath, rule)) {
            files->push_back(rootPath);
            *totalSize += root.size();
            ++(*count);
        }
        return;
    }

    QDirIterator iterator(rootPath, safeGlobPatterns(rule), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isDir()) {
            continue;
        }
        if (!pathMatches(path, rule)) {
            continue;
        }
        files->push_back(path);
        *totalSize += info.size();
        ++(*count);
        if (progress && *count % 100 == 0) {
            progress(path, *count);
        }
    }
}

bool CleanupEngine::emptyRecycleBin(QString* error) {
#ifdef Q_OS_WIN
    QProcess process;
    process.start(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-Command"),
            QStringLiteral("Clear-RecycleBin -DriveLetter C -Force -ErrorAction SilentlyContinue")
        }
    );
    if (!process.waitForFinished(30000)) {
        process.kill();
        if (error) {
            *error = QStringLiteral("清空回收站超时。");
        }
        return false;
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
        return true;
    }
    if (error) {
        const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        *error = stderrText.isEmpty()
            ? QStringLiteral("清空回收站失败，退出码: %1").arg(process.exitCode())
            : stderrText;
    }
    return false;
#else
    Q_UNUSED(error);
    return true;
#endif
}

bool CleanupEngine::backupFile(const QString& source, const QString& backupRoot, QString* error) {
    QFileInfo info(source);
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
