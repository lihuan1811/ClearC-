#include "FileManagementEngine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <string>

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

QString uniqueSiblingBackupPath(const QString& path) {
    const QFileInfo info(path);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss"));
    const QString base = QDir(info.absolutePath()).filePath(
        QStringLiteral("%1.c_diskglow_original_%2").arg(info.fileName(), stamp)
    );
    QString candidate = base;
    int counter = 1;
    while (QFileInfo::exists(candidate)) {
        candidate = QStringLiteral("%1_%2").arg(base).arg(counter);
        ++counter;
    }
    return candidate;
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

    QDirIterator iterator(rootPath, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext() && files.size() < limit) {
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
        files.push_back(entry);
    }

    std::sort(files.begin(), files.end(), [](const ManagedFileEntry& a, const ManagedFileEntry& b) {
        return a.sizeBytes > b.sizeBytes;
    });
    return files;
}

QVector<FolderUsageEntry> FileManagementEngine::scanFolderUsage(const QString& rootPath, int limit) const {
    QVector<FolderUsageEntry> entries;
    QDir root(rootPath);
    if (!root.exists()) {
        return entries;
    }

    for (const QFileInfo& info : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        int fileCount = 0;
        FolderUsageEntry entry;
        entry.path = info.absoluteFilePath();
        entry.sizeBytes = directorySize(entry.path, &fileCount);
        entry.fileCount = fileCount;
        entries.push_back(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const FolderUsageEntry& a, const FolderUsageEntry& b) {
        return a.sizeBytes > b.sizeBytes;
    });
    if (entries.size() > limit) {
        entries.resize(limit);
    }
    return entries;
}

QVector<EmptyFolderEntry> FileManagementEngine::scanEmptyFolders(const QString& rootPath, int limit) const {
    QVector<EmptyFolderEntry> folders;
    if (!QFileInfo(rootPath).isDir()) {
        return folders;
    }

    QDirIterator iterator(rootPath, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext() && folders.size() < limit) {
        const QString path = iterator.next();
        QDir dir(path);
        if (dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
            folders.push_back({path});
        }
    }
    std::sort(folders.begin(), folders.end(), [](const EmptyFolderEntry& a, const EmptyFolderEntry& b) {
        return a.path.length() > b.path.length();
    });
    return folders;
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
    const QByteArray zeros(8192, '\0');
    for (const QString& path : paths) {
        QFile file(path);
        if (!file.exists()) {
            continue;
        }
        const qint64 length = QFileInfo(path).size();
        if (!file.open(QIODevice::WriteOnly)) {
            result.errors.push_back(QStringLiteral("无法打开文件粉碎: %1").arg(path));
            continue;
        }
        qint64 written = 0;
        while (written < length) {
            const qint64 count = qMin<qint64>(zeros.size(), length - written);
            if (file.write(zeros.constData(), count) != count) {
                break;
            }
            written += count;
        }
        file.close();
        if (QFile::remove(path)) {
            result.affectedPaths.push_back(path);
        } else {
            result.errors.push_back(QStringLiteral("粉碎后删除失败: %1").arg(path));
        }
    }
    return result;
}

