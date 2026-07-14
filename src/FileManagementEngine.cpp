#include "FileManagementEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <filesystem>
#include <queue>
#include <string>
#include <system_error>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QString envPath(const QString& name, const QString& fallback = {}) {
    const QString value = qEnvironmentVariable(name.toLocal8Bit().constData());
    return value.isEmpty() ? fallback : QDir::toNativeSeparators(value);
}

QString homePath() {
#ifdef Q_OS_WIN
    return envPath(QStringLiteral("USERPROFILE"), QDir::homePath());
#else
    return QDir::homePath();
#endif
}

QString joinPath(const QString& base, std::initializer_list<QString> parts) {
    QDir dir(base);
    QString result = base;
    for (const QString& part : parts) {
        result = QDir(result).filePath(part);
    }
    return QDir::toNativeSeparators(result);
}

bool isSameOrChild(const QString& path, const QString& root) {
    const QString cleanPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
    const QString cleanRoot = QDir::cleanPath(QFileInfo(root).absoluteFilePath()).toLower();
    return cleanPath == cleanRoot || cleanPath.startsWith(cleanRoot + QLatin1Char('/'));
}

bool shouldSkipScanDirectory(const QFileInfo& info) {
    return info.isDir() && FileManagementEngine::isReparsePoint(info.absoluteFilePath());
}

QString topLevelUsagePath(const QDir& rootDir, const QFileInfo& info) {
    const QString relativePath = rootDir.relativeFilePath(info.absoluteFilePath());
    if (relativePath.isEmpty() || relativePath.startsWith(QStringLiteral(".."))) {
        return info.absoluteFilePath();
    }
    const int slash = relativePath.indexOf(QLatin1Char('/'));
    const QString firstSegment = slash >= 0 ? relativePath.left(slash) : relativePath;
    return QFileInfo(rootDir.filePath(firstSegment)).absoluteFilePath();
}

QString uniqueTargetPath(const QString& path) {
    const QFileInfo info(path);
    const QString directory = info.absolutePath();
    const QString fileName = info.fileName();
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? fileName.left(dot) : fileName;
    const QString extension = dot > 0 ? fileName.mid(dot) : QString();
    QString candidate;
    int counter = 1;
    do {
        candidate = QDir(directory).filePath(QStringLiteral("%1_%2%3").arg(stem).arg(counter).arg(extension));
        ++counter;
    } while (QFileInfo::exists(candidate));
    return candidate;
}

bool directoryHasEntries(const QString& path) {
    return !QDir(path).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot
    ).isEmpty();
}

QString powerShellSingleQuoted(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'%1'").arg(value);
}

#ifdef Q_OS_WIN
QString windowsErrorMessage(const QString& action, DWORD code) {
    return QStringLiteral("%1失败，Windows 错误码: %2").arg(action).arg(code);
}
#endif

}  // namespace

QVector<ManagedFileEntry> FileManagementEngine::listFiles(const QString& rootPath, ManagedFileType type, int limit) const {
    QVector<ManagedFileEntry> files;
    if (!QFileInfo(rootPath).isDir()) {
        return files;
    }

    struct SmallestManagedFileFirst {
        bool operator()(const ManagedFileEntry& left, const ManagedFileEntry& right) const {
            return left.sizeBytes > right.sizeBytes;
        }
    };
    std::priority_queue<ManagedFileEntry, std::vector<ManagedFileEntry>, SmallestManagedFileFirst> largestFiles;
    QDirIterator iterator(rootPath, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const ManagedFileType detected = detectType(path);
        if (type != ManagedFileType::All && detected != type) {
            continue;
        }
        const QFileInfo info(path);
        ManagedFileEntry entry;
        entry.path = path;
        entry.name = info.fileName();
        entry.sizeBytes = info.size();
        entry.type = detected;
        if (limit <= 0) {
            files.push_back(entry);
        } else if (static_cast<int>(largestFiles.size()) < limit) {
            largestFiles.push(entry);
        } else if (entry.sizeBytes > largestFiles.top().sizeBytes) {
            largestFiles.pop();
            largestFiles.push(entry);
        }
    }
    while (!largestFiles.empty()) {
        files.push_back(largestFiles.top());
        largestFiles.pop();
    }

    std::sort(files.begin(), files.end(), [](const ManagedFileEntry& a, const ManagedFileEntry& b) {
        return a.sizeBytes > b.sizeBytes;
    });
    return files;
}

QVector<FolderUsageEntry> FileManagementEngine::scanFolderUsage(const QString& rootPath, int limit) const {
    return scanFolderUsageDetailed(rootPath, limit, 0).folders;
}

