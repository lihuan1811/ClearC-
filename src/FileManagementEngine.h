#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

enum class ManagedFileType {
    All,
    Video,
    Image,
    Installer,
    Archive,
    Document,
};

struct ManagedFileEntry {
    QString path;
    QString name;
    qint64 sizeBytes = 0;
    ManagedFileType type = ManagedFileType::All;
};

struct FolderUsageEntry {
    QString path;
    qint64 sizeBytes = 0;
    int fileCount = 0;
};

struct FileUsageEntry {
    QString path;
    QString extension;
    qint64 sizeBytes = 0;
    int fileCount = 1;
};

struct ExtensionUsageEntry {
    QString extension;
    QString description;
    qint64 sizeBytes = 0;
    int fileCount = 0;
};

struct FolderUsageScan {
    QVector<FolderUsageEntry> folders;
    QVector<FileUsageEntry> files;
    QVector<ExtensionUsageEntry> extensions;
};

struct EmptyFolderEntry {
    QString path;
};

struct MigrationFolder {
    QString key;
    QString name;
    QString subName;
    QString path;
    QString target;
    qint64 sizeBytes = 0;
    bool exists = false;
    bool migrated = false;
};

struct FileOperationResult {
    QStringList affectedPaths;
    QStringList errors;
    QString output;
    bool unsupported = false;
};

class FileManagementEngine {
public:
    QVector<ManagedFileEntry> listFiles(
        const QString& rootPath,
        ManagedFileType type = ManagedFileType::All,
        int limit = 500
    ) const;
    QVector<FolderUsageEntry> scanFolderUsage(const QString& rootPath, int limit = 50) const;
    FolderUsageScan scanFolderUsageDetailed(const QString& rootPath, int folderLimit = 80, int fileLimit = 60000) const;
    QVector<EmptyFolderEntry> scanEmptyFolders(const QString& rootPath, int limit = 1000) const;

    FileOperationResult copyFiles(const QStringList& paths, const QString& targetDirectory) const;
    FileOperationResult moveFiles(const QStringList& paths, const QString& targetDirectory) const;
    FileOperationResult renameFile(const QString& path, const QString& newName) const;
    FileOperationResult deleteFiles(const QStringList& paths) const;
    FileOperationResult shredFiles(const QStringList& paths) const;
    FileOperationResult createShortcut(const QString& target, const QString& shortcutPath) const;
    FileOperationResult repairFolderPermission(const QString& path) const;

    QVector<MigrationFolder> migrationCatalog() const;
    QVector<MigrationFolder> scanMigrationFolders() const;
    FileOperationResult migratePersonalFolder(const QString& folderKey, const QString& targetRoot, bool moveFiles = true) const;
    FileOperationResult restorePersonalFolder(const QString& folderKey) const;
    FileOperationResult createJunction(const QString& source, const QString& target) const;

    static ManagedFileType detectType(const QString& path);
    static QString typeLabel(ManagedFileType type);
    static QString normalizedExtension(const QString& path);
    static QString extensionDescription(const QString& extension);
    static qint64 directorySize(const QString& path, int* fileCount = nullptr);
    static bool isReparsePoint(const QString& path);

private:
    FileOperationResult copyOrMove(const QStringList& paths, const QString& targetDirectory, bool move) const;
    FileOperationResult runCommand(const QString& executable, const QStringList& arguments) const;
    MigrationFolder migrationFolderByKey(const QString& folderKey) const;
    bool copyDirectoryContents(const QString& sourceDirectory, const QString& targetDirectory, FileOperationResult* result) const;
    bool removeDirectoryTree(const QString& path, FileOperationResult* result) const;
    QString junctionTarget(const QString& path) const;
    bool removeJunction(const QString& path, FileOperationResult* result) const;
};
