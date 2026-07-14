#include "SystemCatalog.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>

#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powrprof.h>
#endif

namespace {

WindowsOptimizationCommand command(
    const QString& executable,
    const QStringList& arguments,
    const QString& description
) {
    return {executable, arguments, description};
}

WindowsOptimizationAction action(
    const QString& id,
    const QString& title,
    const QString& description,
    const QString& category,
    const QString& risk,
    const QVector<WindowsOptimizationCommand>& commands,
    const QVector<WindowsOptimizationCommand>& revertCommands = {},
    bool requiresAdmin = false,
    bool recommended = true
) {
    return {id, title, description, category, risk, commands, revertCommands, requiresAdmin, recommended};
}

WindowsOptimizationCommand regAdd(
    const QString& key,
    const QString& value,
    const QString& data,
    const QString& description
) {
    return command(
        QStringLiteral("reg"),
        {QStringLiteral("add"), key, QStringLiteral("/v"), value, QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), data, QStringLiteral("/f")},
        description
    );
}

WindowsOptimizationCommand regDelete(
    const QString& key,
    const QString& value,
    const QString& description
) {
    return command(
        QStringLiteral("reg"),
        {QStringLiteral("delete"), key, QStringLiteral("/v"), value, QStringLiteral("/f")},
        description
    );
}

WindowsOptimizationCommand edgeInstallCommand() {
    return command(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            QStringLiteral("$u='https://go.microsoft.com/fwlink/?linkid=2109047&Channel=Stable&language=zh-cn'; $p=Join-Path $env:TEMP 'MicrosoftEdgeSetup.exe'; (New-Object Net.WebClient).DownloadFile($u,$p); Start-Process $p -ArgumentList '/silent /install' -Wait"),
        },
        QStringLiteral("下载并安装 Microsoft Edge")
    );
}

WindowsOptimizationCommand edgeRemoveCommand() {
    return command(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            QStringLiteral("$roots=@(${env:ProgramFiles},${env:ProgramFiles(x86)}) | Where-Object {$_}; $s=$roots | ForEach-Object {Get-ChildItem (Join-Path $_ 'Microsoft\\Edge\\Application\\*\\Installer\\setup.exe') -ErrorAction SilentlyContinue} | Sort-Object FullName -Descending | Select-Object -First 1; if(!$s){throw '未找到 Edge 官方卸载程序'}; Start-Process $s.FullName -ArgumentList '--uninstall --system-level --verbose-logging --force-uninstall' -Wait; if(Get-Command Get-AppxPackage -ErrorAction SilentlyContinue){Get-AppxPackage *MicrosoftEdge* | Remove-AppxPackage -ErrorAction SilentlyContinue}"),
        },
        QStringLiteral("卸载 Microsoft Edge")
    );
}

WindowsOptimizationCommand browserRepairCommand() {
    return command(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            QStringLiteral("$b=Join-Path $env:APPDATA 'C DiskGlow\\optimization_backups\\browser_homepage_fix'; New-Item $b -ItemType Directory -Force | Out-Null; $keys=@(@('HKCU\\Software\\Policies\\Microsoft\\Edge','edge'),@('HKCU\\Software\\Policies\\Google\\Chrome','chrome')); foreach($pair in $keys){$reg=Join-Path $b ($pair[1]+'.reg'); $absent=Join-Path $b ($pair[1]+'.absent'); if(!(Test-Path $reg) -and !(Test-Path $absent)){& reg.exe query $pair[0] > $null 2>&1; if($LASTEXITCODE -eq 0){& reg.exe export $pair[0] $reg /y > $null 2>&1}else{New-Item $absent -ItemType File | Out-Null}}}; Get-Process chrome,msedge,firefox -ErrorAction SilentlyContinue | Stop-Process -Force; $w=New-Object -ComObject WScript.Shell; $roots=@([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('CommonDesktopDirectory'),[Environment]::GetFolderPath('StartMenu')); $xml=Join-Path $b 'shortcuts.xml'; if(!(Test-Path $xml)){$records=@(); Get-ChildItem $roots -Filter *.lnk -Recurse -ErrorAction SilentlyContinue | ForEach-Object {$l=$w.CreateShortcut($_.FullName); if($l.Arguments -match 'https?://'){$records+=New-Object PSObject -Property @{Path=$_.FullName;Arguments=$l.Arguments}}}; @($records) | Export-Clixml $xml}; Get-ChildItem $roots -Filter *.lnk -Recurse -ErrorAction SilentlyContinue | ForEach-Object {$l=$w.CreateShortcut($_.FullName); if($l.Arguments -match 'https?://'){$l.Arguments='';$l.Save()}}; foreach($pair in $keys){& reg.exe delete $pair[0] /f > $null 2>&1}; exit 0"),
        },
        QStringLiteral("备份并修复浏览器策略与快捷方式")
    );
}