FileOperationResult FileManagementEngine::createShortcut(const QString& target, const QString& shortcutPath) const {
#ifdef Q_OS_WIN
    const QString script = QStringLiteral("$s=New-Object -ComObject WScript.Shell; $l=$s.CreateShortcut('%1'); $l.TargetPath='%2'; $l.Save()")
        .arg(shortcutPath, target);
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
    return {
        {QStringLiteral("desktop"), QStringLiteral("桌面"), QStringLiteral("Desktop"), joinPath(home, {QStringLiteral("Desktop")})},
        {QStringLiteral("documents"), QStringLiteral("我的文档"), QStringLiteral("Documents"), joinPath(home, {QStringLiteral("Documents")})},
        {QStringLiteral("favorites"), QStringLiteral("收藏夹"), QStringLiteral("Favorites"), joinPath(home, {QStringLiteral("Favorites")})},
        {QStringLiteral("inetcache"), QStringLiteral("IE缓存"), QStringLiteral("INetCache"), joinPath(local, {QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("INetCache")})},
        {QStringLiteral("cookies"), QStringLiteral("Cookies"), QStringLiteral("INetCookies"), joinPath(local, {QStringLiteral("Microsoft"), QStringLiteral("Windows"), QStringLiteral("INetCookies")})},
        {QStringLiteral("temp"), QStringLiteral("临时文件"), QStringLiteral("Temp"), joinPath(local, {QStringLiteral("Temp")})},
        {QStringLiteral("contacts"), QStringLiteral("联系人"), QStringLiteral("Contacts"), joinPath(home, {QStringLiteral("Contacts")})},
        {QStringLiteral("downloads"), QStringLiteral("下载"), QStringLiteral("Downloads"), joinPath(home, {QStringLiteral("Downloads")})},
        {QStringLiteral("links"), QStringLiteral("链接"), QStringLiteral("Links"), joinPath(home, {QStringLiteral("Links")})},
        {QStringLiteral("searches"), QStringLiteral("搜索"), QStringLiteral("Searches"), joinPath(home, {QStringLiteral("Searches")})},
        {QStringLiteral("videos"), QStringLiteral("我的视频"), QStringLiteral("Videos"), joinPath(home, {QStringLiteral("Videos")})},
        {QStringLiteral("pictures"), QStringLiteral("我的图片"), QStringLiteral("Pictures"), joinPath(home, {QStringLiteral("Pictures")})},
        {QStringLiteral("music"), QStringLiteral("我的音乐"), QStringLiteral("Music"), joinPath(home, {QStringLiteral("Music")})},
        {QStringLiteral("savedgames"), QStringLiteral("保存的游戏"), QStringLiteral("Saved Games"), joinPath(home, {QStringLiteral("Saved Games")})},
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

    const QString target = QDir(targetRoot).filePath(folder.subName);
    if (isSameOrChild(target, folder.path)) {
        result.errors.push_back(QStringLiteral("目标目录不能位于原目录内部: %1").arg(target));
        return result;
    }
    if (!QDir().mkpath(target)) {
        result.errors.push_back(QStringLiteral("创建目标目录失败: %1").arg(target));
        return result;
    }

    const QFileInfo sourceInfo(folder.path);
    if (sourceInfo.exists()) {
        if (!sourceInfo.isDir()) {
            result.errors.push_back(QStringLiteral("原路径不是目录: %1").arg(folder.path));
            return result;
        }
        if (!copyDirectoryContents(folder.path, target, &result)) {
            return result;
        }

        if (moveFiles) {
            if (!removeDirectoryTree(folder.path, &result)) {
                return result;
            }
        } else {
            const QString preservedPath = uniqueSiblingBackupPath(folder.path);
            if (!QDir().rename(folder.path, preservedPath)) {
                result.errors.push_back(QStringLiteral("保留原目录备份失败: %1 -> %2").arg(folder.path, preservedPath));
                return result;
            }
            result.output += QStringLiteral("原目录已保留为: %1\n").arg(QDir::toNativeSeparators(preservedPath));
        }
    } else {
        QDir().mkpath(QFileInfo(folder.path).absolutePath());
    }

    FileOperationResult junction = createJunction(folder.path, target);
    if (!junction.errors.isEmpty()) {
        result.errors.append(junction.errors);
        return result;
    }
    result.affectedPaths.append(junction.affectedPaths);
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
        return result;
    }
    if (!target.isEmpty() && QFileInfo::exists(target)) {
        if (!copyDirectoryContents(target, folder.path, &result)) {
            return result;
        }
        if (!removeDirectoryTree(target, &result)) {
            return result;
        }
    }
    result.affectedPaths.push_back(folder.path);
    return result;
}

FileOperationResult FileManagementEngine::createJunction(const QString& source, const QString& target) const {
#ifdef Q_OS_WIN
    return runCommand(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("mklink"), QStringLiteral("/J"), source, target});
#else
    FileOperationResult result;
    if (QFile::link(target, source)) {
        result.affectedPaths.push_back(source);
    } else {
        result.errors.push_back(QStringLiteral("创建连接失败: %1 -> %2").arg(source, target));
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

qint64 FileManagementEngine::directorySize(const QString& path, int* fileCount) {
    qint64 total = 0;
    if (fileCount) {
        *fileCount = 0;
    }
    QDirIterator iterator(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
        if (fileCount) {
            ++(*fileCount);
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
    QDirIterator iterator(sourceDirectory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
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
