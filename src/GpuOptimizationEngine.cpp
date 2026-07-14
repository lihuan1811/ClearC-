#include "GpuOptimizationEngine.h"
#include "AmdAdlxBridge.h"
#include "NvidiaNvapiBridge.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace {

QString runProcess(const QString& executable, const QStringList& arguments, int* exitCode = nullptr) {
    QProcess process;
    process.start(executable, arguments);
    if (!process.waitForStarted(5000)) {
        if (exitCode) {
            *exitCode = -1;
        }
        return QStringLiteral("命令启动失败: %1").arg(process.errorString());
    }
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished();
        if (exitCode) {
            *exitCode = -1;
        }
        return QStringLiteral("命令执行超时: %1").arg(executable);
    }
    if (exitCode) {
        *exitCode = process.exitCode();
    }
    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString error = QString::fromLocal8Bit(process.readAllStandardError());
    if (!error.isEmpty()) {
        output += QStringLiteral("\n") + error;
    }
    return output.trimmed();
}

QString vendorFromName(const QString& name) {
    const QString lowered = name.toLower();
    if (lowered.contains(QStringLiteral("nvidia"))) {
        return QStringLiteral("NVIDIA");
    }
    if (lowered.contains(QStringLiteral("amd")) || lowered.contains(QStringLiteral("radeon"))) {
        return QStringLiteral("AMD");
    }
    if (lowered.contains(QStringLiteral("intel"))) {
        return QStringLiteral("Intel");
    }
    return QStringLiteral("Unknown");
}

qint64 adapterRamToMB(const QString& value) {
    bool ok = false;
    const qulonglong bytes = value.trimmed().toULongLong(&ok);
    if (!ok || bytes <= 0) {
        return -1;
    }
    return static_cast<qint64>(bytes / (1024 * 1024));
}

QString programFilesPath(const QString& envName, const QString& fallback) {
    const QString value = qEnvironmentVariable(envName.toLocal8Bit().constData());
    return value.isEmpty() ? fallback : value;
}

bool hasVendor(const QVector<GpuDeviceInfo>& devices, const QString& vendor) {
    for (const GpuDeviceInfo& device : devices) {
        if (device.vendor == vendor) {
            return true;
        }
    }
    return false;
}

QString powerBackupKey(const QString& actionId) {
    return QStringLiteral("gpu/power_scheme_backup/%1").arg(actionId);
}

bool captureActivePowerScheme(const QString& actionId, QString* error) {
#ifdef Q_OS_WIN
    QSettings settings;
    const QString key = powerBackupKey(actionId);
    if (settings.contains(key)) {
        return true;
    }
    int exitCode = 0;
    const QString output = runProcess(QStringLiteral("powercfg"), {QStringLiteral("/getactivescheme")}, &exitCode);
    const QRegularExpression pattern(QStringLiteral("([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})"));
    const QRegularExpressionMatch match = pattern.match(output);
    if (exitCode != 0 || !match.hasMatch()) {
        if (error) {
            *error = QStringLiteral("无法读取当前电源计划，已停止显卡优化: %1").arg(output);
        }
        return false;
    }
    settings.setValue(key, match.captured(1));
    return true;
#else
    Q_UNUSED(actionId);
    if (error) *error = QStringLiteral("仅 Windows 支持电源计划");
    return false;
#endif
}

QString restoreActivePowerScheme(const QString& actionId, int* exitCode) {
#ifdef Q_OS_WIN
    QSettings settings;
    const QString key = powerBackupKey(actionId);
    const QString guid = settings.value(key).toString();
    if (guid.isEmpty()) {
        if (exitCode) *exitCode = 0;
        return QStringLiteral("原电源计划已处于还原状态");
    }
    int code = 0;
    const QString output = runProcess(QStringLiteral("powercfg"), {QStringLiteral("/setactive"), guid}, &code);
    if (code == 0) {
        settings.remove(key);
    }
    if (exitCode) *exitCode = code;
    return output;
#else
    Q_UNUSED(actionId);
    if (exitCode) *exitCode = -1;
    return QStringLiteral("仅 Windows 支持电源计划");
#endif
}

}  // namespace