WindowsOptimizationCommand browserRestoreCommand() {
    return command(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            QStringLiteral("$b=Join-Path $env:APPDATA 'C DiskGlow\\optimization_backups\\browser_homepage_fix'; if(!(Test-Path $b)){throw '未找到浏览器修复备份'}; $keys=@(@('HKCU\\Software\\Policies\\Microsoft\\Edge','edge'),@('HKCU\\Software\\Policies\\Google\\Chrome','chrome')); foreach($pair in $keys){$reg=Join-Path $b ($pair[1]+'.reg'); $absent=Join-Path $b ($pair[1]+'.absent'); if(Test-Path $reg){& reg.exe import $reg > $null 2>&1; if($LASTEXITCODE -ne 0){throw ('注册表恢复失败: '+$pair[0])}}elseif(Test-Path $absent){& reg.exe delete $pair[0] /f > $null 2>&1}}; $xml=Join-Path $b 'shortcuts.xml'; if(Test-Path $xml){$w=New-Object -ComObject WScript.Shell; Import-Clixml $xml | ForEach-Object {if(Test-Path $_.Path){$l=$w.CreateShortcut($_.Path);$l.Arguments=$_.Arguments;$l.Save()}}}; Remove-Item $b -Recurse -Force; exit 0"),
        },
        QStringLiteral("恢复浏览器策略与快捷方式参数")
    );
}

QString actionBackupGroup(const QString& actionId) {
    return QStringLiteral("optimization/action_backups/%1").arg(actionId);
}

bool edgeInstalled() {
#ifdef Q_OS_WIN
    const QStringList roots = {
        qEnvironmentVariable("ProgramFiles"),
        qEnvironmentVariable("ProgramFiles(x86)"),
    };
    for (const QString& root : roots) {
        if (root.isEmpty()) {
            continue;
        }
        const QDir applicationRoot(QDir(root).filePath(QStringLiteral("Microsoft/Edge/Application")));
        const QFileInfoList versions = applicationRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& version : versions) {
            if (QFileInfo(QDir(version.absoluteFilePath()).filePath(QStringLiteral("msedge.exe"))).exists()) {
                return true;
            }
        }
    }
#endif
    return false;
}

QStringList serviceNamesForAction(const QString& actionId) {
    if (actionId == QStringLiteral("office_idle_service")) {
        return {QStringLiteral("MapsBroker")};
    }
    if (actionId == QStringLiteral("gaming_index")) {
        return {QStringLiteral("WSearch")};
    }
    if (actionId == QStringLiteral("windows_update_disable") || actionId == QStringLiteral("windows_update_enable")) {
        return {QStringLiteral("wuauserv"), QStringLiteral("bits"), QStringLiteral("UsoSvc")};
    }
    if (actionId == QStringLiteral("defender_permanent") || actionId == QStringLiteral("defender_restore")) {
        return {QStringLiteral("WinDefend")};
    }
    return {};
}