FolderUsageScan FileManagementEngine::scanFolderUsageDetailed(const QString& rootPath, int folderLimit, int fileLimit) const {
    FolderUsageScan scan;
    QDir root(rootPath);
    if (!root.exists()) {
        return scan;
    }

    root = QDir(QFileInfo(root.absolutePath()).absoluteFilePath());
    const QString rootAbsolutePath = QFileInfo(root.absolutePath()).absoluteFilePath();
    QMap<QString, FolderUsageEntry> topLevelUsage;
    QMap<QString, ExtensionUsageEntry> extensionUsage;
    QVector<QString> pending = {rootAbsolutePath};
    const bool collectFileDetails = fileLimit != 0;
    struct SmallestFileFirst {
        bool operator()(const FileUsageEntry& left, const FileUsageEntry& right) const {
            return left.sizeBytes > right.sizeBytes;
        }
    };
    std::priority_queue<FileUsageEntry, std::vector<FileUsageEntry>, SmallestFileFirst> largestFiles;

    while (!pending.isEmpty()) {
        const QString current = pending.takeLast();
        const QFileInfo currentInfo(current);
        if (currentInfo.absoluteFilePath() != rootAbsolutePath && shouldSkipScanDirectory(currentInfo)) {
            continue;
        }

        const QFileInfoList entries = QDir(current).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks
        );
        for (const QFileInfo& info : entries) {
            if (info.isDir()) {
                if (shouldSkipScanDirectory(info)) {
                    continue;
                }
                if (currentInfo.absoluteFilePath() == rootAbsolutePath) {
                    FolderUsageEntry& topEntry = topLevelUsage[info.absoluteFilePath()];
                    if (topEntry.path.isEmpty()) {
                        topEntry.path = info.absoluteFilePath();
                    }
                }
                pending.push_back(info.absoluteFilePath());
                continue;
            }
            if (!info.isFile()) {
                continue;
            }

            const qint64 sizeBytes = info.size();
            const QString topPath = topLevelUsagePath(root, info);
            FolderUsageEntry& topEntry = topLevelUsage[topPath];
            if (topEntry.path.isEmpty()) {
                topEntry.path = topPath;
            }
            topEntry.sizeBytes += sizeBytes;
            ++topEntry.fileCount;

            if (!collectFileDetails) {
                continue;
            }

            const QString extension = normalizedExtension(info.absoluteFilePath());
            ExtensionUsageEntry& extensionEntry = extensionUsage[extension];
            if (extensionEntry.extension.isEmpty()) {
                extensionEntry.extension = extension;
                extensionEntry.description = extensionDescription(extension);
            }
            extensionEntry.sizeBytes += sizeBytes;
            ++extensionEntry.fileCount;

            FileUsageEntry fileEntry;
            fileEntry.path = info.absoluteFilePath();
            fileEntry.extension = extension;
            fileEntry.sizeBytes = sizeBytes;
            fileEntry.fileCount = 1;
            if (fileLimit < 0) {
                scan.files.push_back(fileEntry);
            } else if (static_cast<int>(largestFiles.size()) < fileLimit) {
                largestFiles.push(fileEntry);
            } else if (fileEntry.sizeBytes > largestFiles.top().sizeBytes) {
                largestFiles.pop();
                largestFiles.push(fileEntry);
            }
        }
    }

    for (auto it = topLevelUsage.cbegin(); it != topLevelUsage.cend(); ++it) {
        scan.folders.push_back(it.value());
    }
    std::sort(scan.folders.begin(), scan.folders.end(), [](const FolderUsageEntry& a, const FolderUsageEntry& b) {
        return a.sizeBytes > b.sizeBytes;
    });
    if (folderLimit > 0 && scan.folders.size() > folderLimit) {
        scan.folders.resize(folderLimit);
    }

    if (collectFileDetails) {
        while (!largestFiles.empty()) {
            scan.files.push_back(largestFiles.top());
            largestFiles.pop();
        }
        for (auto it = extensionUsage.cbegin(); it != extensionUsage.cend(); ++it) {
            scan.extensions.push_back(it.value());
        }
        std::sort(scan.extensions.begin(), scan.extensions.end(), [](const ExtensionUsageEntry& a, const ExtensionUsageEntry& b) {
            return a.sizeBytes > b.sizeBytes;
        });

        std::sort(scan.files.begin(), scan.files.end(), [](const FileUsageEntry& a, const FileUsageEntry& b) {
            return a.sizeBytes > b.sizeBytes;
        });
    }

    return scan;
}

FileOperationResult FileManagementEngine::copyFiles(const QStringList& paths, const QString& targetDirectory) const {
    return copyOrMove(paths, targetDirectory, false);
}

FileOperationResult FileManagementEngine::moveFiles(const QStringList& paths, const QString& targetDirectory) const {
    return copyOrMove(paths, targetDirectory, true);
}

FileOperationResult FileManagementEngine::renameFile(const QString& path, const QString& newName) const {
    FileOperationResult result;
    QFileInfo info(path);
    if (!info.exists()) {
        result.errors.push_back(QStringLiteral("文件不存在: %1").arg(path));
        return result;
    }
    const QString target = QDir(info.absolutePath()).filePath(newName);
    if (QFile::rename(path, target)) {
        result.affectedPaths.push_back(target);
    } else {
        result.errors.push_back(QStringLiteral("重命名失败: %1").arg(path));
    }
    return result;
}

FileOperationResult FileManagementEngine::renameFiles(const QStringList& paths, const QString& prefix) const {
    FileOperationResult result;
    QString cleanPrefix = prefix.trimmed();
    cleanPrefix.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    if (paths.isEmpty() || cleanPrefix.isEmpty()) {
        result.errors.push_back(QStringLiteral("请选择文件并输入有效的批量重命名前缀。"));
        return result;
    }

    struct RenamePlan {
        QString source;
        QString temporary;
        QString target;
    };
    QVector<RenamePlan> plans;
    QSet<QString> targets;
    const QString operationId = QUuid::createUuid().toString(QUuid::Id128);
    for (int index = 0; index < paths.size(); ++index) {
        const QFileInfo info(paths.at(index));
        if (!info.exists() || !info.isFile()) {
            result.errors.push_back(QStringLiteral("只能批量重命名现有文件: %1").arg(paths.at(index)));
            return result;
        }
        const QString extension = info.suffix().isEmpty() ? QString() : QStringLiteral(".%1").arg(info.suffix());
        const QString target = QDir(info.absolutePath()).filePath(
            QStringLiteral("%1_%2%3").arg(cleanPrefix).arg(index + 1, 3, 10, QLatin1Char('0')).arg(extension)
        );
        const QString normalizedTarget = QDir::cleanPath(target).toLower();
        if (targets.contains(normalizedTarget)
            || (QFileInfo::exists(target) && QFileInfo(target).absoluteFilePath().compare(info.absoluteFilePath(), Qt::CaseInsensitive) != 0)) {
            result.errors.push_back(QStringLiteral("目标文件已存在，未执行批量重命名: %1").arg(target));
            return result;
        }
        targets.insert(normalizedTarget);
        plans.push_back({
            info.absoluteFilePath(),
            QDir(info.absolutePath()).filePath(QStringLiteral(".c_diskglow_rename_%1_%2.tmp").arg(operationId).arg(index)),
            target,
        });
    }

    int staged = 0;
    for (; staged < plans.size(); ++staged) {
        if (!QFile::rename(plans.at(staged).source, plans.at(staged).temporary)) {
            result.errors.push_back(QStringLiteral("暂存重命名失败: %1").arg(plans.at(staged).source));
            break;
        }
    }
    if (staged != plans.size()) {
        for (int index = staged - 1; index >= 0; --index) {
            QFile::rename(plans.at(index).temporary, plans.at(index).source);
        }
        return result;
    }

    int completed = 0;
    for (; completed < plans.size(); ++completed) {
        if (!QFile::rename(plans.at(completed).temporary, plans.at(completed).target)) {
            result.errors.push_back(QStringLiteral("批量重命名失败: %1").arg(plans.at(completed).target));
            break;
        }
        result.affectedPaths.push_back(plans.at(completed).target);
    }
    if (completed != plans.size()) {
        for (int index = completed - 1; index >= 0; --index) {
            QFile::rename(plans.at(index).target, plans.at(index).source);
        }
        for (int index = completed; index < plans.size(); ++index) {
            QFile::rename(plans.at(index).temporary, plans.at(index).source);
        }
        result.affectedPaths.clear();
    }
    return result;
}