QVector<GpuDeviceInfo> GpuOptimizationEngine::detectDevices() const {
    QVector<GpuDeviceInfo> devices = queryVideoControllers();
    const QVector<GpuDeviceInfo> nvidiaDevices = queryNvidiaSmi();
    const bool nvapi = nvapiAvailable();
    const bool adlx = adlxAvailable();
    const bool amdSoftware = amdSoftwareAvailable();

    for (const GpuDeviceInfo& nvidia : nvidiaDevices) {
        bool merged = false;
        for (GpuDeviceInfo& existing : devices) {
            if (existing.vendor == QStringLiteral("NVIDIA")
                && (existing.name.contains(nvidia.name, Qt::CaseInsensitive)
                    || nvidia.name.contains(existing.name, Qt::CaseInsensitive))) {
                existing.temperatureC = nvidia.temperatureC;
                existing.loadPercent = nvidia.loadPercent;
                existing.memoryMB = nvidia.memoryMB > 0 ? nvidia.memoryMB : existing.memoryMB;
                existing.driverVersion = nvidia.driverVersion.isEmpty() ? existing.driverVersion : nvidia.driverVersion;
                existing.nvidiaSmiAvailable = true;
                merged = true;
                break;
            }
        }
        if (!merged) {
            devices.push_back(nvidia);
        }
    }

    for (GpuDeviceInfo& device : devices) {
        if (device.vendor == QStringLiteral("NVIDIA")) {
            device.nvidiaSmiAvailable = device.nvidiaSmiAvailable || !nvidiaSmiPath().isEmpty();
            device.nvapiAvailable = nvapi;
            if (device.nvidiaSmiAvailable) {
                device.capabilities.push_back(QStringLiteral("nvidia-smi"));
            }
            if (device.nvapiAvailable) {
                device.capabilities.push_back(QStringLiteral("NVAPI"));
            }
        } else if (device.vendor == QStringLiteral("AMD")) {
            device.adlxAvailable = adlx;
            device.amdSoftwareAvailable = amdSoftware;
            if (device.adlxAvailable) {
                device.capabilities.push_back(QStringLiteral("ADLX"));
            }
            if (device.amdSoftwareAvailable) {
                device.capabilities.push_back(QStringLiteral("AMDSoftware"));
            }
        } else if (device.vendor == QStringLiteral("Intel")) {
            device.capabilities.push_back(QStringLiteral("Intel"));
        }
    }
    return devices;
}