bool actionUsesPower(const WindowsOptimizationAction& action) {
    for (const WindowsOptimizationCommand& current : action.commands) {
        if (current.executable.compare(QStringLiteral("powercfg"), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

void captureRegistryValues(const WindowsOptimizationAction& action, QSettings* backup) {
    QVector<QPair<QString, QString>> values;
    for (const WindowsOptimizationCommand& current : action.commands) {
        if (current.executable.compare(QStringLiteral("reg"), Qt::CaseInsensitive) != 0
            || current.arguments.size() < 4) {
            continue;
        }
        const int valueIndex = current.arguments.indexOf(QStringLiteral("/v"), 2);
        if (valueIndex < 0 || valueIndex + 1 >= current.arguments.size()) {
            continue;
        }
        values.push_back({current.arguments.at(1), current.arguments.at(valueIndex + 1)});
    }
    if (action.id == QStringLiteral("gaming_hibernate")) {
        values.push_back({
            QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Power"),
            QStringLiteral("HibernateEnabled"),
        });
    }
    backup->setValue(QStringLiteral("registry/count"), values.size());
    for (int index = 0; index < values.size(); ++index) {
        const QString prefix = QStringLiteral("registry/%1/").arg(index);
        const QString key = values.at(index).first;
        const QString valueName = values.at(index).second;
        QSettings native(key, QSettings::NativeFormat);
        backup->setValue(prefix + QStringLiteral("key"), key);
        backup->setValue(prefix + QStringLiteral("valueName"), valueName);
        backup->setValue(prefix + QStringLiteral("exists"), native.contains(valueName));
        if (native.contains(valueName)) {
            backup->setValue(prefix + QStringLiteral("value"), native.value(valueName));
        }
    }
}

QStringList restoreRegistryValues(QSettings* backup) {
    QStringList errors;
    const int count = backup->value(QStringLiteral("registry/count"), 0).toInt();
    for (int index = 0; index < count; ++index) {
        const QString prefix = QStringLiteral("registry/%1/").arg(index);
        const QString key = backup->value(prefix + QStringLiteral("key")).toString();
        const QString valueName = backup->value(prefix + QStringLiteral("valueName")).toString();
        if (key.isEmpty() || valueName.isEmpty()) {
            continue;
        }
        QSettings native(key, QSettings::NativeFormat);
        if (backup->value(prefix + QStringLiteral("exists"), false).toBool()) {
            native.setValue(valueName, backup->value(prefix + QStringLiteral("value")));
        } else {
            native.remove(valueName);
        }
        native.sync();
        if (native.status() != QSettings::NoError) {
            errors.push_back(QStringLiteral("注册表原状态恢复失败: %1\\%2").arg(key, valueName));
        }
    }
    return errors;
}

#ifdef Q_OS_WIN
void capturePowerState(const WindowsOptimizationAction& action, QSettings* backup) {
    if (!actionUsesPower(action)) {
        return;
    }
    GUID* activeScheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &activeScheme) == ERROR_SUCCESS && activeScheme) {
        backup->setValue(
            QStringLiteral("power/activeScheme"),
            QByteArray(reinterpret_cast<const char*>(activeScheme), sizeof(GUID))
        );
        LocalFree(activeScheme);
    }
    if (action.id == QStringLiteral("gaming_power")) {
        DWORD minimum = 0;
        if (PowerReadACValueIndex(
                nullptr,
                &GUID_MIN_POWER_SAVINGS,
                &GUID_PROCESSOR_SETTINGS_SUBGROUP,
                &GUID_PROCESSOR_THROTTLE_MINIMUM,
                &minimum
            ) == ERROR_SUCCESS) {
            backup->setValue(QStringLiteral("power/highPerformanceMinimum"), static_cast<quint32>(minimum));
        }
    }
}

QStringList restorePowerState(const WindowsOptimizationAction& action, QSettings* backup) {
    QStringList errors;
    if (action.id == QStringLiteral("gaming_power") && backup->contains(QStringLiteral("power/highPerformanceMinimum"))) {
        const DWORD minimum = backup->value(QStringLiteral("power/highPerformanceMinimum")).toUInt();
        if (PowerWriteACValueIndex(
                nullptr,
                &GUID_MIN_POWER_SAVINGS,
                &GUID_PROCESSOR_SETTINGS_SUBGROUP,
                &GUID_PROCESSOR_THROTTLE_MINIMUM,
                minimum
            ) != ERROR_SUCCESS) {
            errors.push_back(QStringLiteral("高性能电源计划的 CPU 原设置恢复失败"));
        }
    }
    const QByteArray activeBytes = backup->value(QStringLiteral("power/activeScheme")).toByteArray();
    if (activeBytes.size() == static_cast<int>(sizeof(GUID))) {
        GUID activeScheme{};
        std::memcpy(&activeScheme, activeBytes.constData(), sizeof(GUID));
        if (PowerSetActiveScheme(nullptr, &activeScheme) != ERROR_SUCCESS) {
            errors.push_back(QStringLiteral("原电源计划恢复失败"));
        }
    }
    return errors;
}

void captureServiceStates(const WindowsOptimizationAction& action, QSettings* backup) {
    const QStringList services = serviceNamesForAction(action.id);
    backup->setValue(QStringLiteral("services/names"), services);
    if (services.isEmpty()) {
        return;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return;
    }
    for (const QString& name : services) {
        SC_HANDLE service = OpenServiceW(
            manager,
            reinterpret_cast<LPCWSTR>(name.utf16()),
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS
        );
        if (!service) {
            continue;
        }
        DWORD required = 0;
        QueryServiceConfigW(service, nullptr, 0, &required);
        QByteArray buffer(static_cast<int>(required), '\0');
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        SERVICE_STATUS_PROCESS status{};
        DWORD statusBytes = 0;
        if (required > 0
            && QueryServiceConfigW(service, config, required, &required)
            && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &statusBytes)) {
            const QString prefix = QStringLiteral("services/%1/").arg(name);
            backup->setValue(prefix + QStringLiteral("exists"), true);
            backup->setValue(prefix + QStringLiteral("startType"), static_cast<quint32>(config->dwStartType));
            backup->setValue(prefix + QStringLiteral("running"), status.dwCurrentState == SERVICE_RUNNING);
            SERVICE_DELAYED_AUTO_START_INFO delayed{};
            DWORD delayedBytes = 0;
            if (QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, reinterpret_cast<LPBYTE>(&delayed), sizeof(delayed), &delayedBytes)) {
                backup->setValue(prefix + QStringLiteral("delayed"), delayed.fDelayedAutostart != FALSE);
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
}

QStringList restoreServiceStates(const WindowsOptimizationAction& action, QSettings* backup) {
    QStringList errors;
    const QStringList services = backup->value(QStringLiteral("services/names"), serviceNamesForAction(action.id)).toStringList();
    if (services.isEmpty()) {
        return errors;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return {QStringLiteral("无法打开 Windows 服务管理器以恢复原状态")};
    }
    for (const QString& name : services) {
        const QString prefix = QStringLiteral("services/%1/").arg(name);
        if (!backup->value(prefix + QStringLiteral("exists"), false).toBool()) {
            continue;
        }
        SC_HANDLE service = OpenServiceW(
            manager,
            reinterpret_cast<LPCWSTR>(name.utf16()),
            SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP
        );
        if (!service) {
            errors.push_back(QStringLiteral("无法打开服务恢复原状态: %1").arg(name));
            continue;
        }
        const DWORD startType = backup->value(prefix + QStringLiteral("startType")).toUInt();
        if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            errors.push_back(QStringLiteral("服务启动类型恢复失败: %1").arg(name));
        }
        if (backup->contains(prefix + QStringLiteral("delayed"))) {
            SERVICE_DELAYED_AUTO_START_INFO delayed{};
            delayed.fDelayedAutostart = backup->value(prefix + QStringLiteral("delayed")).toBool();
            ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
        }
        SERVICE_STATUS_PROCESS current{};
        DWORD currentBytes = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&current), sizeof(current), &currentBytes)) {
            const bool shouldRun = backup->value(prefix + QStringLiteral("running"), false).toBool();
            if (shouldRun && current.dwCurrentState == SERVICE_STOPPED) {
                if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                    errors.push_back(QStringLiteral("服务运行状态恢复失败: %1").arg(name));
                }
            } else if (!shouldRun && current.dwCurrentState == SERVICE_RUNNING) {
                SERVICE_STATUS ignored{};
                if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored)) {
                    errors.push_back(QStringLiteral("服务停止状态恢复失败: %1").arg(name));
                }
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return errors;
}
#else
void capturePowerState(const WindowsOptimizationAction&, QSettings*) {}
QStringList restorePowerState(const WindowsOptimizationAction&, QSettings*) { return {}; }
void captureServiceStates(const WindowsOptimizationAction&, QSettings*) {}
QStringList restoreServiceStates(const WindowsOptimizationAction&, QSettings*) { return {}; }
#endif