FileOperationResult FileManagementEngine::deleteFiles(const QStringList& paths) const {
    FileOperationResult result;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (!info.exists()) {
            continue;
        }
        bool ok = false;
        if (info.isDir() && !info.isSymLink()) {
            ok = QDir(path).removeRecursively();
        } else {
            ok = QFile::remove(path);
        }
        if (ok) {
            result.affectedPaths.push_back(path);
        } else {
            result.errors.push_back(QStringLiteral("删除失败: %1").arg(path));
        }
    }
    return result;
}

FileOperationResult FileManagementEngine::shredFiles(const QStringList& paths) const {
    FileOperationResult result;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            result.errors.push_back(QStringLiteral("只能粉碎现有文件: %1").arg(path));
            continue;
        }
        QFile file(path);
        const qint64 length = info.size();
        if (!file.open(QIODevice::ReadWrite)) {
            result.errors.push_back(QStringLiteral("无法打开文件粉碎: %1").arg(path));
            continue;
        }
        bool overwritten = true;
        for (int pass = 0; pass < 2 && overwritten; ++pass) {
            if (!file.seek(0)) {
                overwritten = false;
                break;
            }
            qint64 written = 0;
            QByteArray block(8192, '\0');
            while (written < length) {
                const qint64 count = qMin<qint64>(block.size(), length - written);
                if (pass == 0) {
                    for (qint64 index = 0; index < count; ++index) {
                        block[static_cast<int>(index)] = static_cast<char>(QRandomGenerator::global()->generate() & 0xff);
                    }
                } else {
                    block.fill('\0');
                }
                if (file.write(block.constData(), count) != count) {
                    overwritten = false;
                    break;
                }
                written += count;
            }
            overwritten = overwritten && file.flush();
        }
        file.close();
        if (!overwritten) {
            result.errors.push_back(QStringLiteral("文件未完整覆写，已保留原文件: %1").arg(path));
        } else if (QFile::remove(path)) {
            result.affectedPaths.push_back(path);
        } else {
            result.errors.push_back(QStringLiteral("粉碎后删除失败: %1").arg(path));
        }
    }
    return result;
}

FileOperationResult FileManagementEngine::migrateFilesWithShortcuts(const QStringList& paths, const QString& targetDirectory) const {
    FileOperationResult result;
    if (!QDir().mkpath(targetDirectory)) {
        result.errors.push_back(QStringLiteral("创建迁移目标失败: %1").arg(targetDirectory));
        return result;
    }
    for (const QString& source : paths) {
        const QFileInfo sourceInfo(source);
        if (!sourceInfo.exists() || !sourceInfo.isFile()) {
            result.errors.push_back(QStringLiteral("迁移源文件不存在: %1").arg(source));
            continue;
        }
        const QString destination = QDir(targetDirectory).filePath(sourceInfo.fileName());
        const QString shortcutPath = sourceInfo.absoluteFilePath() + QStringLiteral(".lnk");
        if (QFileInfo::exists(destination) || QFileInfo::exists(shortcutPath)) {
            result.errors.push_back(QStringLiteral("目标文件或原位快捷方式已存在，已跳过: %1").arg(source));
            continue;
        }

        FileOperationResult moved = moveFiles({sourceInfo.absoluteFilePath()}, targetDirectory);
        if (!moved.errors.isEmpty() || !QFileInfo::exists(destination)) {
            result.errors.append(moved.errors);
            if (moved.errors.isEmpty()) {
                result.errors.push_back(QStringLiteral("迁移后未找到目标文件: %1").arg(destination));
            }
            continue;
        }
        FileOperationResult shortcut = createShortcut(destination, shortcutPath);
        if (!shortcut.errors.isEmpty() || !QFileInfo::exists(shortcutPath)) {
            result.errors.append(shortcut.errors);
            FileOperationResult rollback = moveFiles({destination}, sourceInfo.absolutePath());
            if (!rollback.errors.isEmpty()) {
                result.errors.push_back(QStringLiteral("快捷方式创建失败且文件回滚失败: %1").arg(source));
                result.errors.append(rollback.errors);
            }
            continue;
        }
        result.affectedPaths.push_back(destination);
        result.affectedPaths.push_back(shortcutPath);
    }
    return result;
}

FileOperationResult FileManagementEngine::createShortcut(const QString& target, const QString& shortcutPath) const {
#ifdef Q_OS_WIN
    const QString script = QStringLiteral("$s=New-Object -ComObject WScript.Shell; $l=$s.CreateShortcut(%1); $l.TargetPath=%2; $l.Save()")
        .arg(powerShellSingleQuoted(shortcutPath), powerShellSingleQuoted(target));
    return runCommand(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), script});
#else
    Q_UNUSED(target);
    Q_UNUSED(shortcutPath);
    return {{}, {}, QStringLiteral("创建快捷方式仅支持 Windows。"), true};
#endif
}

FileOperationResult FileManagementEngine::repairFolderPermission(const QString& path) const {
#ifdef Q_OS_WIN
    return runCommand(QStringLiteral("icacls"), {path, QStringLiteral("/reset"), QStringLiteral("/T"), QStringLiteral("/C")});
#else
    Q_UNUSED(path);
    return {{}, {}, QStringLiteral("文件夹权限修复仅支持 Windows。"), true};
#endif
}

