#include "SystemCatalog.h"

#include <QProcess>

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
            {command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc stop wuauserv & sc stop bits & sc stop UsoSvc & sc config wuauserv start= disabled & sc config bits start= disabled & sc config UsoSvc start= disabled")}, QStringLiteral("禁用 Windows 自动更新"))},
            {command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config wuauserv start= demand & sc config bits start= delayed-auto & sc config UsoSvc start= demand & sc start bits & sc start wuauserv")}, QStringLiteral("恢复 Windows 自动更新"))},
            true,
            false
        ),
        action(
            QStringLiteral("windows_update_enable"),
            QStringLiteral("Windows 自动更新：一键开启"),
            QStringLiteral("恢复更新相关服务并立即启动。"),
            QStringLiteral("Windows 自动更新"),
            QStringLiteral("安全"),
            {command(QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config wuauserv start= demand & sc config bits start= delayed-auto & sc config UsoSvc start= demand & sc start bits & sc start wuauserv")}, QStringLiteral("开启 Windows 自动更新"))},
            {},
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
            {command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-Command"), QStringLiteral("$u='https://go.microsoft.com/fwlink/?linkid=2109047&Channel=Stable&language=zh-cn'; $p=Join-Path $env:TEMP 'MicrosoftEdgeSetup.exe'; Invoke-WebRequest $u -OutFile $p; Start-Process $p -ArgumentList '/silent /install' -Wait")}, QStringLiteral("下载并安装 Microsoft Edge"))},
            {},
            true,
            false
        ),
        action(
            QStringLiteral("edge_remove"),
            QStringLiteral("Edge：一键彻底删除"),
            QStringLiteral("调用 Edge 官方卸载器并移除当前用户 Appx 包。"),
            QStringLiteral("Edge 工具箱"),
            QStringLiteral("高危"),
            {command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-Command"), QStringLiteral("$s=Get-ChildItem '${env:ProgramFiles(x86)}\\Microsoft\\Edge\\Application\\*\\Installer\\setup.exe' -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1; if($s){Start-Process $s.FullName -ArgumentList '--uninstall --system-level --verbose-logging --force-uninstall' -Wait}; Get-AppxPackage *MicrosoftEdge* | Remove-AppxPackage -ErrorAction SilentlyContinue")}, QStringLiteral("卸载 Microsoft Edge"))},
            {},
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
                command(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-Command"), QStringLiteral("Get-Process chrome,msedge,firefox -ErrorAction SilentlyContinue | Stop-Process -Force; $w=New-Object -ComObject WScript.Shell; $roots=@([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('CommonDesktopDirectory'),[Environment]::GetFolderPath('StartMenu')); Get-ChildItem $roots -Filter *.lnk -Recurse -ErrorAction SilentlyContinue | ForEach-Object {$l=$w.CreateShortcut($_.FullName); if($l.Arguments -match 'https?://'){$l.Arguments='';$l.Save()}}")}, QStringLiteral("清理浏览器快捷方式恶意参数")),
                command(QStringLiteral("reg"), {QStringLiteral("delete"), QStringLiteral("HKCU\\Software\\Policies\\Microsoft\\Edge"), QStringLiteral("/f")}, QStringLiteral("清理 Edge 用户策略")),
                command(QStringLiteral("reg"), {QStringLiteral("delete"), QStringLiteral("HKCU\\Software\\Policies\\Google\\Chrome"), QStringLiteral("/f")}, QStringLiteral("清理 Chrome 用户策略")),
                command(QStringLiteral("ipconfig"), {QStringLiteral("/flushdns")}, QStringLiteral("刷新 DNS 缓存")),
                command(QStringLiteral("netsh"), {QStringLiteral("winsock"), QStringLiteral("reset")}, QStringLiteral("重置 Winsock")),
            },
            {},
            true,
            false
        ),
    };
}

QVector<RepairItem> SystemCatalog::repairActions() {
    return {
        {QStringLiteral("sfc_scan"), QStringLiteral("SFC 系统文件修复"), QStringLiteral("安全"), QStringLiteral("检查并修复受保护系统文件。"), QStringLiteral("sfc /scannow"), true, false},
        {QStringLiteral("chkdsk_scan"), QStringLiteral("CHKDSK 磁盘安全扫描"), QStringLiteral("安全"), QStringLiteral("在线扫描 C 盘文件系统错误。"), QStringLiteral("chkdsk C: /scan"), true, false},
        {QStringLiteral("flush_dns"), QStringLiteral("DNS 刷新"), QStringLiteral("安全"), QStringLiteral("清空 DNS 解析缓存。"), QStringLiteral("ipconfig /flushdns"), true, false},
        {QStringLiteral("winsock_reset"), QStringLiteral("Winsock 网络重置"), QStringLiteral("安全"), QStringLiteral("重置 Windows 网络套接字目录。"), QStringLiteral("netsh winsock reset"), true, false},
        {QStringLiteral("dism_restore_health"), QStringLiteral("DISM 系统镜像修复"), QStringLiteral("谨慎"), QStringLiteral("使用微软 DISM 在线修复系统组件仓库。"), QStringLiteral("DISM /Online /Cleanup-Image /RestoreHealth"), false, true},
        {QStringLiteral("chkdsk_deep"), QStringLiteral("磁盘错误深度修复"), QStringLiteral("谨慎"), QStringLiteral("安排 C 盘深度修复，可能需要重启。"), QStringLiteral("echo Y|chkdsk C: /F /R"), false, true},
        {QStringLiteral("windows_update_reset"), QStringLiteral("系统更新组件修复"), QStringLiteral("谨慎"), QStringLiteral("重建 SoftwareDistribution 和 catroot2。"), QStringLiteral("net stop wuauserv & net stop bits & net stop cryptsvc & ren %systemroot%\\SoftwareDistribution SoftwareDistribution.old & ren %systemroot%\\System32\\catroot2 catroot2.old & net start cryptsvc & net start bits & net start wuauserv"), false, true},
        {QStringLiteral("cache_reset"), QStringLiteral("缓存重置修复"), QStringLiteral("谨慎"), QStringLiteral("重置微软商店缓存。"), QStringLiteral("wsreset.exe"), false, true},
    };
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