void captureDefenderAndHibernateState(const WindowsOptimizationAction& action, QSettings* backup) {
#ifdef Q_OS_WIN
    if (action.id == QStringLiteral("edge_install") || action.id == QStringLiteral("edge_remove")) {
        backup->setValue(QStringLiteral("edge/installed"), edgeInstalled());
    }
    if (action.id.startsWith(QStringLiteral("defender_"))) {
        QProcess process;
        process.start(
            QStringLiteral("powershell"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("(Get-MpPreference).DisableRealtimeMonitoring")}
        );
        if (process.waitForFinished(15000) && process.exitCode() == 0) {
            const QString value = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
            if (value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                || value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
                backup->setValue(QStringLiteral("defender/realtimeDisabled"), value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
            }
        }
    }
    if (action.id == QStringLiteral("gaming_hibernate")) {
        QSettings power(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Power"), QSettings::NativeFormat);
        if (power.contains(QStringLiteral("HibernateEnabled"))) {
            backup->setValue(QStringLiteral("hibernate/enabled"), power.value(QStringLiteral("HibernateEnabled")).toInt() != 0);
        }
    }
    if (action.id == QStringLiteral("gaming_restore")) {
        QSettings systemRestore(
            QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore"),
            QSettings::NativeFormat
        );
        backup->setValue(
            QStringLiteral("systemRestore/disabled"),
            systemRestore.value(QStringLiteral("DisableSR"), 0).toInt() != 0
        );
    }
#else
    Q_UNUSED(action);
    Q_UNUSED(backup);
#endif
}

QString restoreDefenderAndHibernateState(const WindowsOptimizationAction& action, QSettings* backup, int* exitCode) {
    QVector<WindowsOptimizationCommand> commands;
    if (action.id.startsWith(QStringLiteral("defender_")) && backup->contains(QStringLiteral("defender/realtimeDisabled"))) {
        const bool disabled = backup->value(QStringLiteral("defender/realtimeDisabled")).toBool();
        commands.push_back(command(
            QStringLiteral("powershell"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Set-MpPreference -DisableRealtimeMonitoring $%1").arg(disabled ? QStringLiteral("true") : QStringLiteral("false"))},
            QStringLiteral("恢复 Defender 实时防护原状态")
        ));
    }
    if (action.id == QStringLiteral("gaming_hibernate") && backup->contains(QStringLiteral("hibernate/enabled"))) {
        commands.push_back(command(
            QStringLiteral("powercfg"),
            {QStringLiteral("/hibernate"), backup->value(QStringLiteral("hibernate/enabled")).toBool() ? QStringLiteral("on") : QStringLiteral("off")},
            QStringLiteral("恢复休眠原状态")
        ));
    }
    if (commands.isEmpty()) {
        if (exitCode) *exitCode = 0;
        return {};
    }
    return SystemCatalog::runActionCommands(commands, exitCode);
}

}  // namespace

QVector<WindowsOptimizationAction> SystemCatalog::officeOptimizationActions() {
    return {
        action(
            QStringLiteral("office_startup_delay"),
            QStringLiteral("关闭启动应用延迟"),
            QStringLiteral("缩短登录后启动项等待时间，不删除任何启动项。"),
            QStringLiteral("开机启动"),
            QStringLiteral("安全"),
            {regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize"), QStringLiteral("StartupDelayInMSec"), QStringLiteral("0"), QStringLiteral("关闭启动应用延迟"))},
            {regDelete(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize"), QStringLiteral("StartupDelayInMSec"), QStringLiteral("恢复启动应用延迟默认"))}
        ),
        action(
            QStringLiteral("office_content_delivery"),
            QStringLiteral("关闭系统推荐与广告内容"),
            QStringLiteral("关闭 Windows 推荐应用、提示和内容投递开关。"),
            QStringLiteral("广告弹窗"),
            QStringLiteral("安全"),
            {
                regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager"), QStringLiteral("SilentInstalledAppsEnabled"), QStringLiteral("0"), QStringLiteral("关闭静默推荐应用")),
                regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager"), QStringLiteral("SystemPaneSuggestionsEnabled"), QStringLiteral("0"), QStringLiteral("关闭开始菜单建议")),
            },
            {
                regDelete(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager"), QStringLiteral("SilentInstalledAppsEnabled"), QStringLiteral("恢复推荐应用默认")),
                regDelete(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager"), QStringLiteral("SystemPaneSuggestionsEnabled"), QStringLiteral("恢复系统建议默认")),
            }
        ),
        action(
            QStringLiteral("office_animation"),
            QStringLiteral("适度精简窗口动画"),
            QStringLiteral("关闭透明效果，保留常用界面能力。"),
            QStringLiteral("视觉效果"),
            QStringLiteral("安全"),
            {regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QStringLiteral("EnableTransparency"), QStringLiteral("0"), QStringLiteral("关闭透明效果"))},
            {regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QStringLiteral("EnableTransparency"), QStringLiteral("1"), QStringLiteral("恢复透明效果"))}
        ),
        action(
            QStringLiteral("office_background_cache"),
            QStringLiteral("清理后台临时缓存"),
            QStringLiteral("只清理当前用户临时目录中可删除的文件。"),
            QStringLiteral("后台缓存"),
            QStringLiteral("安全"),
            {command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Get-ChildItem $env:TEMP -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue")}, QStringLiteral("清理当前用户临时缓存"))}
        ),
        action(
            QStringLiteral("office_network"),
            QStringLiteral("刷新网络解析缓存"),
            QStringLiteral("刷新 DNS 缓存，修复常见域名解析异常。"),
            QStringLiteral("网络"),
            QStringLiteral("安全"),
            {command(QStringLiteral("ipconfig"), {QStringLiteral("/flushdns")}, QStringLiteral("刷新 DNS 缓存"))}
        ),
        action(
            QStringLiteral("office_idle_service"),
            QStringLiteral("闲置地图服务改为按需"),
            QStringLiteral("将 MapsBroker 改为按需启动，不删除服务。"),
            QStringLiteral("系统服务"),
            QStringLiteral("安全"),
            {command(QStringLiteral("sc"), {QStringLiteral("config"), QStringLiteral("MapsBroker"), QStringLiteral("start="), QStringLiteral("demand")}, QStringLiteral("地图服务改为按需"))},
            {command(QStringLiteral("sc"), {QStringLiteral("config"), QStringLiteral("MapsBroker"), QStringLiteral("start="), QStringLiteral("auto")}, QStringLiteral("恢复地图服务自动启动"))},
            true
        ),
    };
}

QVector<WindowsOptimizationAction> SystemCatalog::gamingOptimizationActions() {
    QVector<WindowsOptimizationAction> actions = officeOptimizationActions();
    actions += QVector<WindowsOptimizationAction>{
        action(
            QStringLiteral("gaming_power"),
            QStringLiteral("切换高性能电源并关闭 CPU 节能"),
            QStringLiteral("启用高性能计划，并把交流供电时处理器最低状态设为 100%。"),
            QStringLiteral("电源与 CPU"),
            QStringLiteral("谨慎"),
            {
                command(QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_MIN")}, QStringLiteral("启用高性能电源计划")),
                command(QStringLiteral("powercfg"), {QStringLiteral("/setacvalueindex"), QStringLiteral("SCHEME_CURRENT"), QStringLiteral("SUB_PROCESSOR"), QStringLiteral("PROCTHROTTLEMIN"), QStringLiteral("100")}, QStringLiteral("关闭交流供电 CPU 节能")),
            },
            {
                command(QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_BALANCED")}, QStringLiteral("恢复平衡电源计划")),
                command(QStringLiteral("powercfg"), {QStringLiteral("/setacvalueindex"), QStringLiteral("SCHEME_CURRENT"), QStringLiteral("SUB_PROCESSOR"), QStringLiteral("PROCTHROTTLEMIN"), QStringLiteral("5")}, QStringLiteral("恢复 CPU 节能")),
            },
            true,
            false
        ),
        action(
            QStringLiteral("gaming_visuals"),
            QStringLiteral("关闭系统动画"),
            QStringLiteral("调整为最佳性能视觉效果，减少桌面动画开销。"),
            QStringLiteral("视觉效果"),
            QStringLiteral("谨慎"),
            {regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects"), QStringLiteral("VisualFXSetting"), QStringLiteral("2"), QStringLiteral("启用最佳性能视觉效果"))},
            {regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects"), QStringLiteral("VisualFXSetting"), QStringLiteral("0"), QStringLiteral("恢复 Windows 自动选择视觉效果"))},
            false,
            false
        ),
        action(
            QStringLiteral("gaming_notifications"),
            QStringLiteral("关闭系统通知"),
            QStringLiteral("关闭通知中心弹窗，可能错过办公提醒。"),
            QStringLiteral("通知"),
            QStringLiteral("谨慎"),
            {regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications"), QStringLiteral("ToastEnabled"), QStringLiteral("0"), QStringLiteral("关闭通知弹窗"))},
            {regAdd(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications"), QStringLiteral("ToastEnabled"), QStringLiteral("1"), QStringLiteral("恢复通知弹窗"))},
            false,
            false
        ),
        action(
            QStringLiteral("gaming_restore"),
            QStringLiteral("禁用系统还原"),
            QStringLiteral("关闭 C 盘系统保护；不会主动删除已有还原点。"),
            QStringLiteral("系统还原"),
            QStringLiteral("高危"),
            {command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Disable-ComputerRestore -Drive 'C:\\'")}, QStringLiteral("禁用 C 盘系统还原"))},
            {command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Enable-ComputerRestore -Drive 'C:\\'")}, QStringLiteral("恢复 C 盘系统还原"))},
            true,
            false
        ),
        action(
            QStringLiteral("gaming_hibernate"),
            QStringLiteral("关闭休眠"),
            QStringLiteral("释放 hiberfil.sys，占用休眠和快速启动能力。"),
            QStringLiteral("休眠"),
            QStringLiteral("谨慎"),
            {command(QStringLiteral("powercfg"), {QStringLiteral("/hibernate"), QStringLiteral("off")}, QStringLiteral("关闭休眠"))},
            {command(QStringLiteral("powercfg"), {QStringLiteral("/hibernate"), QStringLiteral("on")}, QStringLiteral("恢复休眠"))},
            true,
            false
        ),
        action(
            QStringLiteral("gaming_index"),
            QStringLiteral("禁用磁盘索引"),
            QStringLiteral("停止 Windows Search 并禁用服务，文件搜索会变慢。"),
            QStringLiteral("磁盘索引"),
            QStringLiteral("谨慎"),
            {command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc stop WSearch & sc config WSearch start= disabled")}, QStringLiteral("禁用 Windows Search"))},
            {command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config WSearch start= delayed-auto & sc start WSearch")}, QStringLiteral("恢复 Windows Search"))},
            true,
            false
        ),
    };
    return actions;
}

QVector<WindowsOptimizationAction> SystemCatalog::advancedControlActions() {
    return {
        action(
            QStringLiteral("windows_update_disable"),
            QStringLiteral("Windows 自动更新：一键禁用"),
            QStringLiteral("停止并禁用 Windows Update、BITS 和 Update Orchestrator 服务。"),
            QStringLiteral("Windows 自动更新"),
            QStringLiteral("高危"),
            {
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc stop wuauserv & sc stop bits & sc config wuauserv start= disabled & sc config bits start= disabled")}, QStringLiteral("禁用 Windows Update 与 BITS")),
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc query UsoSvc >nul 2>&1 && (sc stop UsoSvc & sc config UsoSvc start= disabled) || exit /b 0")}, QStringLiteral("兼容处理 Update Orchestrator")),
            },
            {
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config wuauserv start= demand & sc config bits start= delayed-auto & sc start bits & sc start wuauserv")}, QStringLiteral("恢复 Windows Update 与 BITS")),
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc query UsoSvc >nul 2>&1 && sc config UsoSvc start= demand || exit /b 0")}, QStringLiteral("兼容恢复 Update Orchestrator")),
            },
            true,
            false
        ),
        action(
            QStringLiteral("windows_update_enable"),
            QStringLiteral("Windows 自动更新：一键开启"),
            QStringLiteral("恢复更新相关服务并立即启动。"),
            QStringLiteral("Windows 自动更新"),
            QStringLiteral("安全"),
            {
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config wuauserv start= demand & sc config bits start= delayed-auto & sc start bits & sc start wuauserv")}, QStringLiteral("开启 Windows Update 与 BITS")),
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc query UsoSvc >nul 2>&1 && sc config UsoSvc start= demand || exit /b 0")}, QStringLiteral("兼容开启 Update Orchestrator")),
            },
            {
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc stop wuauserv & sc stop bits & sc config wuauserv start= disabled & sc config bits start= disabled")}, QStringLiteral("恢复为禁用 Windows Update 与 BITS")),
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc query UsoSvc >nul 2>&1 && (sc stop UsoSvc & sc config UsoSvc start= disabled) || exit /b 0")}, QStringLiteral("兼容恢复 Update Orchestrator")),
            },
            true
        ),
        action(
            QStringLiteral("defender_temporary"),
            QStringLiteral("Defender：临时终止实时防护"),
            QStringLiteral("临时关闭实时防护，系统或策略可能自动恢复。"),
            QStringLiteral("Windows 安全中心"),
            QStringLiteral("高危"),
            {command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Set-MpPreference -DisableRealtimeMonitoring $true")}, QStringLiteral("临时关闭 Defender 实时防护"))},
            {command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Set-MpPreference -DisableRealtimeMonitoring $false")}, QStringLiteral("恢复 Defender 实时防护"))},
            true,
            false
        ),
        action(
            QStringLiteral("defender_permanent"),
            QStringLiteral("Defender：永久禁用防护"),
            QStringLiteral("写入策略并停止 Defender 服务；受防篡改保护时可能被系统拒绝。"),
            QStringLiteral("Windows 安全中心"),
            QStringLiteral("高危"),
            {
                regAdd(QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender"), QStringLiteral("DisableAntiSpyware"), QStringLiteral("1"), QStringLiteral("写入 Defender 禁用策略")),
                regAdd(QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection"), QStringLiteral("DisableRealtimeMonitoring"), QStringLiteral("1"), QStringLiteral("写入实时防护禁用策略")),
                command(QStringLiteral("sc"), {QStringLiteral("stop"), QStringLiteral("WinDefend")}, QStringLiteral("停止 Defender 服务")),
            },
            {
                regDelete(QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender"), QStringLiteral("DisableAntiSpyware"), QStringLiteral("删除 Defender 禁用策略")),
                regDelete(QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection"), QStringLiteral("DisableRealtimeMonitoring"), QStringLiteral("删除实时防护禁用策略")),
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config WinDefend start= auto & sc start WinDefend")}, QStringLiteral("恢复 Defender 服务")),
            },
            true,
            false
        ),
        action(
            QStringLiteral("defender_restore"),
            QStringLiteral("Defender：一键恢复开启"),
            QStringLiteral("删除本工具写入的禁用策略并恢复实时防护。"),
            QStringLiteral("Windows 安全中心"),
            QStringLiteral("安全"),
            {
                regDelete(QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender"), QStringLiteral("DisableAntiSpyware"), QStringLiteral("删除 Defender 禁用策略")),
                regDelete(QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection"), QStringLiteral("DisableRealtimeMonitoring"), QStringLiteral("删除实时防护禁用策略")),
                command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Set-MpPreference -DisableRealtimeMonitoring $false")}, QStringLiteral("恢复实时防护")),
                command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config WinDefend start= auto & sc start WinDefend")}, QStringLiteral("恢复 Defender 服务")),
            },
            {},
            true
        ),
        action(
            QStringLiteral("edge_install"),
            QStringLiteral("Edge：一键静默安装"),
            QStringLiteral("从微软官方地址下载稳定版安装器并静默安装。"),
            QStringLiteral("Edge 工具箱"),
            QStringLiteral("谨慎"),
            {edgeInstallCommand()},
            {edgeRemoveCommand()},
            true,
            false
        ),
        action(
            QStringLiteral("edge_remove"),
            QStringLiteral("Edge：一键彻底删除"),
            QStringLiteral("调用 Edge 官方卸载器并移除当前用户 Appx 包。"),
            QStringLiteral("Edge 工具箱"),
            QStringLiteral("高危"),
            {edgeRemoveCommand()},
            {edgeInstallCommand()},
            true,
            false
        ),
        action(
            QStringLiteral("browser_homepage_fix"),
            QStringLiteral("一键修复浏览器主页篡改"),
            QStringLiteral("终止常见劫持进程、清理快捷方式恶意参数和浏览器策略，并重置 DNS/Winsock。"),
            QStringLiteral("浏览器修复"),
            QStringLiteral("谨慎"),
            {
                browserRepairCommand(),
                command(QStringLiteral("ipconfig"), {QStringLiteral("/flushdns")}, QStringLiteral("刷新 DNS 缓存")),
                command(QStringLiteral("netsh"), {QStringLiteral("winsock"), QStringLiteral("reset")}, QStringLiteral("重置 Winsock")),
            },
            {browserRestoreCommand()},
            true,
            false
        ),
    };
}

QVector<RepairItem> SystemCatalog::repairActions() {
    QVector<RepairItem> actions = {
        {QStringLiteral("sfc_scan"), QStringLiteral("SFC 系统文件修复"), QStringLiteral("安全"), QStringLiteral("检查并修复受保护系统文件。"), QStringLiteral("sfc /scannow"), true, false},
        {QStringLiteral("chkdsk_scan"), QStringLiteral("CHKDSK 磁盘安全扫描"), QStringLiteral("安全"), QStringLiteral("以只读方式检查 C 盘文件系统错误，兼容 Windows 7。"), QStringLiteral("chkdsk C:"), true, false},
        {QStringLiteral("flush_dns"), QStringLiteral("DNS 刷新"), QStringLiteral("安全"), QStringLiteral("清空 DNS 解析缓存。"), QStringLiteral("ipconfig /flushdns"), true, false},
        {QStringLiteral("winsock_reset"), QStringLiteral("Winsock 网络重置"), QStringLiteral("安全"), QStringLiteral("重置 Windows 网络套接字目录。"), QStringLiteral("netsh winsock reset"), true, false},
        {QStringLiteral("dism_restore_health"), QStringLiteral("DISM 系统镜像修复"), QStringLiteral("谨慎"), QStringLiteral("使用 DISM 修复系统组件仓库；Windows 7 自动回退到 SFC。"), QStringLiteral("powershell -NoProfile -Command \"$v=[Environment]::OSVersion.Version; if($v.Major -eq 6 -and $v.Minor -eq 1){sfc /scannow}else{DISM /Online /Cleanup-Image /RestoreHealth}\""), false, true},
        {QStringLiteral("chkdsk_deep"), QStringLiteral("磁盘错误深度修复"), QStringLiteral("谨慎"), QStringLiteral("安排 C 盘深度修复，可能需要重启。"), QStringLiteral("echo Y|chkdsk C: /F /R"), false, true},
        {QStringLiteral("windows_update_reset"), QStringLiteral("系统更新组件修复"), QStringLiteral("谨慎"), QStringLiteral("重建 SoftwareDistribution 和 catroot2。"), QStringLiteral("net stop wuauserv & net stop bits & net stop cryptsvc & ren %systemroot%\\SoftwareDistribution SoftwareDistribution.old & ren %systemroot%\\System32\\catroot2 catroot2.old & net start cryptsvc & net start bits & net start wuauserv"), false, true},
    };
#ifdef Q_OS_WIN
    const QString systemRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    if (QFileInfo(QDir(systemRoot).filePath(QStringLiteral("System32/wsreset.exe"))).exists()) {
        actions.push_back({QStringLiteral("cache_reset"), QStringLiteral("缓存重置修复"), QStringLiteral("谨慎"), QStringLiteral("重置微软商店缓存。"), QStringLiteral("wsreset.exe"), false, true});
    }
#else
    actions.push_back({QStringLiteral("cache_reset"), QStringLiteral("缓存重置修复"), QStringLiteral("谨慎"), QStringLiteral("重置微软商店缓存。"), QStringLiteral("wsreset.exe"), false, true});
#endif
    return actions;
}

QString SystemCatalog::runCommand(const QString& commandText, int* exitCode) {
    QProcess process;
#ifdef Q_OS_WIN
    process.start(QStringLiteral("cmd.exe"), {QStringLiteral("/C"), commandText});
#else
    process.start(QStringLiteral("sh"), {QStringLiteral("-c"), commandText});
#endif
    if (!process.waitForStarted(5000)) {
        if (exitCode) {
            *exitCode = -1;
        }
        return QStringLiteral("命令启动失败: %1").arg(process.errorString());
    }
    if (!process.waitForFinished(-1)) {
        process.kill();
        process.waitForFinished();
    }
    if (exitCode) {
        *exitCode = process.exitCode();
    }
    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString error = QString::fromLocal8Bit(process.readAllStandardError());
    if (!error.isEmpty()) {
        output += QStringLiteral("\n") + error;
    }
    return output.trimmed().isEmpty() ? QStringLiteral("命令无输出") : output.trimmed();
}

QString SystemCatalog::runActionCommands(
    const QVector<WindowsOptimizationCommand>& commands,
    int* exitCode
) {
    QString output;
    int overallExit = 0;
    for (const WindowsOptimizationCommand& current : commands) {
        if (current.executable.trimmed().isEmpty()) {
            continue;
        }
        QProcess process;
        process.start(current.executable, current.arguments);
        if (!process.waitForStarted(5000)) {
            if (overallExit == 0) {
                overallExit = -1;
            }
            output += QStringLiteral("[%1]\n命令启动失败: %2\n")
                .arg(current.description, process.errorString());
            continue;
        }
        if (!process.waitForFinished(-1)) {
            process.kill();
            process.waitForFinished();
        }
        if (process.exitCode() != 0 && overallExit == 0) {
            overallExit = process.exitCode();
        }
        output += QStringLiteral("[%1]\n").arg(current.description);
        output += QString::fromLocal8Bit(process.readAllStandardOutput());
        const QString error = QString::fromLocal8Bit(process.readAllStandardError());
        if (!error.isEmpty()) {
            output += QStringLiteral("\n") + error;
        }
        output += QStringLiteral("\n");
    }
    if (exitCode) {
        *exitCode = overallExit;
    }
    return output.trimmed().isEmpty() ? QStringLiteral("命令无输出") : output.trimmed();
}

QString SystemCatalog::runOptimizationAction(
    const WindowsOptimizationAction& action,
    bool revert,
    int* exitCode
) {
    QSettings backup;
    backup.beginGroup(actionBackupGroup(action.id));
    if (revert && !backup.value(QStringLiteral("valid"), false).toBool()) {
        backup.endGroup();
        if (exitCode) {
            *exitCode = -1;
        }
        return QStringLiteral("未找到本工具执行前保存的原状态快照，为避免写入错误默认值，未进行还原。");
    }
    if (!revert && !action.revertCommands.isEmpty() && !backup.value(QStringLiteral("valid"), false).toBool()) {
        captureRegistryValues(action, &backup);
        capturePowerState(action, &backup);
        captureServiceStates(action, &backup);
        captureDefenderAndHibernateState(action, &backup);
        backup.setValue(QStringLiteral("valid"), true);
        backup.sync();
    }

    QVector<WindowsOptimizationCommand> commands = revert ? action.revertCommands : action.commands;
    if (revert && action.id == QStringLiteral("gaming_restore")
        && backup.contains(QStringLiteral("systemRestore/disabled"))
        && backup.value(QStringLiteral("systemRestore/disabled")).toBool()) {
        commands = action.commands;
    }
    QString edgeNotice;
    if ((action.id == QStringLiteral("edge_install") || action.id == QStringLiteral("edge_remove"))
        && backup.contains(QStringLiteral("edge/installed"))) {
        const bool wasInstalled = backup.value(QStringLiteral("edge/installed")).toBool();
        const bool operationWouldChangeState = action.id == QStringLiteral("edge_install") ? !wasInstalled : wasInstalled;
        if (!operationWouldChangeState) {
            commands.clear();
            edgeNotice = wasInstalled
                ? QStringLiteral("执行前 Edge 已安装，本次未修改安装状态。")
                : QStringLiteral("执行前 Edge 未安装，本次未修改安装状态。");
        }
    }
    int commandExit = 0;
    QStringList output;
    output.push_back(edgeNotice.isEmpty() ? runActionCommands(commands, &commandExit) : edgeNotice);
    int overallExit = commandExit;
    if (revert && backup.value(QStringLiteral("valid"), false).toBool()) {
        QStringList restoreErrors = restoreRegistryValues(&backup);
        restoreErrors.append(restorePowerState(action, &backup));
        restoreErrors.append(restoreServiceStates(action, &backup));
        int stateExit = 0;
        const QString stateOutput = restoreDefenderAndHibernateState(action, &backup, &stateExit);
        if (!stateOutput.isEmpty()) {
            output.push_back(stateOutput);
        }
        if (stateExit != 0 && overallExit == 0) {
            overallExit = stateExit;
        }
        if (!restoreErrors.isEmpty()) {
            output.push_back(restoreErrors.join(QStringLiteral("\n")));
            if (overallExit == 0) {
                overallExit = -1;
            }
        }
        if (overallExit == 0) {
            backup.remove(QString());
        }
    }
    backup.endGroup();
    if (exitCode) {
        *exitCode = overallExit;
    }
    return output.join(QStringLiteral("\n"));
}