QVector<MigrationFolder> FileManagementEngine::migrationCatalog() const {
    const QString home = homePath();
    const QString local = envPath(QStringLiteral("LOCALAPPDATA"), joinPath(home, {QStringLiteral("AppData"), QStringLiteral("Local")}));
    const QString roaming = envPath(QStringLiteral("APPDATA"), joinPath(home, {QStringLiteral("AppData"), QStringLiteral("Roaming")}));
    const QString documents = joinPath(home, {QStringLiteral("Documents")});
    const QString systemRoot = envPath(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    return {
        {QStringLiteral("desktop"), QStringLiteral("桌面"), QStringLiteral("Desktop"), joinPath(home, {QStringLiteral("Desktop")})},
        {QStringLiteral("documents"), QStringLiteral("我的文档"), QStringLiteral("Documents"), documents},
        {QStringLiteral("downloads"), QStringLiteral("下载"), QStringLiteral("Downloads"), joinPath(home, {QStringLiteral("Downloads")})},
        {QStringLiteral("videos"), QStringLiteral("我的视频"), QStringLiteral("Videos"), joinPath(home, {QStringLiteral("Videos")})},
        {QStringLiteral("pictures"), QStringLiteral("我的图片"), QStringLiteral("Pictures"), joinPath(home, {QStringLiteral("Pictures")})},
        {QStringLiteral("appdata_cache"), QStringLiteral("AppData 本地软件缓存（微信/QQ）"), QStringLiteral("AppData-Local-Tencent"), joinPath(local, {QStringLiteral("Tencent")})},
        {QStringLiteral("appdata_roaming_cache"), QStringLiteral("AppData 漫游软件缓存（微信/QQ）"), QStringLiteral("AppData-Roaming-Tencent"), joinPath(roaming, {QStringLiteral("Tencent")})},
        {QStringLiteral("wechat_data"), QStringLiteral("微信数据目录"), QStringLiteral("WeChat-Files"), joinPath(documents, {QStringLiteral("WeChat Files")})},
        {QStringLiteral("qq_data"), QStringLiteral("QQ 数据目录"), QStringLiteral("Tencent-Files"), joinPath(documents, {QStringLiteral("Tencent Files")})},
        {QStringLiteral("temp"), QStringLiteral("当前用户 Temp 临时文件夹"), QStringLiteral("User-Temp"), joinPath(local, {QStringLiteral("Temp")})},
        {QStringLiteral("system_temp"), QStringLiteral("Windows 系统 Temp 临时文件夹"), QStringLiteral("System-Temp"), joinPath(systemRoot, {QStringLiteral("Temp")})},
    };
}

QVector<MigrationFolder> FileManagementEngine::scanMigrationFolders() const {
    QVector<MigrationFolder> folders = migrationCatalog();
    for (MigrationFolder& folder : folders) {
        const QFileInfo info(folder.path);
        folder.exists = info.exists();
        folder.migrated = isReparsePoint(folder.path);
        folder.target = folder.migrated ? junctionTarget(folder.path) : QString();
        folder.sizeBytes = folder.exists ? directorySize(folder.migrated && !folder.target.isEmpty() ? folder.target : folder.path) : 0;
    }
    return folders;
}

FileOperationResult FileManagementEngine::migratePersonalFolder(const QString& folderKey, const QString& targetRoot, bool moveFiles) const {
    FileOperationResult result;
    const MigrationFolder folder = migrationFolderByKey(folderKey);
    if (folder.key.isEmpty()) {
        result.errors.push_back(QStringLiteral("未知的文件夹项: %1").arg(folderKey));
        return result;
    }
    if (isReparsePoint(folder.path)) {
        result.errors.push_back(QStringLiteral("已经迁移过: %1").arg(folder.name));
        return result;
    }

    const QString applicationPath = QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath();
    if (!applicationPath.isEmpty() && isSameOrChild(applicationPath, folder.path)) {
        result.errors.push_back(QStringLiteral("本程序正位于“%1”内，运行中无法移动。请先把程序移动到其它磁盘后再迁移该文件夹。").arg(folder.name));
        return result;
    }

    const QString targetRootPath = QFileInfo(targetRoot).absoluteFilePath();
    const QString target = QDir(targetRootPath).filePath(folder.subName);
    if (isSameOrChild(target, folder.path)) {
        result.errors.push_back(QStringLiteral("目标目录不能位于原目录内部: %1").arg(target));
        return result;
    }
    if (!QDir().mkpath(targetRootPath)) {
        result.errors.push_back(QStringLiteral("创建目标根目录失败: %1").arg(targetRootPath));
        return result;
    }
    if (!ensureSupportsJunction(targetRootPath, folder.name, &result)) {
        return result;
    }
    const QFileInfo targetInfo(target);
    if (targetInfo.exists()
        && (!targetInfo.isDir() || targetInfo.isSymLink() || isReparsePoint(target))) {
        result.errors.push_back(QStringLiteral("迁移目标不是可用的普通目录: %1").arg(target));
        return result;
    }
    if (targetInfo.exists() && directoryHasEntries(target)) {
        result.errors.push_back(QStringLiteral("迁移目标已存在内容，为避免混入旧文件和破坏回滚，未执行迁移: %1").arg(target));
        return result;
    }
    if (!QDir().mkpath(target)) {
        result.errors.push_back(QStringLiteral("创建目标目录失败: %1").arg(target));
        return result;
    }

    const QFileInfo sourceInfo(folder.path);
    const bool sourceExisted = sourceInfo.exists();
    bool moved = false;
    if (sourceExisted) {
        if (!sourceInfo.isDir()) {
            result.errors.push_back(QStringLiteral("原路径不是目录: %1").arg(folder.path));
            return result;
        }
        if (moveFiles) {
            if (!mergeMoveDirectoryContents(folder.path, target, folder.name, &result)) {
                rollbackMigration(target, folder.path);
                return result;
            }
            moved = true;
        } else {
            if (directoryHasEntries(folder.path)) {
                result.errors.push_back(QStringLiteral("“%1”内还有文件，请勾选“迁移时移动原文件”后再迁移。").arg(folder.name));
                return result;
            }
        }

        if (!removeEmptyDirectory(folder.path, &result)) {
            if (moved) {
                rollbackMigration(target, folder.path);
            }
            result.errors.push_back(QStringLiteral("“%1”中可能有文件正被占用，无法完成迁移。请关闭相关程序后重试。").arg(folder.name));
            return result;
        }
    } else {
        QDir().mkpath(QFileInfo(folder.path).absolutePath());
    }

    FileOperationResult junction = createJunction(folder.path, target);
    if (!junction.errors.isEmpty()) {
        result.errors.append(junction.errors);
        if (sourceExisted || moved) {
            rollbackMigration(target, folder.path);
        }
        return result;
    }
    if (!updateMigrationRedirect(folder, target, &result)) {
        removeJunction(folder.path, &result);
        rollbackMigration(target, folder.path);
        return result;
    }
    result.affectedPaths.append(junction.affectedPaths);
    result.affectedPaths.push_back(target);
    result.affectedPaths.push_back(folder.path);
    return result;
}

FileOperationResult FileManagementEngine::restorePersonalFolder(const QString& folderKey) const {
    FileOperationResult result;
    const MigrationFolder folder = migrationFolderByKey(folderKey);
    if (folder.key.isEmpty()) {
        result.errors.push_back(QStringLiteral("未知的文件夹项: %1").arg(folderKey));
        return result;
    }
    if (!isReparsePoint(folder.path)) {
        result.errors.push_back(QStringLiteral("未处于迁移状态: %1").arg(folder.name));
        return result;
    }
    const QString target = junctionTarget(folder.path);
    if (target.isEmpty()) {
        result.errors.push_back(QStringLiteral("无法读取迁移目标: %1").arg(folder.path));
        return result;
    }
    if (!removeJunction(folder.path, &result)) {
        return result;
    }
    if (!QDir().mkpath(folder.path)) {
        result.errors.push_back(QStringLiteral("创建还原目录失败: %1").arg(folder.path));
        FileOperationResult junction = createJunction(folder.path, target);
        result.errors.append(junction.errors);
        return result;
    }
    if (!target.isEmpty() && QFileInfo::exists(target) && QFileInfo(target).absoluteFilePath() != QFileInfo(folder.path).absoluteFilePath()) {
        if (!mergeMoveDirectoryContents(target, folder.path, folder.name, &result)) {
            FileOperationResult rollback;
            mergeMoveDirectoryContents(folder.path, target, folder.name, &rollback);
            removeEmptyDirectory(folder.path, &rollback);
            const FileOperationResult junction = createJunction(folder.path, target);
            rollback.errors.append(junction.errors);
            updateMigrationRedirect(folder, target, &rollback);
            result.errors.push_back(QStringLiteral("还原失败，已尝试恢复原迁移状态。"));
            result.errors.append(rollback.errors);
            return result;
        }
        if (directoryHasEntries(target)) {
            FileOperationResult rollback;
            mergeMoveDirectoryContents(folder.path, target, folder.name, &rollback);
            removeEmptyDirectory(folder.path, &rollback);
            const FileOperationResult junction = createJunction(folder.path, target);
            rollback.errors.append(junction.errors);
            updateMigrationRedirect(folder, target, &rollback);
            result.errors.push_back(QStringLiteral("目标目录在还原期间产生了新文件，已停止并尝试恢复原迁移状态。"));
            result.errors.append(rollback.errors);
            return result;
        }
    }
    if (!updateMigrationRedirect(folder, folder.path, &result)) {
        FileOperationResult rollback;
        mergeMoveDirectoryContents(folder.path, target, folder.name, &rollback);
        removeEmptyDirectory(folder.path, &rollback);
        const FileOperationResult junction = createJunction(folder.path, target);
        rollback.errors.append(junction.errors);
        updateMigrationRedirect(folder, target, &rollback);
        result.errors.push_back(QStringLiteral("系统路径更新失败，已尝试恢复原迁移状态。"));
        result.errors.append(rollback.errors);
        return result;
    }
    if (!target.isEmpty() && QFileInfo::exists(target) && !QDir().rmdir(target)) {
        result.errors.push_back(QStringLiteral("目标空目录无法删除，可稍后手动删除: %1").arg(target));
    }
    result.affectedPaths.push_back(folder.path);
    return result;
}

FileOperationResult FileManagementEngine::createJunction(const QString& source, const QString& target) const {
#ifdef Q_OS_WIN
    FileOperationResult result = runCommand(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("mklink"), QStringLiteral("/J"), source, target});
    if (!result.errors.isEmpty()) {
        const QString detail = result.output.trimmed().isEmpty() ? result.errors.join(QStringLiteral("\n")) : result.output.trimmed();
        QString hint;
        const QString lower = detail.toLower();
        if (detail.contains(QStringLiteral("拒绝访问")) || lower.contains(QStringLiteral("denied"))) {
            hint = QStringLiteral("（请尝试以管理员身份运行本程序）");
        } else if (detail.contains(QStringLiteral("已存在")) || lower.contains(QStringLiteral("exist"))) {
            hint = QStringLiteral("（原位置仍存在同名文件夹，请手动清理后重试）");
        }
        result.errors.clear();
        result.errors.push_back(QStringLiteral("创建连接点失败: %1%2").arg(detail.isEmpty() ? QStringLiteral("未知错误") : detail, hint));
    }
    return result;
#else
    FileOperationResult result;
    std::error_code error;
    std::filesystem::create_directory_symlink(
        std::filesystem::path(target.toStdString()),
        std::filesystem::path(source.toStdString()),
        error
    );
    if (!error) {
        result.affectedPaths.push_back(source);
    } else {
        result.errors.push_back(QStringLiteral("创建连接失败: %1 -> %2，%3").arg(source, target, QString::fromStdString(error.message())));
    }
    return result;
#endif
}

