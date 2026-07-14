#include "SoftwareUninstallEngine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace {

QString environmentPath(const char* name) {
    return QDir::toNativeSeparators(qEnvironmentVariable(name));
}

QString safeFileName(QString value) {
    value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    value = value.trimmed();
    return value.isEmpty() ? QStringLiteral("application") : value.left(80);
}

QString normalizedAppFolder(QString name) {
    name.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9\\x{4e00}-\\x{9fff}._ -]")));
    return name.trimmed();
}

bool removePath(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        return true;
    }
    return info.isDir() && !info.isSymLink()
        ? QDir(path).removeRecursively()
        : QFile::remove(path);
}

bool registryKeyExists(const QString& key) {
#ifdef Q_OS_WIN
    if (key.isEmpty()) {
        return false;
    }
    QProcess process;
    process.start(QStringLiteral("reg"), {QStringLiteral("query"), key});
    return process.waitForFinished(10000) && process.exitCode() == 0;
#else
    Q_UNUSED(key);
    return false;
#endif
}

bool isProtectedResidualPath(const QString& path) {
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (normalized.isEmpty() || QDir(normalized).isRoot()) {
        return true;
    }
    const QStringList protectedPaths = {
        qEnvironmentVariable("SystemDrive"),
        qEnvironmentVariable("SystemRoot"),
        qEnvironmentVariable("ProgramFiles"),
        qEnvironmentVariable("ProgramFiles(x86)"),
        qEnvironmentVariable("ProgramData"),
        qEnvironmentVariable("USERPROFILE"),
        qEnvironmentVariable("LOCALAPPDATA"),
        qEnvironmentVariable("APPDATA"),
    };
    for (const QString& protectedPath : protectedPaths) {
        if (!protectedPath.isEmpty()
            && normalized.compare(QDir::cleanPath(QFileInfo(protectedPath).absoluteFilePath()), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

QVector<InstalledApplication> SoftwareUninstallEngine::installedApplications() const {
    QVector<InstalledApplication> applications;
#ifdef Q_OS_WIN
    const QStringList roots = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
    };
    for (const QString& root : roots) {
        QSettings settings(root, QSettings::NativeFormat);
        for (const QString& group : settings.childGroups()) {
            settings.beginGroup(group);
            const QString name = settings.value(QStringLiteral("DisplayName")).toString().trimmed();
            const bool hidden = settings.value(QStringLiteral("SystemComponent")).toInt() == 1;
            if (!name.isEmpty() && !hidden) {
                InstalledApplication app;
                app.name = name;
                app.publisher = settings.value(QStringLiteral("Publisher")).toString();
                app.version = settings.value(QStringLiteral("DisplayVersion")).toString();
                app.installLocation = QDir::toNativeSeparators(settings.value(QStringLiteral("InstallLocation")).toString());
                app.installDate = settings.value(QStringLiteral("InstallDate")).toString();
                app.uninstallCommand = settings.value(QStringLiteral("QuietUninstallString")).toString();
                if (app.uninstallCommand.isEmpty()) {
                    app.uninstallCommand = settings.value(QStringLiteral("UninstallString")).toString();
                }
                app.registryKey = root + QLatin1Char('\\') + group;
                app.sizeBytes = settings.value(QStringLiteral("EstimatedSize")).toLongLong() * 1024;
                applications.push_back(app);
            }
            settings.endGroup();
        }
    }

    QProcess appx;
    appx.start(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-Command"),
            QStringLiteral("Get-AppxPackage | Select-Object Name,Publisher,Version,PackageFullName,InstallLocation | ConvertTo-Json -Compress")
        }
    );
    if (appx.waitForFinished(20000) && appx.exitCode() == 0) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(appx.readAllStandardOutput(), &error);
        QJsonArray rows;
        if (error.error == QJsonParseError::NoError && document.isArray()) {
            rows = document.array();
        } else if (error.error == QJsonParseError::NoError && document.isObject()) {
            rows.push_back(document.object());
        }
        for (const QJsonValue& value : rows) {
            const QJsonObject object = value.toObject();
            InstalledApplication app;
            app.name = object.value(QStringLiteral("Name")).toString();
            app.publisher = object.value(QStringLiteral("Publisher")).toString();
            app.version = object.value(QStringLiteral("Version")).toString();
            app.installLocation = QDir::toNativeSeparators(object.value(QStringLiteral("InstallLocation")).toString());
            app.packageFullName = object.value(QStringLiteral("PackageFullName")).toString();
            if (app.name.isEmpty() || app.packageFullName.isEmpty()) {
                continue;
            }
            QString escaped = app.packageFullName;
            escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
            app.uninstallCommand = QStringLiteral("powershell -NoProfile -Command \"Get-AppxPackage -PackageFullName '%1' | Remove-AppxPackage\"").arg(escaped);
            app.storeApp = true;
            applications.push_back(app);
        }
    }
#endif
    std::sort(applications.begin(), applications.end(), [](const InstalledApplication& left, const InstalledApplication& right) {
        if (left.sizeBytes != right.sizeBytes) {
            return left.sizeBytes > right.sizeBytes;
        }
        return left.name.localeAwareCompare(right.name) < 0;
    });
    return applications;
}

QString SoftwareUninstallEngine::backupRegistry(const InstalledApplication& app, QString* error) const {
    if (app.registryKey.isEmpty() || !registryKeyExists(app.registryKey)) {
        return {};
    }
    const QString root = registryBackupRoot();
    if (!QDir().mkpath(root)) {
        if (error) {
            *error = QStringLiteral("无法创建注册表备份目录: %1").arg(root);
        }
        return {};
    }
    const QString fileName = QStringLiteral("%1-%2.reg")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")), safeFileName(app.name));
    const QString destination = QDir(root).filePath(fileName);
#ifdef Q_OS_WIN
    QProcess process;
    process.start(QStringLiteral("reg"), {QStringLiteral("export"), app.registryKey, destination, QStringLiteral("/y")});
    if (!process.waitForFinished(15000) || process.exitCode() != 0) {
        if (error) {
            const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            *error = details.isEmpty() ? QStringLiteral("注册表备份失败: %1").arg(app.registryKey) : details;
        }
        return {};
    }
    return destination;
#else
    Q_UNUSED(error);
    return {};
#endif
}

UninstallResult SoftwareUninstallEngine::startUninstall(const InstalledApplication& app) const {
    UninstallResult result;
    if (app.uninstallCommand.trimmed().isEmpty()) {
        result.errors.push_back(QStringLiteral("没有可执行的卸载命令: %1").arg(app.name));
        return result;
    }
    QString backupError;
    result.registryBackup = backupRegistry(app, &backupError);
    if (registryKeyExists(app.registryKey) && result.registryBackup.isEmpty()) {
        result.errors.push_back(backupError);
        return result;
    }
#ifdef Q_OS_WIN
    result.started = QProcess::startDetached(QStringLiteral("cmd.exe"), {QStringLiteral("/C"), app.uninstallCommand});
#else
    result.started = false;
#endif
    if (!result.started) {
        result.errors.push_back(QStringLiteral("无法启动卸载程序: %1").arg(app.name));
    }
    return result;
}

QStringList SoftwareUninstallEngine::findResidualPaths(const InstalledApplication& app) const {
    QStringList candidates;
    if (!app.installLocation.trimmed().isEmpty()) {
        candidates.push_back(QDir::toNativeSeparators(app.installLocation));
    }
    const QString folder = normalizedAppFolder(app.name);
    if (folder.size() >= 3) {
        const QStringList bases = {environmentPath("LOCALAPPDATA"), environmentPath("APPDATA"), environmentPath("ProgramData")};
        for (const QString& base : bases) {
            if (!base.isEmpty()) {
                candidates.push_back(QDir(base).filePath(folder));
            }
        }
    }
    QStringList existing;
    for (const QString& candidate : candidates) {
        const QString absolute = QFileInfo(candidate).absoluteFilePath();
        if (QFileInfo::exists(absolute)
            && !isProtectedResidualPath(absolute)
            && !existing.contains(absolute, Qt::CaseInsensitive)) {
            existing.push_back(absolute);
        }
    }
    return existing;
}

UninstallResult SoftwareUninstallEngine::cleanResiduals(const InstalledApplication& app) const {
    UninstallResult result;
    QString backupError;
    result.registryBackup = backupRegistry(app, &backupError);
    const bool registryPresent = registryKeyExists(app.registryKey);
    if (registryPresent && result.registryBackup.isEmpty()) {
        result.errors.push_back(backupError);
        return result;
    }

    for (const QString& path : findResidualPaths(app)) {
        if (removePath(path)) {
            result.removedPaths.push_back(path);
        } else {
            result.errors.push_back(QStringLiteral("残留目录删除失败: %1").arg(path));
        }
    }
#ifdef Q_OS_WIN
    if (registryPresent) {
        QProcess process;
        process.start(QStringLiteral("reg"), {QStringLiteral("delete"), app.registryKey, QStringLiteral("/f")});
        process.waitForFinished(15000);
        if (process.exitCode() != 0) {
            result.errors.push_back(QStringLiteral("卸载注册表项删除失败: %1").arg(app.registryKey));
        }
    }

    const QStringList runKeys = {
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run"),
    };
    for (const QString& key : runKeys) {
        QSettings settings(key, QSettings::NativeFormat);
        for (const QString& valueName : settings.childKeys()) {
            const QString value = settings.value(valueName).toString();
            const bool pointsIntoInstallLocation = !app.installLocation.isEmpty()
                && !isProtectedResidualPath(app.installLocation)
                && value.contains(app.installLocation, Qt::CaseInsensitive);
            const bool matchesSpecificName = app.name.size() >= 4
                && valueName.contains(app.name, Qt::CaseInsensitive);
            if (pointsIntoInstallLocation || matchesSpecificName) {
                settings.remove(valueName);
            }
        }
    }

    const QStringList shortcutRoots = {
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation),
        environmentPath("ProgramData") + QStringLiteral("\\Microsoft\\Windows\\Start Menu\\Programs"),
    };
    for (const QString& root : shortcutRoots) {
        if (root.isEmpty() || !QFileInfo(root).isDir()) {
            continue;
        }
        QDirIterator iterator(root, {QStringLiteral("*.lnk")}, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            if (app.name.size() >= 4
                && QFileInfo(path).completeBaseName().contains(app.name, Qt::CaseInsensitive)
                && QFile::remove(path)) {
                result.removedPaths.push_back(path);
            }
        }
    }
#endif
    result.started = true;
    return result;
}

QString SoftwareUninstallEngine::registryBackupRoot() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base.isEmpty() ? QDir::homePath() : base).filePath(QStringLiteral("registry_backups"));
}
