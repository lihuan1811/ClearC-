#pragma once

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QStorageInfo>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <initializer_list>

struct DiskInfo {
    qint64 totalBytes = 0;
    qint64 freeBytes = 0;
    qint64 usedBytes = 0;
};

struct CleanupRule {
    QString id;
    QString title;
    QStringList paths;
    QStringList patterns;
    QStringList pathContains;
    bool scanOnly = false;
    bool aggregate = true;
    bool recommended = true;
    bool professional = true;
    bool privacySensitive = false;
};

struct CleanupEntry {
    QString ruleId;
    QString title;
    QString path;
    QStringList files;
    qint64 size = 0;
    bool scanOnly = false;
    bool recommended = true;
    bool professional = true;
    bool privacySensitive = false;
};

struct CleanupScanResult {
    QVector<CleanupEntry> entries;
    qint64 totalBytes = 0;
    qint64 recommendedBytes = 0;
    qint64 professionalBytes = 0;
    int scannedCount = 0;
};

struct FileEntry {
    QString name;
    QString path;
    qint64 size = 0;
    QString digest;
};

struct CleanOptions {
    bool simulate = false;
    bool backup = true;
    bool allowScanOnly = false;
    QString backupRoot;
};

struct CleanResult {
    qint64 cleanedBytes = 0;
    int attemptedCount = 0;
    int deletedCount = 0;
    int skippedCount = 0;
    QStringList errors;
};

struct BackupRecord {
    QString id;
    QString sourcePath;
    QString backupPath;
    qint64 size = 0;
    QDateTime createdAt;
};

struct BackupInfo {
    QString backupRoot;
    QVector<BackupRecord> backups;
    qint64 totalBytes = 0;
};

class CleanupEngine {
public:
    enum class CleanMode {
        Recommended,
        Professional,
        SelectAll,
    };

    using ProgressCallback = std::function<void(const QString& path, int count)>;

    CleanupEngine();

    DiskInfo diskInfo() const;
    CleanupScanResult scanSystem(const ProgressCallback& progress = {});
    QVector<CleanupEntry> entriesForMode(
        const QVector<CleanupEntry>& entries,
        CleanMode mode
    ) const;
    qint64 cleanEntries(
        const QVector<CleanupEntry>& entries,
        const CleanOptions& options,
        const ProgressCallback& progress = {}
    ) const;
    CleanResult cleanEntriesDetailed(
        const QVector<CleanupEntry>& entries,
        const CleanOptions& options,
        const ProgressCallback& progress = {}
    ) const;

    static QVector<CleanupRule> cleanupRules();
    static QString formatSize(qint64 bytes);
    static QVector<FileEntry> scanLargeFilesAsync(
        const QString& root,
        qint64 minSize = 100 * 1024 * 1024,
        int maxFiles = 50000
    );
    static QVector<QVector<FileEntry>> scanDuplicateFilesAsync(
        const QString& root,
        int maxFiles = 50000
    );
    static bool deletePath(const QString& path, QString* error = nullptr);
    static QString backupRoot();
    static BackupInfo backupInfo(const QString& backupRoot = {});
    static bool restoreBackupItem(const BackupRecord& record, QString* error = nullptr);
    static bool deleteBackupItem(const BackupRecord& record, QString* error = nullptr);
    static bool pruneBackups(const QString& backupRoot = {}, int maxBackups = 5, qint64 maxBytes = 1024LL * 1024LL * 1024LL);

private:
    static QString envPath(const QString& name, const QString& fallback);
    static QString winJoin(std::initializer_list<QString> parts);
    static bool pathMatches(const QString& path, const CleanupRule& rule);
    static qint64 fileSize(const QString& path);
    static QString fileDigest(const QString& path);
    static void collectRuleFiles(
        const CleanupRule& rule,
        const QString& rootPath,
        QStringList* files,
        qint64* totalSize,
        const ProgressCallback& progress,
        int* count
    );
    static bool backupFile(const QString& source, const QString& backupRoot, QString* error = nullptr);
    static QString defaultBackupRoot();
};