ManagedFileType FileManagementEngine::detectType(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (QStringList{QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("wmv")}.contains(ext)) {
        return ManagedFileType::Video;
    }
    if (QStringList{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp")}.contains(ext)) {
        return ManagedFileType::Image;
    }
    if (QStringList{QStringLiteral("exe"), QStringLiteral("msi"), QStringLiteral("msp")}.contains(ext)) {
        return ManagedFileType::Installer;
    }
    if (QStringList{QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"), QStringLiteral("iso"), QStringLiteral("tar"), QStringLiteral("gz")}.contains(ext)) {
        return ManagedFileType::Archive;
    }
    if (QStringList{QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("xls"), QStringLiteral("xlsx"), QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("pdf"), QStringLiteral("txt")}.contains(ext)) {
        return ManagedFileType::Document;
    }
    return ManagedFileType::All;
}

QString FileManagementEngine::typeLabel(ManagedFileType type) {
    switch (type) {
    case ManagedFileType::Video:
        return QStringLiteral("视频");
    case ManagedFileType::Image:
        return QStringLiteral("图片");
    case ManagedFileType::Installer:
        return QStringLiteral("安装包");
    case ManagedFileType::Archive:
        return QStringLiteral("压缩包");
    case ManagedFileType::Document:
        return QStringLiteral("文档");
    default:
        return QStringLiteral("全部");
    }
}

QString FileManagementEngine::normalizedExtension(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    return suffix.isEmpty() ? QStringLiteral("(无扩展名)") : QStringLiteral(".%1").arg(suffix);
}

QString FileManagementEngine::extensionDescription(const QString& extension) {
    const QString key = extension.trimmed().toLower();
    if (key == QStringLiteral("(无扩展名)")) {
        return QStringLiteral("无扩展名");
    }
    static const QMap<QString, QString> descriptions = {
        {QStringLiteral(".dll"), QStringLiteral("应用程序扩展")},
        {QStringLiteral(".bin"), QStringLiteral("BIN 文件")},
        {QStringLiteral(".exe"), QStringLiteral("应用程序")},
        {QStringLiteral(".sys"), QStringLiteral("系统文件")},
        {QStringLiteral(".dat"), QStringLiteral("DAT 文件")},
        {QStringLiteral(".log"), QStringLiteral("LOG 文件")},
        {QStringLiteral(".msi"), QStringLiteral("Windows Installer 程序包")},
        {QStringLiteral(".ttf"), QStringLiteral("TrueType 字体文件")},
        {QStringLiteral(".ttc"), QStringLiteral("TrueType Collection 字体")},
        {QStringLiteral(".pak"), QStringLiteral("PAK 文件")},
        {QStringLiteral(".js"), QStringLiteral("JavaScript 文件")},
        {QStringLiteral(".jpg"), QStringLiteral("美图看看")},
        {QStringLiteral(".jpeg"), QStringLiteral("JPEG 图片")},
        {QStringLiteral(".png"), QStringLiteral("PNG 文件")},
        {QStringLiteral(".so"), QStringLiteral("SO 文件")},
        {QStringLiteral(".db"), QStringLiteral("Data Base File")},
        {QStringLiteral(".7z"), QStringLiteral("压缩存档文件")},
        {QStringLiteral(".zip"), QStringLiteral("ZIP 压缩文件")},
        {QStringLiteral(".pri"), QStringLiteral("PRI 文件")},
        {QStringLiteral(".mun"), QStringLiteral("MUN 文件")},
        {QStringLiteral(".tmp"), QStringLiteral("临时文件")},
    };
    if (descriptions.contains(key)) {
        return descriptions.value(key);
    }
    return key.startsWith(QLatin1Char('.'))
        ? QStringLiteral("%1 文件").arg(key.mid(1).toUpper())
        : QStringLiteral("文件");
}

qint64 FileManagementEngine::directorySize(const QString& path, int* fileCount) {
    qint64 total = 0;
    if (fileCount) {
        *fileCount = 0;
    }
    QVector<QString> pending = {path};
    while (!pending.isEmpty()) {
        const QString current = pending.takeLast();
        const QFileInfo currentInfo(current);
        if (currentInfo.absoluteFilePath() != QFileInfo(path).absoluteFilePath() && shouldSkipScanDirectory(currentInfo)) {
            continue;
        }
        const QFileInfoList entries = QDir(current).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks
        );
        for (const QFileInfo& info : entries) {
            if (info.isDir()) {
                if (!shouldSkipScanDirectory(info)) {
                    pending.push_back(info.absoluteFilePath());
                }
                continue;
            }
            if (!info.isFile()) {
                continue;
            }
            total += info.size();
            if (fileCount) {
                ++(*fileCount);
            }
        }
    }
    return total;
}

bool FileManagementEngine::isReparsePoint(const QString& path) {
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    const QFileInfo info(path);
    return info.isSymLink();
#endif
}

bool FileManagementEngine::copyDirectoryContents(const QString& sourceDirectory, const QString& targetDirectory, FileOperationResult* result) const {
    const QFileInfo sourceInfo(sourceDirectory);
    if (!sourceInfo.exists() || !sourceInfo.isDir()) {
        if (result) {
            result->errors.push_back(QStringLiteral("源目录不存在: %1").arg(sourceDirectory));
        }
        return false;
    }
    if (isSameOrChild(targetDirectory, sourceDirectory)) {
        if (result) {
            result->errors.push_back(QStringLiteral("目标目录不能位于源目录内部: %1").arg(targetDirectory));
        }
        return false;
    }
    if (!QDir().mkpath(targetDirectory)) {
        if (result) {
            result->errors.push_back(QStringLiteral("创建目标目录失败: %1").arg(targetDirectory));
        }
        return false;
    }

    bool ok = true;
    QDirIterator iterator(
        sourceDirectory,
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDirIterator::Subdirectories
    );
    while (iterator.hasNext()) {
        const QString sourcePath = iterator.next();
        const QFileInfo info = iterator.fileInfo();
        const QString relative = QDir(sourceDirectory).relativeFilePath(sourcePath);
        const QString targetPath = QDir(targetDirectory).filePath(relative);
        if (info.isDir()) {
            if (!QDir().mkpath(targetPath)) {
                ok = false;
                if (result) {
                    result->errors.push_back(QStringLiteral("创建目录失败: %1").arg(targetPath));
                }
            }
            continue;
        }
        if (!info.isFile()) {
            continue;
        }
        if (!QDir().mkpath(QFileInfo(targetPath).absolutePath())) {
            ok = false;
            if (result) {
                result->errors.push_back(QStringLiteral("创建目录失败: %1").arg(QFileInfo(targetPath).absolutePath()));
            }
            continue;
        }
        if (QFileInfo::exists(targetPath)) {
            ok = false;
            if (result) {
                result->errors.push_back(QStringLiteral("目标文件已存在，已停止覆盖: %1").arg(targetPath));
            }
            continue;
        }
        if (!QFile::copy(sourcePath, targetPath)) {
            ok = false;
            if (result) {
                result->errors.push_back(QStringLiteral("复制失败: %1 -> %2").arg(sourcePath, targetPath));
            }
        }
    }
    if (ok && result) {
        result->affectedPaths.push_back(targetDirectory);
    }
    return ok;
}

bool FileManagementEngine::mergeMoveDirectoryContents(const QString& sourceDirectory, const QString& targetDirectory, const QString& folderName, FileOperationResult* result) const {
    const QFileInfo sourceInfo(sourceDirectory);
    if (!sourceInfo.exists() || !sourceInfo.isDir()) {
        if (result) {
            result->errors.push_back(QStringLiteral("源目录不存在: %1").arg(sourceDirectory));
        }
        return false;
    }
    if (isSameOrChild(targetDirectory, sourceDirectory)) {
        if (result) {
            result->errors.push_back(QStringLiteral("目标目录不能位于源目录内部: %1").arg(targetDirectory));
        }
        return false;
    }
    if (!QDir().mkpath(targetDirectory)) {
        if (result) {
            result->errors.push_back(QStringLiteral("创建目标目录失败: %1").arg(targetDirectory));
        }
        return false;
    }

    bool ok = true;
    const QFileInfoList entries = QDir(sourceDirectory).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::NoSort
    );
    for (const QFileInfo& entry : entries) {
        QString targetPath = QDir(targetDirectory).filePath(entry.fileName());
        const QFileInfo targetInfo(targetPath);
        if (targetInfo.exists()) {
            if (entry.isDir() && !entry.isSymLink() && targetInfo.isDir() && !targetInfo.isSymLink()) {
                if (!mergeMoveDirectoryContents(entry.absoluteFilePath(), targetPath, folderName, result)) {
                    ok = false;
                    break;
                }
                if (!removeEmptyDirectory(entry.absoluteFilePath(), result)) {
                    ok = false;
                    break;
                }
                continue;
            }
            targetPath = uniqueTargetPath(targetPath);
        }
        if (!movePathWithFallback(entry, targetPath, folderName, result)) {
            ok = false;
            break;
        }
    }
    if (ok && result) {
        result->affectedPaths.push_back(targetDirectory);
    }
    return ok;
}