QVector<GpuOptimizationAction> GpuOptimizationEngine::supportedActions(const QVector<GpuDeviceInfo>& devices) const {
#ifdef Q_OS_WIN
    const bool windowsHost = true;
#else
    const bool windowsHost = false;
#endif
    const bool nvidia = hasVendor(devices, QStringLiteral("NVIDIA"));
    const bool amd = hasVendor(devices, QStringLiteral("AMD"));
    bool nvidiaSmi = false;
    bool nvapi = false;
    bool adlx = false;
    bool amdSoftware = false;
    for (const GpuDeviceInfo& device : devices) {
        nvidiaSmi = nvidiaSmi || device.nvidiaSmiAvailable;
        nvapi = nvapi || device.nvapiAvailable;
        adlx = adlx || device.adlxAvailable;
        amdSoftware = amdSoftware || device.amdSoftwareAvailable;
    }

    QVector<GpuOptimizationAction> actions;
    auto add = [&actions](
        const QString& id,
        const QString& vendor,
        const QString& title,
        const QString& description,
        const QString& risk,
        bool supported,
        bool modifies,
        const QVector<WindowsOptimizationCommand>& commands,
        const QVector<WindowsOptimizationCommand>& revertCommands = {}
    ) {
        if (!supported) {
            return;
        }
        GpuOptimizationAction action;
        action.id = id;
        action.vendor = vendor;
        action.title = title;
        action.description = description;
        action.riskLabel = risk;
        action.supported = supported;
        action.modifiesSystem = modifies;
        action.requiresConfirmation = modifies;
        action.commands = commands;
        action.revertCommands = revertCommands;
        actions.push_back(action);
    };

    add(
        QStringLiteral("nvidia_one_click"),
        QStringLiteral("NVIDIA"),
        QStringLiteral("NVIDIA 一键调优"),
        QStringLiteral("通过官方 NVAPI 保存并设置全局最高性能和关闭垂直同步，同时切换高性能电源。低延迟模式不在官方 NVAPI SDK 的可修改范围内，需在 NVIDIA 控制面板手动设置。"),
        QStringLiteral("谨慎"),
        windowsHost && nvidia && nvapi,
        true,
        {
            {QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_MIN")}, QStringLiteral("启用最高性能电源")},
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start nvcplui.exe")}, QStringLiteral("打开 NVIDIA 官方控制面板")},
        },
        {
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start nvcplui.exe")}, QStringLiteral("打开 NVIDIA 控制面板确认还原结果")},
        }
    );
    add(
        QStringLiteral("amd_one_click"),
        QStringLiteral("AMD"),
        QStringLiteral("AMD 一键调优"),
        QStringLiteral("通过 AMD 官方 ADLX 保存并关闭垂直同步和帧率目标限制，同时切换高性能电源；只在当前驱动实际支持这些接口时显示。"),
        QStringLiteral("谨慎"),
        windowsHost && amd && adlx,
        true,
        {
            {QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_MIN")}, QStringLiteral("启用全局性能电源")},
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start amd-software:")}, QStringLiteral("打开 AMD 官方控制面板")},
        },
        {
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start amd-software:")}, QStringLiteral("打开 AMD Software 确认还原结果")},
        }
    );
    add(
        QStringLiteral("nvidia_official_panel"),
        QStringLiteral("NVIDIA"),
        QStringLiteral("打开 NVIDIA 官方控制面板"),
        QStringLiteral("当前驱动未提供所需 NVAPI 修改接口时，仅提供官方控制面板入口，不承诺自动调优。"),
        QStringLiteral("只读"),
        windowsHost && nvidia && !nvapi && nvidiaSmi,
        false,
        {
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start nvcplui.exe")}, QStringLiteral("打开 NVIDIA 官方控制面板")},
        }
    );
    add(
        QStringLiteral("amd_official_panel"),
        QStringLiteral("AMD"),
        QStringLiteral("打开 AMD Software"),
        QStringLiteral("当前驱动未提供可修改的 ADLX 3D 接口时，仅提供 AMD 官方入口。"),
        QStringLiteral("只读"),
        windowsHost && amd && !adlx && amdSoftware,
        false,
        {
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start amd-software:")}, QStringLiteral("打开 AMD Software")},
        }
    );

    return actions;
}

QString GpuOptimizationEngine::runAction(const GpuOptimizationAction& action, int* exitCode) const {
    QStringList output;
    if (action.id == QStringLiteral("nvidia_one_click") || action.id == QStringLiteral("amd_one_click")) {
        QString captureError;
        if (!captureActivePowerScheme(action.id, &captureError)) {
            if (exitCode) *exitCode = -1;
            return captureError;
        }
        int bridgeExit = 0;
        output.push_back(action.id == QStringLiteral("nvidia_one_click")
            ? NvidiaNvapiBridge::applyGlobalPerformanceSettings(&bridgeExit)
            : AmdAdlxBridge::applyGlobalPerformanceSettings(&bridgeExit));
        if (bridgeExit != 0) {
            if (exitCode) *exitCode = bridgeExit;
            return output.join(QStringLiteral("\n"));
        }
    }
    int commandExit = 0;
    output.push_back(SystemCatalog::runActionCommands(action.commands, &commandExit));
    if (exitCode) *exitCode = commandExit;
    return output.join(QStringLiteral("\n"));
}

QString GpuOptimizationEngine::restoreAction(const GpuOptimizationAction& action, int* exitCode) const {
    QStringList output;
    int overallExit = 0;
    if (action.id == QStringLiteral("nvidia_one_click") || action.id == QStringLiteral("amd_one_click")) {
        int bridgeExit = 0;
        output.push_back(action.id == QStringLiteral("nvidia_one_click")
            ? NvidiaNvapiBridge::restoreGlobalPerformanceSettings(&bridgeExit)
            : AmdAdlxBridge::restoreGlobalPerformanceSettings(&bridgeExit));
        if (bridgeExit != 0) {
            overallExit = bridgeExit;
        }
        int powerExit = 0;
        output.push_back(restoreActivePowerScheme(action.id, &powerExit));
        if (overallExit == 0 && powerExit != 0) {
            overallExit = powerExit;
        }
    }
    int commandExit = 0;
    output.push_back(SystemCatalog::runActionCommands(action.revertCommands, &commandExit));
    if (overallExit == 0 && commandExit != 0) {
        overallExit = commandExit;
    }
    if (exitCode) *exitCode = overallExit;
    return output.join(QStringLiteral("\n"));
}

QVector<GpuDeviceInfo> GpuOptimizationEngine::queryVideoControllers() const {
    QVector<GpuDeviceInfo> devices;
#ifdef Q_OS_WIN
    const QString script = QStringLiteral("Get-WmiObject Win32_VideoController | ForEach-Object { \"{0}`t{1}`t{2}\" -f ($_.Name -replace \"`t\",\" \"),$_.DriverVersion,$_.AdapterRAM }");
    int exitCode = 0;
    const QString output = runProcess(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), script}, &exitCode);
    if (exitCode != 0 || output.isEmpty()) {
        return devices;
    }
    for (const QString& line : output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 3) {
            continue;
        }
        const QString name = fields.at(0).trimmed();
        if (name.isEmpty()) {
            continue;
        }
        GpuDeviceInfo device;
        device.name = name;
        device.vendor = vendorFromName(name);
        device.driverVersion = fields.at(1).trimmed();
        device.memoryMB = adapterRamToMB(fields.at(2));
        devices.push_back(device);
    }
