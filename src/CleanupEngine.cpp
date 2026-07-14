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
    CleanupScanResult result;
    int count = 0;
    for (const CleanupRule& rule : cleanupRules()) {
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
        } else if (!entry.privacySensitive) {
            selected.push_back(entry);
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
    const QString systemRoot = envPath(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    const QString systemDrive = envPath(QStringLiteral("SystemDrive"), QStringLiteral("C:"));

    QVector<CleanupRule> rules;
    auto add = [&rules](
        const QString& id,
        const QString& category,
        const QString& title,
        const QString& risk,
        const QStringList& paths,
        const QStringList& patterns = {},
        const QStringList& pathContains = {},
        bool recommended = true,
        bool scanOnly = false,
        bool aggregate = false
    ) {
        CleanupRule rule;
        rule.id = id;
        rule.category = category;
        rule.title = title;
        rule.riskLabel = risk;
        rule.paths = paths;
        rule.patterns = patterns;
        rule.pathContains = pathContains;
        rule.scanOnly = scanOnly;
        rule.aggregate = aggregate;
        rule.recommended = recommended;
        rule.professional = true;
        rule.privacySensitive = false;
        rules.push_back(rule);
    };

    add(
        QStringLiteral("system_temp"),
        QStringLiteral("系统临时文件"),
        QStringLiteral("系统与用户临时文件"),
        QStringLiteral("安全"),
        {envPath(QStringLiteral("TEMP"), winJoin({localAppData, QStringLiteral("Temp")})), winJoin({systemRoot, QStringLiteral("Temp")})}
    );
    add(
        QStringLiteral("windows_update_cache"),
        QStringLiteral("Windows 更新缓存"),
        QStringLiteral("Windows 更新下载与临时缓存"),
        QStringLiteral("谨慎"),
        {winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("Download")}), winJoin({systemRoot, QStringLiteral("SoftwareDistribution"), QStringLiteral("Temp")})},
        {}, {}, false
    );
    add(
        QStringLiteral("system_logs"),
        QStringLiteral("系统日志与报错文件"),
        QStringLiteral("系统日志"),
        QStringLiteral("安全"),
        {winJoin({systemRoot, QStringLiteral("Logs")}), winJoin({systemRoot, QStringLiteral("Panther")})},
        {QStringLiteral("*.log"), QStringLiteral("*.etl"), QStringLiteral("*.tmp")}
    );
    add(
        QStringLiteral("dump_files"),
        QStringLiteral("系统日志与报错文件"),
        QStringLiteral("报错 DUMP 文件"),
        QStringLiteral("安全"),
        {winJoin({systemRoot, QStringLiteral("Minidump")}), winJoin({systemRoot, QStringLiteral("MEMORY.DMP")}), winJoin({localAppData, QStringLiteral("CrashDumps")})},
        {QStringLiteral("*.dmp"), QStringLiteral("MEMORY.DMP")}
    );
    add(
        QStringLiteral("thumbnail_cache"),
        QStringLiteral("缩略图与回收站"),
        QStringLiteral("缩略图缓存"),
        QStringLiteral("安全"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("Explorer")})},
        {QStringLiteral("thumbcache_*.db"), QStringLiteral("iconcache_*.db")}
    );
    add(
        QStringLiteral("recycle_bin"),
        QStringLiteral("缩略图与回收站"),
        QStringLiteral("回收站文件"),
        QStringLiteral("谨慎"),
        {winJoin({systemDrive, QStringLiteral("$Recycle.Bin")})},
        {}, {}, false, false, true
    );
    add(
        QStringLiteral("edge_cache"),
        QStringLiteral("浏览器缓存"),
        QStringLiteral("Edge 缓存"),
        QStringLiteral("安全"),
        {winJoin({localAppData, QStringLiteral("Microsoft"), QStringLiteral("Edge"), QStringLiteral("User Data")})},
        {}, {QStringLiteral("\\cache\\"), QStringLiteral("\\code cache\\"), QStringLiteral("\\gpucache\\")}
    );
    add(
        QStringLiteral("chrome_cache"),
        QStringLiteral("浏览器缓存"),
        QStringLiteral("Chrome 缓存"),
        QStringLiteral("安全"),
        {winJoin({localAppData, QStringLiteral("Google"), QStringLiteral("Chrome"), QStringLiteral("User Data")})},
        {}, {QStringLiteral("\\cache\\"), QStringLiteral("\\code cache\\"), QStringLiteral("\\gpucache\\")}
    );
    add(
        QStringLiteral("qq_browser_cache"),
        QStringLiteral("浏览器缓存"),
        QStringLiteral("QQ 浏览器缓存"),
        QStringLiteral("安全"),
        {winJoin({localAppData, QStringLiteral("Tencent"), QStringLiteral("QQBrowser")}), winJoin({appData, QStringLiteral("Tencent"), QStringLiteral("QQBrowser")})},
        {}, {QStringLiteral("\\cache\\"), QStringLiteral("\\code cache\\"), QStringLiteral("\\gpucache\\")}
    );
    add(
        QStringLiteral("redundant_packages"),
        QStringLiteral("冗余安装包、压缩包与镜像"),
        QStringLiteral("用户目录中的安装包、压缩包和镜像"),
        QStringLiteral("谨慎"),
        {winJoin({userProfile, QStringLiteral("Downloads")}), winJoin({userProfile, QStringLiteral("Desktop")}), winJoin({userProfile, QStringLiteral("Documents")})},
        {QStringLiteral("*.exe"), QStringLiteral("*.msi"), QStringLiteral("*.zip"), QStringLiteral("*.rar"), QStringLiteral("*.7z"), QStringLiteral("*.iso")},
        {}, false, true
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
        if (pathMatches(rootPath, rule)) {
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
        if (!pathMatches(path, rule)) {
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