bool FileManagementEngine::movePathWithFallback(const QFileInfo& sourceInfo, const QString& targetPath, const QString& folderName, FileOperationResult* result) const {
    const QString sourcePath = sourceInfo.absoluteFilePath();
    if (!QDir().mkpath(QFileInfo(targetPath).absolutePath())) {
        if (result) {
            result->errors.push_back(QStringLiteral("创建目标目录失败: %1").arg(QFileInfo(targetPath).absolutePath()));
        }
        return false;
    }

    const bool renamed = sourceInfo.isDir() && !sourceInfo.isSymLink()
        ? QDir().rename(sourcePath, targetPath)
        : QFile::rename(sourcePath, targetPath);
    if (renamed) {
        return true;
    }

    if (sourceInfo.isDir() && !sourceInfo.isSymLink()) {
        if (!mergeMoveDirectoryContents(sourcePath, targetPath, folderName, result)) {
            return false;
        }
        return removeEmptyDirectory(sourcePath, result);
    }

    if (QFileInfo::exists(targetPath)) {
        if (result) {
            result->errors.push_back(QStringLiteral("目标位置已存在同名项: %1").arg(targetPath));
        }
        return false;
    }
    if (!QFile::copy(sourcePath, targetPath)) {
        if (result) {
            const QString label = folderName.isEmpty()
                ? QStringLiteral("文件")
                : QStringLiteral("“%1”中的文件").arg(folderName);
            result->errors.push_back(QStringLiteral("移动%1 %2 失败: 无法复制到 %3").arg(label, sourceInfo.fileName(), targetPath));
        }
        return false;
    }
    if (!QFile::remove(sourcePath)) {
        QFile::remove(targetPath);
        if (result) {
            result->errors.push_back(QStringLiteral("移动文件失败，原文件可能正被占用: %1").arg(sourcePath));
        }
        return false;
    }
    return true;
}

