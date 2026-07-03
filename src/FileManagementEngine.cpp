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
        folder.target = folder.migrated ? QFileInfo(folder.path).symLinkTarget() : QString();
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
    QDir().mkpath(targetRoot);
    if (QFileInfo::exists(folder.path) && moveFiles) {
        const FileOperationResult moved = copyOrMove({folder.path}, targetRoot, true);
        if (!moved.errors.isEmpty()) {
            return moved;
        }
    } else {
        QDir().mkpath(target);
    }
    FileOperationResult junction = createJunction(folder.path, target);
    if (junction.errors.isEmpty()) {
        junction.affectedPaths.push_back(folder.path);
    }
    return junction;
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
    const QString target = QFileInfo(folder.path).symLinkTarget();
    QDir(folder.path).removeRecursively();
    QDir().mkpath(folder.path);
    if (!target.isEmpty() && QFileInfo::exists(target)) {
        FileOperationResult moved = copyOrMove({target}, QFileInfo(folder.path).absolutePath(), true);
        moved.affectedPaths.push_back(folder.path);
        return moved;
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
