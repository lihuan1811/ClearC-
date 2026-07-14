#include "GpuOptimizationEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVariant>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QString runProcess(const QString& executable, const QStringList& arguments, int* exitCode = nullptr) {
    QProcess process;
    process.start(executable, arguments);
    process.waitForFinished(15000);
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

qint64 adapterRamToMB(const QJsonValue& value) {
    bool ok = false;
    const qint64 bytes = value.toVariant().toLongLong(&ok);
    if (!ok || bytes <= 0) {
        return -1;
    }
    return bytes / (1024 * 1024);
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

bool hasAnyGpu(const QVector<GpuDeviceInfo>& devices) {
    return !devices.isEmpty();
}

#ifdef Q_OS_WIN
bool loadWindowsLibrary(const wchar_t* name) {
    HMODULE module = LoadLibraryW(name);
    if (!module) {
        return false;
    }
    FreeLibrary(module);
    return true;
}
#endif

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
    const bool intel = hasVendor(devices, QStringLiteral("Intel"));
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
        QStringLiteral("切换最高性能电源并清理 NVIDIA 着色器缓存；随后打开官方控制面板确认低延迟和垂直同步。仅在 nvidia-smi 或 NVAPI 可用时显示。"),
        QStringLiteral("谨慎"),
        windowsHost && nvidia && (nvidiaSmi || nvapi),
        true,
        {
            {QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_MIN")}, QStringLiteral("启用最高性能电源")},
            {QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("$p=@(\"$env:LOCALAPPDATA\\NVIDIA\\DXCache\",\"$env:LOCALAPPDATA\\NVIDIA\\GLCache\",\"$env:ProgramData\\NVIDIA Corporation\\NV_Cache\"); $p | ForEach-Object {Remove-Item $_ -Recurse -Force -ErrorAction SilentlyContinue}")}, QStringLiteral("清理 NVIDIA 着色器缓存")},
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start nvcplui.exe")}, QStringLiteral("打开 NVIDIA 官方控制面板")},
        },
        {
            {QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_BALANCED")}, QStringLiteral("恢复平衡电源计划")},
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start nvcplui.exe")}, QStringLiteral("打开 NVIDIA 控制面板恢复默认设置")},
        }
    );
    add(
        QStringLiteral("amd_one_click"),
        QStringLiteral("AMD"),
        QStringLiteral("AMD 一键调优"),
        QStringLiteral("切换全局性能电源并清理 AMD 驱动缓存；随后打开 AMD Software 确认垂直同步和帧率限制。仅在 ADLX 或 AMD Software 可用时显示。"),
        QStringLiteral("谨慎"),
        windowsHost && amd && (adlx || amdSoftware),
        true,
        {
            {QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_MIN")}, QStringLiteral("启用全局性能电源")},
            {QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Remove-Item \"$env:LOCALAPPDATA\\AMD\\DxCache\",\"$env:LOCALAPPDATA\\AMD\\GLCache\" -Recurse -Force -ErrorAction SilentlyContinue")}, QStringLiteral("清理 AMD 驱动缓存")},
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start amd-software:")}, QStringLiteral("打开 AMD 官方控制面板")},
        },
        {
            {QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_BALANCED")}, QStringLiteral("恢复平衡电源计划")},
            {QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start amd-software:")}, QStringLiteral("打开 AMD Software 恢复默认设置")},
        }
    );
    add(
        QStringLiteral("gpu_restore_defaults"),
        QStringLiteral("显卡"),
        QStringLiteral("恢复显卡默认设置"),
        QStringLiteral("恢复 Windows 平衡电源，并打开已检测到的厂商官方控制面板完成默认设置恢复。"),
        QStringLiteral("安全"),
        windowsHost && (nvidia || amd),
        true,
        {
            {QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_BALANCED")}, QStringLiteral("恢复平衡电源计划")},
            {QStringLiteral("cmd"), {QStringLiteral("/C"), nvidia ? QStringLiteral("start nvcplui.exe") : QStringLiteral("start amd-software:")}, QStringLiteral("打开厂商控制面板")},
        }
    );

    return actions;
}

QString GpuOptimizationEngine::runAction(const GpuOptimizationAction& action, int* exitCode) const {
    return SystemCatalog::runActionCommands(action.commands, exitCode);
}

QString GpuOptimizationEngine::restoreAction(const GpuOptimizationAction& action, int* exitCode) const {
    return SystemCatalog::runActionCommands(action.revertCommands, exitCode);
}

QVector<GpuDeviceInfo> GpuOptimizationEngine::queryVideoControllers() const {
    QVector<GpuDeviceInfo> devices;
#ifdef Q_OS_WIN
    const QString script = QStringLiteral("Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | ConvertTo-Json -Compress");
    int exitCode = 0;
    const QString output = runProcess(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), script}, &exitCode);
    if (exitCode != 0 || output.isEmpty()) {
        return devices;
    }
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        return devices;
    }
    QJsonArray array;
    if (doc.isArray()) {
        array = doc.array();
    } else if (doc.isObject()) {
        array.push_back(doc.object());
    }
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("Name")).toString();
        if (name.isEmpty()) {
            continue;
        }
        GpuDeviceInfo device;
        device.name = name;
        device.vendor = vendorFromName(name);
        device.driverVersion = object.value(QStringLiteral("DriverVersion")).toString();
        device.memoryMB = adapterRamToMB(object.value(QStringLiteral("AdapterRAM")));
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
#ifdef Q_OS_WIN
    return loadWindowsLibrary(L"nvapi64.dll") || loadWindowsLibrary(L"nvapi.dll");
#else
    return false;
#endif
}

bool GpuOptimizationEngine::adlxAvailable() const {
#ifdef Q_OS_WIN
    return loadWindowsLibrary(L"amdadlx64.dll") || loadWindowsLibrary(L"atiadlxx.dll");
#else
    return false;
#endif
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