bool FileManagementEngine::removeDirectoryTree(const QString& path, FileOperationResult* result) const {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;
    }
    if (isReparsePoint(path)) {
        return removeJunction(path, result);
    }
    if (!info.isDir()) {
        const bool removed = QFile::remove(path);
        if (!removed && result) {
            result->errors.push_back(QStringLiteral("删除文件失败: %1").arg(path));
        }
        return removed;
    }
    const bool removed = QDir(path).removeRecursively();
    if (!removed && result) {
        result->errors.push_back(QStringLiteral("删除目录失败: %1").arg(path));
    }
    return removed;
}

bool FileManagementEngine::removeEmptyDirectory(const QString& path, FileOperationResult* result) const {
    if (!QFileInfo::exists(path)) {
        return true;
    }
    if (isReparsePoint(path)) {
        return removeJunction(path, result);
    }
    if (QDir().rmdir(path)) {
        if (result) {
            result->affectedPaths.push_back(path);
        }
        return true;
    }
    if (result) {
        result->errors.push_back(QStringLiteral("无法移除目录 %1，目录可能非空或有文件被占用。").arg(path));
    }
    return false;
}

bool FileManagementEngine::rollbackMigration(const QString& targetDirectory, const QString& sourceDirectory) const {
    FileOperationResult ignored;
    QDir().mkpath(sourceDirectory);
    if (QFileInfo::exists(targetDirectory)) {
        mergeMoveDirectoryContents(targetDirectory, sourceDirectory, QString(), &ignored);
    }
    removeEmptyDirectory(targetDirectory, &ignored);
    return ignored.errors.isEmpty();
}

bool FileManagementEngine::ensureSupportsJunction(const QString& targetRoot, const QString& folderName, FileOperationResult* result) const {
#ifdef Q_OS_WIN
    QStorageInfo storage(targetRoot);
    storage.refresh();
    if (!storage.isValid()) {
        return true;
    }
    const QString fileSystem = QString::fromLatin1(storage.fileSystemType()).trimmed().toUpper();
    if (!fileSystem.isEmpty() && fileSystem != QStringLiteral("NTFS")) {
        if (result) {
            result->errors.push_back(QStringLiteral("目标磁盘为 %1 格式，不支持连接点，无法迁移“%2”。请选择 NTFS 格式的磁盘作为目标。").arg(fileSystem, folderName));
        }
        return false;
    }
#else
    Q_UNUSED(targetRoot);
    Q_UNUSED(folderName);
    Q_UNUSED(result);
#endif
    return true;
}