#endif
    return devices;
}

QVector<GpuDeviceInfo> GpuOptimizationEngine::queryNvidiaSmi() const {
    QVector<GpuDeviceInfo> devices;
    const QString smi = nvidiaSmiPath();
    if (smi.isEmpty()) {
        return devices;
    }
    int exitCode = 0;
    const QString output = runProcess(
        smi,
        {QStringLiteral("--query-gpu=name,driver_version,memory.total,temperature.gpu,utilization.gpu"), QStringLiteral("--format=csv,noheader,nounits")},
        &exitCode
    );
    if (exitCode != 0 || output.isEmpty()) {
        return devices;
    }
    for (const QString& line : output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(QLatin1Char(','));
        if (parts.size() < 5) {
            continue;
        }
        GpuDeviceInfo device;
        device.name = parts.at(0).trimmed();
        device.vendor = QStringLiteral("NVIDIA");
        device.driverVersion = parts.at(1).trimmed();
        device.memoryMB = parts.at(2).trimmed().toLongLong();
        device.temperatureC = parts.at(3).trimmed().toInt();
        device.loadPercent = parts.at(4).trimmed().toInt();
        device.nvidiaSmiAvailable = true;
        devices.push_back(device);
    }
    return devices;
}

QString GpuOptimizationEngine::nvidiaSmiPath() const {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"));
    if (!found.isEmpty()) {
        return found;
    }
#ifdef Q_OS_WIN
    const QString programFiles = programFilesPath(QStringLiteral("ProgramFiles"), QStringLiteral("C:\\Program Files"));
    const QString candidate = QDir(programFiles).filePath(QStringLiteral("NVIDIA Corporation/NVSMI/nvidia-smi.exe"));
    if (QFileInfo::exists(candidate)) {
        return QDir::toNativeSeparators(candidate);
    }
#endif
    return {};
}

bool GpuOptimizationEngine::nvapiAvailable() const {
    return NvidiaNvapiBridge::supportsGlobalProfileSettings();
}

bool GpuOptimizationEngine::adlxAvailable() const {
    return AmdAdlxBridge::supportsGlobal3DSettings();
}

bool GpuOptimizationEngine::amdSoftwareAvailable() const {
#ifdef Q_OS_WIN
    const QString programFiles = programFilesPath(QStringLiteral("ProgramFiles"), QStringLiteral("C:\\Program Files"));
    const QString programFilesX86 = programFilesPath(QStringLiteral("ProgramFiles(x86)"), QStringLiteral("C:\\Program Files (x86)"));
    const QStringList candidates = {
        QDir(programFiles).filePath(QStringLiteral("AMD/CNext/CNext/AMDSoftware.exe")),
        QDir(programFiles).filePath(QStringLiteral("AMD/CNext/CNext/RadeonSoftware.exe")),
        QDir(programFilesX86).filePath(QStringLiteral("AMD/CNext/CNext/AMDSoftware.exe")),
        QDir(programFilesX86).filePath(QStringLiteral("AMD/CNext/CNext/RadeonSoftware.exe")),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return true;
        }
    }
#endif
    return false;
}
