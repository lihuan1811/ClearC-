#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct InstalledApplication {
    QString name;
    QString publisher;
    QString version;
    QString installLocation;
    QString installDate;
    QString uninstallCommand;
    QString registryKey;
    QString packageFullName;
    qint64 sizeBytes = 0;
    bool storeApp = false;
};

struct UninstallResult {
    bool started = false;
    QString registryBackup;
    QStringList removedPaths;
    QStringList errors;
};

class SoftwareUninstallEngine {
public:
    QVector<InstalledApplication> installedApplications() const;
    QString backupRegistry(const InstalledApplication& app, QString* error = nullptr) const;
    UninstallResult startUninstall(const InstalledApplication& app) const;
    QStringList findResidualPaths(const InstalledApplication& app) const;
    UninstallResult cleanResiduals(const InstalledApplication& app) const;

    static QString registryBackupRoot();
};