bool FileManagementEngine::updateMigrationRedirect(
    const MigrationFolder& folder,
    const QString& path,
    FileOperationResult* result
) const {
#ifdef Q_OS_WIN
    if (folder.key == QStringLiteral("temp") || folder.key == QStringLiteral("system_temp")) {
        const QString registryPath = folder.key == QStringLiteral("system_temp")
            ? QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment")
            : QStringLiteral("HKEY_CURRENT_USER\\Environment");
        QSettings environment(
            registryPath,
            QSettings::NativeFormat
        );
        environment.setValue(QStringLiteral("TEMP"), QDir::toNativeSeparators(path));
        environment.setValue(QStringLiteral("TMP"), QDir::toNativeSeparators(path));
        environment.sync();
        if (environment.status() != QSettings::NoError) {
            if (result) {
                result->errors.push_back(QStringLiteral("更新 TEMP/TMP 环境变量失败。"));
            }
            return false;
        }
        DWORD_PTR broadcastResult = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 3000, &broadcastResult);
        return true;
    }

    const QString valueName = personalFolderRegistryValue(folder.key);
    if (valueName.isEmpty()) {
        return true;
    }
    const QString nativePath = QDir::toNativeSeparators(path);
    QSettings userShell(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders"),
        QSettings::NativeFormat
    );
    QSettings shell(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders"),
        QSettings::NativeFormat
    );
    userShell.setValue(valueName, nativePath);
    shell.setValue(valueName, nativePath);
    userShell.sync();
    shell.sync();
    if (userShell.status() != QSettings::NoError || shell.status() != QSettings::NoError) {
        if (result) {
            result->errors.push_back(QStringLiteral("更新系统个人文件夹注册表路径失败: %1").arg(folder.name));
        }
        return false;
    }
    DWORD_PTR broadcastResult = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 3000, &broadcastResult);
#else
    Q_UNUSED(folder);
    Q_UNUSED(path);
    Q_UNUSED(result);
#endif
    return true;
}

QString FileManagementEngine::personalFolderRegistryValue(const QString& folderKey) {
    if (folderKey == QStringLiteral("desktop")) {
        return QStringLiteral("Desktop");
    }
    if (folderKey == QStringLiteral("documents")) {
        return QStringLiteral("Personal");
    }
    if (folderKey == QStringLiteral("downloads")) {
        return QStringLiteral("{374DE290-123F-4565-9164-39C4925E467B}");
    }
    if (folderKey == QStringLiteral("pictures")) {
        return QStringLiteral("My Pictures");
    }
    if (folderKey == QStringLiteral("videos")) {
        return QStringLiteral("My Video");
    }
    return {};
}

QString FileManagementEngine::junctionTarget(const QString& path) const {
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (handle != INVALID_HANDLE_VALUE) {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
        CloseHandle(handle);
        if (length > 0 && length < buffer.size()) {
            QString target = QString::fromWCharArray(buffer.data(), static_cast<int>(length));
            if (target.startsWith(QStringLiteral("\\\\?\\UNC\\"))) {
                target = QStringLiteral("\\\\") + target.mid(8);
            } else if (target.startsWith(QStringLiteral("\\\\?\\"))) {
                target = target.mid(4);
            }
            return QDir::cleanPath(QDir::fromNativeSeparators(target));
        }
    }
#endif
    return QFileInfo(path).symLinkTarget();
}

bool FileManagementEngine::removeJunction(const QString& path, FileOperationResult* result) const {
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    if (RemoveDirectoryW(reinterpret_cast<LPCWSTR>(nativePath.utf16()))) {
        if (result) {
            result->affectedPaths.push_back(path);
        }
        return true;
    }
    if (result) {
        result->errors.push_back(windowsErrorMessage(QStringLiteral("删除 junction"), GetLastError()));
    }
    return false;
#else
    if (QFile::remove(path) || QDir().rmdir(path)) {
        if (result) {
            result->affectedPaths.push_back(path);
        }
        return true;
    }
    if (result) {
        result->errors.push_back(QStringLiteral("删除连接失败: %1").arg(path));
    }
    return false;
#endif
}

FileOperationResult FileManagementEngine::copyOrMove(const QStringList& paths, const QString& targetDirectory, bool move) const {
    FileOperationResult result;
    QDir().mkpath(targetDirectory);
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (!info.exists()) {
            result.errors.push_back(QStringLiteral("路径不存在: %1").arg(path));
            continue;
        }
        const QString target = QDir(targetDirectory).filePath(info.fileName());
        bool ok = false;
        if (move) {
            ok = info.isDir() && !info.isSymLink()
                ? QDir().rename(path, target)
                : QFile::rename(path, target);
            if (!ok && info.isDir() && !info.isSymLink()) {
                ok = copyDirectoryContents(path, target, &result) && removeDirectoryTree(path, &result);
            } else if (!ok && info.isFile() && !QFileInfo::exists(target) && QFile::copy(path, target)) {
                ok = QFile::remove(path);
            }
        } else if (info.isFile()) {
            ok = QFile::copy(path, target);
        } else if (info.isDir()) {
            QDir().mkpath(target);
            ok = true;
            QDirIterator iterator(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                const QString sourceFile = iterator.next();
                const QString relative = QDir(path).relativeFilePath(sourceFile);
                const QString targetFile = QDir(target).filePath(relative);
                QDir().mkpath(QFileInfo(targetFile).absolutePath());
                if (!QFile::copy(sourceFile, targetFile)) {
                    ok = false;
                    result.errors.push_back(QStringLiteral("复制失败: %1").arg(sourceFile));
                }
            }
        }
        if (ok) {
            result.affectedPaths.push_back(target);
        } else {
            result.errors.push_back(QStringLiteral("%1失败: %2").arg(move ? QStringLiteral("移动") : QStringLiteral("复制"), path));
        }
    }
    return result;
}

FileOperationResult FileManagementEngine::runCommand(const QString& executable, const QStringList& arguments) const {
    QProcess process;
    process.start(executable, arguments);
    process.waitForFinished(-1);
    FileOperationResult result;
    result.output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString error = QString::fromLocal8Bit(process.readAllStandardError());
    if (!error.isEmpty()) {
        result.output += QStringLiteral("\n") + error;
    }
    if (process.exitCode() != 0) {
        result.errors.push_back(result.output.trimmed().isEmpty()
            ? QStringLiteral("命令执行失败: %1").arg(executable)
            : result.output.trimmed());
    }
    return result;
}

MigrationFolder FileManagementEngine::migrationFolderByKey(const QString& folderKey) const {
    for (const MigrationFolder& folder : migrationCatalog()) {
        if (folder.key == folderKey) {
            return folder;
        }
    }
    return {};
}
