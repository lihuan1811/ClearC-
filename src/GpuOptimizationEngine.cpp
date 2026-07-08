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

    add(QStringLiteral("nvidia_smi_status"), QStringLiteral("NVIDIA"), QStringLiteral("NVIDIA 状态刷新"),
        QStringLiteral("使用 nvidia-smi 查看温度、负载、显存和驱动状态。"), QStringLiteral("只读"), nvidia && nvidiaSmi, false,
        {{nvidiaSmiPath(), {QStringLiteral("--query-gpu=name,driver_version,memory.total,temperature.gpu,utilization.gpu"), QStringLiteral("--format=csv")}, QStringLiteral("nvidia-smi 状态查询")}});
    add(QStringLiteral("nvidia_control_panel"), QStringLiteral("NVIDIA"), QStringLiteral("打开 NVIDIA 控制面板"),
        QStringLiteral("打开厂商控制面板，具体 3D 设置由用户在官方面板内确认。"), QStringLiteral("只读"), windowsHost && nvidia && (nvidiaSmi || nvapi), false,
        {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start nvcplui.exe")}, QStringLiteral("打开 NVIDIA 控制面板")}});
    add(QStringLiteral("amd_software"), QStringLiteral("AMD"), QStringLiteral("打开 AMD Software"),
        QStringLiteral("打开 AMD 官方控制面板；ADLX/AMDSoftware 可用时才显示。"), QStringLiteral("只读"), windowsHost && amd && (adlx || amdSoftware), false,
        {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start amd-software:")}, QStringLiteral("打开 AMDSoftware")}});
    add(QStringLiteral("intel_graphics_settings"), QStringLiteral("Intel"), QStringLiteral("打开 Windows 图形设置"),
        QStringLiteral("Intel 显卡使用 Windows 图形设置和厂商驱动面板进行调整。"), QStringLiteral("只读"), windowsHost && intel, false,
        {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start ms-settings:display-advancedgraphics")}, QStringLiteral("打开图形设置")}});
    add(QStringLiteral("gpu_power_high_performance"), QStringLiteral("Windows"), QStringLiteral("切换高性能电源计划"),
        QStringLiteral("仅使用 Windows powercfg 切换电源计划，不修改电压、频率或风扇。"), QStringLiteral("低风险"), windowsHost && hasAnyGpu(devices), true,
        {{QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_MIN")}, QStringLiteral("启用高性能电源计划")}},
        {{QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_BALANCED")}, QStringLiteral("恢复平衡电源计划")}});
    add(QStringLiteral("gpu_hags_enable"), QStringLiteral("Windows"), QStringLiteral("启用硬件加速 GPU 调度"),
        QStringLiteral("写入 GraphicsDrivers\\HwSchMode，需重启后生效。"), QStringLiteral("中风险"), windowsHost && hasAnyGpu(devices), true,
        {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers"), QStringLiteral("/v"), QStringLiteral("HwSchMode"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("2"), QStringLiteral("/f")}, QStringLiteral("启用 HAGS")}},
        {{QStringLiteral("reg"), {QStringLiteral("delete"), QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers"), QStringLiteral("/v"), QStringLiteral("HwSchMode"), QStringLiteral("/f")}, QStringLiteral("恢复 HAGS 默认")}});

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
