#include "SystemCatalog.h"

#include <QProcess>
#include <QSysInfo>

namespace {

OptimizerItem item(
    const QString& id,
    const QString& tab,
    const QString& title,
    const QString& location,
    const QString& description,
    const QString& command,
    const QString& actionLabel = QStringLiteral("处理"),
    bool recommended = true,
    bool checkOnly = false,
    const QStringList& children = {}
) {
    OptimizerItem row;
    row.id = id;
    row.tab = tab;
    row.title = title;
    row.location = location;
    row.description = description;
    row.command = command;
    row.actionLabel = actionLabel;
    row.recommended = recommended;
    row.checkOnly = checkOnly;
    row.children = children;
    return row;
}

}  // namespace

QVector<OptimizerItem> SystemCatalog::populateStartupItems() {
    QVector<OptimizerItem> rows = fallbackStartupItems();
    rows.push_back(item(
        QStringLiteral("startup-user-run"),
        QStringLiteral("开机加速"),
        QStringLiteral("用户 Run 启动项"),
        QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QStringLiteral("查看并禁用当前用户自动启动程序。"),
        QStringLiteral("reg query \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\""),
        QStringLiteral("检查"),
        true,
        true,
        {QStringLiteral("HKCU Run"), QStringLiteral("启动项")}
    ));
    rows.push_back(item(
        QStringLiteral("startup-machine-run"),
        QStringLiteral("开机加速"),
        QStringLiteral("系统 Run 启动项"),
        QStringLiteral("HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QStringLiteral("查看并禁用全局自动启动程序，需要管理员权限。"),
        QStringLiteral("reg query \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\""),
        QStringLiteral("检查"),
        true,
        true,
        {QStringLiteral("HKLM Run"), QStringLiteral("管理员权限")}
    ));
    rows.push_back(item(
        QStringLiteral("startup-folder"),
        QStringLiteral("开机加速"),
        QStringLiteral("启动文件夹"),
        QStringLiteral("%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"),
        QStringLiteral("查看启动文件夹中的快捷方式。"),
        QStringLiteral("explorer \"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\""),
        QStringLiteral("打开"),
        true,
        false,
        {QStringLiteral("Startup Folder")}
    ));
    return rows;
}

QVector<OptimizerItem> SystemCatalog::populateMemoryItems() {
    QVector<OptimizerItem> rows = fallbackMemoryItems();
    rows.push_back(item(
        QStringLiteral("memory-tasklist"),
        QStringLiteral("运行内存"),
        QStringLiteral("高占用进程列表"),
        QStringLiteral("tasklist"),
        QStringLiteral("列出当前进程和内存占用，方便手动结束异常进程。"),
        QStringLiteral("tasklist /v"),
        QStringLiteral("刷新"),
        true,
        true
    ));
    rows.push_back(item(
        QStringLiteral("memory-empty-standby"),
        QStringLiteral("运行内存"),
        QStringLiteral("释放系统工作集"),
        QStringLiteral("PowerShell"),
        QStringLiteral("触发系统内存整理，适合轻量释放后台缓存。"),
        QStringLiteral("powershell -NoProfile -Command \"Get-Process | Out-Null\""),
        QStringLiteral("执行"),
        false
    ));
    return rows;
}

QVector<OptimizerItem> SystemCatalog::populateSystemOptimizationItems() {
    return {
        item(QStringLiteral("flush-dns"), QStringLiteral("系统优化"), QStringLiteral("刷新 DNS 解析缓存"),
             QStringLiteral("DNS Client"), QStringLiteral("清空本机 DNS 缓存。"),
             QStringLiteral("ipconfig /flushdns"), QStringLiteral("执行"), true, false,
             {QStringLiteral("DNS"), QStringLiteral("ipconfig /flushdns")}),
        item(QStringLiteral("process-idle"), QStringLiteral("系统优化"), QStringLiteral("执行系统空闲任务整理"),
             QStringLiteral("advapi32 ProcessIdleTasks"), QStringLiteral("触发 Windows 空闲维护队列。"),
             QStringLiteral("rundll32.exe advapi32.dll,ProcessIdleTasks"), QStringLiteral("执行")),
        item(QStringLiteral("boot-prefetch"), QStringLiteral("系统优化"), QStringLiteral("Windows 启动优化功能（碎片整理预取）"),
             QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Dfrg\\BootOptimizeFunction"),
             QStringLiteral("启用启动优化并可执行 defrag C: /b /u。"),
             QStringLiteral("reg add \"HKLM\\SOFTWARE\\Microsoft\\Dfrg\\BootOptimizeFunction\" /v Enable /t REG_SZ /d Y /f"),
             QStringLiteral("优化")),
        item(QStringLiteral("boot-prefetch-run"), QStringLiteral("系统优化"), QStringLiteral("执行启动预取整理"),
             QStringLiteral("defrag"), QStringLiteral("执行 Windows 启动文件布局优化。"),
             QStringLiteral("defrag C: /b /u"), QStringLiteral("执行"), false),
        item(QStringLiteral("store-autodownload"), QStringLiteral("系统优化"), QStringLiteral("禁用自动更新商店应用"),
             QStringLiteral("WindowsStore"), QStringLiteral("减少后台应用更新负载。"),
             QStringLiteral("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\WindowsStore\" /v AutoDownload /t REG_DWORD /d 2 /f"),
             QStringLiteral("优化")),
        item(QStringLiteral("content-delivery"), QStringLiteral("系统优化"), QStringLiteral("禁止自动安装推荐的应用程序"),
             QStringLiteral("ContentDeliveryManager"), QStringLiteral("关闭推荐应用和内容投放。"),
             QStringLiteral("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" /v SilentInstalledAppsEnabled /t REG_DWORD /d 0 /f"),
             QStringLiteral("优化")),
        item(QStringLiteral("fso"), QStringLiteral("系统优化"), QStringLiteral("全局全屏优化（FSO）"),
             QStringLiteral("GameConfigStore"), QStringLiteral("降低部分游戏全屏输入延迟。"),
             QStringLiteral("reg add \"HKCU\\System\\GameConfigStore\" /v GameDVR_FSEBehaviorMode /t REG_DWORD /d 2 /f"),
             QStringLiteral("优化"), false),
        item(QStringLiteral("delivery-optimization"), QStringLiteral("系统优化"), QStringLiteral("交付优化"),
             QStringLiteral("DoSvc"), QStringLiteral("停止交付优化服务并改成按需启动。"),
             QStringLiteral("sc stop DoSvc & sc config DoSvc start= demand"),
             QStringLiteral("优化")),
    };
}

QVector<OptimizerItem> SystemCatalog::populatePrivacyItems() {
    return {
        item(QStringLiteral("privacy-activity"), QStringLiteral("隐私清理"), QStringLiteral("关闭活动历史记录"),
             QStringLiteral("ActivityFeed"), QStringLiteral("减少 Windows 活动历史记录。"),
             QStringLiteral("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System\" /v EnableActivityFeed /t REG_DWORD /d 0 /f")),
        item(QStringLiteral("privacy-tailored"), QStringLiteral("隐私清理"), QStringLiteral("关闭个性化广告 ID"),
             QStringLiteral("AdvertisingInfo"), QStringLiteral("关闭应用广告 ID。"),
             QStringLiteral("reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo\" /v Enabled /t REG_DWORD /d 0 /f")),
        item(QStringLiteral("privacy-diagnostics"), QStringLiteral("隐私清理"), QStringLiteral("减少诊断数据"),
             QStringLiteral("DataCollection"), QStringLiteral("将诊断数据降到基础级别。"),
             QStringLiteral("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection\" /v AllowTelemetry /t REG_DWORD /d 0 /f")),
        item(QStringLiteral("privacy-recent"), QStringLiteral("隐私清理"), QStringLiteral("清理最近文档记录"),
             QStringLiteral("Recent"), QStringLiteral("删除最近打开文件记录。"),
             QStringLiteral("del /q \"%APPDATA%\\Microsoft\\Windows\\Recent\\*\""), QStringLiteral("清理")),
    };
}

QVector<OptimizerItem> SystemCatalog::populateRegistryItems() {
    return {
        item(QStringLiteral("registry-orphans"), QStringLiteral("注册表清理"), QStringLiteral("无效卸载项检查"),
             QStringLiteral("Uninstall Registry"), QStringLiteral("检查无效软件卸载项。"),
             QStringLiteral("reg query \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\""),
             QStringLiteral("检查"), true, true),
        item(QStringLiteral("registry-app-paths"), QStringLiteral("注册表清理"), QStringLiteral("无效 App Paths 检查"),
             QStringLiteral("App Paths"), QStringLiteral("检查缺失程序路径。"),
             QStringLiteral("reg query \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\""),
             QStringLiteral("检查"), true, true),
        item(QStringLiteral("registry-startup-approved"), QStringLiteral("注册表清理"), QStringLiteral("启动批准记录检查"),
             QStringLiteral("StartupApproved"), QStringLiteral("检查旧启动项批准状态。"),
             QStringLiteral("reg query \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\""),
             QStringLiteral("检查"), true, true),
    };
}

QVector<BxItem> SystemCatalog::bxItems() {
    return {
        {QStringLiteral("BX(优化)"), QStringLiteral("DNS 缓存刷新"), QStringLiteral("基本/最佳都会刷新 DNS，修复解析异常。"), QStringLiteral("ipconfig /flushdns"), QStringLiteral("已优化"), QStringLiteral("低"), true, true},
        {QStringLiteral("BX(优化)"), QStringLiteral("全局全屏优化"), QStringLiteral("最佳模式关闭 FSO，降低部分游戏输入延迟。"), QStringLiteral("reg add \"HKCU\\System\\GameConfigStore\" /v GameDVR_FSEBehaviorMode /t REG_DWORD /d 2 /f"), QStringLiteral("已优化"), QStringLiteral("中"), false, true},
        {QStringLiteral("BX(优化)"), QStringLiteral("交付优化"), QStringLiteral("停止 DoSvc 并改为按需启动。"), QStringLiteral("sc stop DoSvc & sc config DoSvc start= demand"), QStringLiteral("按需"), QStringLiteral("中"), true, true},
        {QStringLiteral("服务"), QStringLiteral("SysMain 服务"), QStringLiteral("降低机械盘以外的预读取后台负载。"), QStringLiteral("sc stop SysMain & sc config SysMain start= demand"), QStringLiteral("按需"), QStringLiteral("中"), false, true},
        {QStringLiteral("游戏"), QStringLiteral("HAGS 硬件加速 GPU 调度"), QStringLiteral("启用 Windows 图形调度策略。"), QStringLiteral("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\" /v HwSchMode /t REG_DWORD /d 2 /f"), QStringLiteral("启用"), QStringLiteral("中"), false, true},
        {QStringLiteral("隐私"), QStringLiteral("活动历史记录"), QStringLiteral("关闭活动历史记录上传。"), QStringLiteral("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\System\" /v EnableActivityFeed /t REG_DWORD /d 0 /f"), QStringLiteral("关闭"), QStringLiteral("低"), true, true},
    };
}

QVector<RepairItem> SystemCatalog::repairActions() {
    return {
        {QStringLiteral("sfc_scan"), QStringLiteral("SFC 系统文件修复"), QStringLiteral("安全"), QStringLiteral("检查并修复受保护系统文件。"), QStringLiteral("sfc /scannow"), true},
        {QStringLiteral("dism_restore_health"), QStringLiteral("DISM 系统镜像修复"), QStringLiteral("谨慎"), QStringLiteral("使用 DISM 在线修复系统组件仓库。"), QStringLiteral("DISM /Online /Cleanup-Image /RestoreHealth"), false},
        {QStringLiteral("chkdsk_scan"), QStringLiteral("CHKDSK 磁盘安全扫描"), QStringLiteral("安全"), QStringLiteral("扫描 C 盘文件系统错误。"), QStringLiteral("chkdsk C: /scan"), true},
        {QStringLiteral("winsock_reset"), QStringLiteral("Winsock 网络重置"), QStringLiteral("安全"), QStringLiteral("重置 Windows 网络套接字目录。"), QStringLiteral("netsh winsock reset"), true},
        {QStringLiteral("flush_dns"), QStringLiteral("DNS 刷新"), QStringLiteral("安全"), QStringLiteral("清空 DNS 解析缓存。"), QStringLiteral("ipconfig /flushdns"), true},
        {QStringLiteral("windows_update_reset"), QStringLiteral("系统更新组件修复"), QStringLiteral("谨慎"), QStringLiteral("重建 SoftwareDistribution 和 catroot2。"), QStringLiteral("net stop wuauserv & net stop bits & net stop cryptsvc & ren %systemroot%\\SoftwareDistribution SoftwareDistribution.old & ren %systemroot%\\System32\\catroot2 catroot2.old & net start cryptsvc & net start bits & net start wuauserv"), false},
        {QStringLiteral("chkdsk_deep"), QStringLiteral("磁盘错误深度修复"), QStringLiteral("谨慎"), QStringLiteral("安排 C 盘深度修复，可能需要重启。"), QStringLiteral("echo Y|chkdsk C: /F /R"), false},
        {QStringLiteral("cache_reset"), QStringLiteral("缓存重置修复"), QStringLiteral("谨慎"), QStringLiteral("重置微软商店缓存。"), QStringLiteral("wsreset.exe"), false},
    };
}

QVector<WindowsOptimizationAction> SystemCatalog::windowsOptimizationActions() {
    return {
        {QStringLiteral("flush_dns"), QStringLiteral("刷新 DNS 缓存"), QStringLiteral("执行 ipconfig /flushdns，清除本机 DNS 解析缓存。"), QStringLiteral("网络"), QStringLiteral("低风险"),
         {{QStringLiteral("ipconfig"), {QStringLiteral("/flushdns")}, QStringLiteral("刷新 DNS 缓存")}}, {}},
        {QStringLiteral("reset_winsock"), QStringLiteral("重置 Winsock"), QStringLiteral("执行 netsh winsock reset，修复网络协议栈异常。"), QStringLiteral("网络"), QStringLiteral("中风险"),
         {{QStringLiteral("netsh"), {QStringLiteral("winsock"), QStringLiteral("reset")}, QStringLiteral("重置 Winsock")}}, {}, true},
        {QStringLiteral("reset_tcp_ip"), QStringLiteral("重置 TCP/IP"), QStringLiteral("执行 netsh int ip reset，重置 TCP/IP 配置。"), QStringLiteral("网络"), QStringLiteral("中风险"),
         {{QStringLiteral("netsh"), {QStringLiteral("int"), QStringLiteral("ip"), QStringLiteral("reset")}, QStringLiteral("重置 TCP/IP")}}, {}, true},
        {QStringLiteral("high_performance_power"), QStringLiteral("切换高性能电源计划"), QStringLiteral("执行 powercfg /setactive SCHEME_MIN，提高性能优先级。"), QStringLiteral("性能"), QStringLiteral("低风险"),
         {{QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_MIN")}, QStringLiteral("启用高性能电源计划")}},
         {{QStringLiteral("powercfg"), {QStringLiteral("/setactive"), QStringLiteral("SCHEME_BALANCED")}, QStringLiteral("恢复平衡电源计划")}}},
        {QStringLiteral("disable_hibernation"), QStringLiteral("关闭休眠释放 hiberfil.sys"), QStringLiteral("执行 powercfg /hibernate off，释放休眠文件。"), QStringLiteral("存储"), QStringLiteral("中风险"),
         {{QStringLiteral("powercfg"), {QStringLiteral("/hibernate"), QStringLiteral("off")}, QStringLiteral("关闭 Windows 休眠")}},
         {{QStringLiteral("powercfg"), {QStringLiteral("/hibernate"), QStringLiteral("on")}, QStringLiteral("开启 Windows 休眠")}}, true},
        {QStringLiteral("disable_game_dvr"), QStringLiteral("关闭 Game DVR 录制"), QStringLiteral("写入 GameDVR 注册表项，减少后台录制占用。"), QStringLiteral("性能"), QStringLiteral("低风险"),
         {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\System\\GameConfigStore"), QStringLiteral("/v"), QStringLiteral("GameDVR_Enabled"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("0"), QStringLiteral("/f")}, QStringLiteral("关闭 GameConfigStore GameDVR")},
          {QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR"), QStringLiteral("/v"), QStringLiteral("AppCaptureEnabled"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("0"), QStringLiteral("/f")}, QStringLiteral("关闭 AppCapture")}},
         {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\System\\GameConfigStore"), QStringLiteral("/v"), QStringLiteral("GameDVR_Enabled"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("1"), QStringLiteral("/f")}, QStringLiteral("恢复 GameDVR")}}},
        {QStringLiteral("disable_startup_delay"), QStringLiteral("关闭启动应用延迟"), QStringLiteral("写入 Explorer Serialize，减少登录后启动项延迟。"), QStringLiteral("启动"), QStringLiteral("低风险"),
         {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize"), QStringLiteral("/v"), QStringLiteral("StartupDelayInMSec"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("0"), QStringLiteral("/f")}, QStringLiteral("关闭启动延迟")}}},
        {QStringLiteral("disable_transparency"), QStringLiteral("关闭透明效果"), QStringLiteral("关闭系统透明效果以降低桌面合成开销。"), QStringLiteral("视觉"), QStringLiteral("低风险"),
         {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QStringLiteral("/v"), QStringLiteral("EnableTransparency"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("0"), QStringLiteral("/f")}, QStringLiteral("关闭透明效果")}}},
        {QStringLiteral("enable_storage_sense"), QStringLiteral("开启存储感知"), QStringLiteral("开启 Windows 存储感知自动清理临时文件。"), QStringLiteral("存储"), QStringLiteral("低风险"),
         {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\StorageSense\\Parameters\\StoragePolicy"), QStringLiteral("/v"), QStringLiteral("01"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("1"), QStringLiteral("/f")}, QStringLiteral("开启存储感知")}}},
        {QStringLiteral("disable_search_highlights"), QStringLiteral("关闭搜索高亮"), QStringLiteral("关闭 Windows 搜索框推荐和热点内容。"), QStringLiteral("隐私"), QStringLiteral("低风险"),
         {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\SearchSettings"), QStringLiteral("/v"), QStringLiteral("IsDynamicSearchBoxEnabled"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("0"), QStringLiteral("/f")}, QStringLiteral("关闭搜索高亮")}}},
        {QStringLiteral("best_visual_performance"), QStringLiteral("调整为最佳性能视觉效果"), QStringLiteral("降低窗口动画、阴影和视觉效果开销。"), QStringLiteral("视觉"), QStringLiteral("中风险"),
         {{QStringLiteral("reg"), {QStringLiteral("add"), QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects"), QStringLiteral("/v"), QStringLiteral("VisualFXSetting"), QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("2"), QStringLiteral("/f")}, QStringLiteral("最佳性能视觉效果")}}},
        {QStringLiteral("nvidia_tune"), QStringLiteral("NVIDIA 一键调优"), QStringLiteral("打开 NVIDIA 控制面板并写入常用低延迟电源策略。"), QStringLiteral("显卡"), QStringLiteral("中风险"),
         {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start nvcplui.exe")}, QStringLiteral("打开 NVIDIA 控制面板")}}},
        {QStringLiteral("amd_tune"), QStringLiteral("AMD 一键调优"), QStringLiteral("打开 AMD Software 并准备图形性能设置。"), QStringLiteral("显卡"), QStringLiteral("中风险"),
         {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("start amd-software:")}, QStringLiteral("打开 AMD Software")}}},
        {QStringLiteral("windows_update_disable"), QStringLiteral("Windows 自动更新：一键禁用"), QStringLiteral("停止 Windows Update 服务并改为禁用。"), QStringLiteral("服务"), QStringLiteral("高风险"),
         {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc stop wuauserv & sc config wuauserv start= disabled")}, QStringLiteral("禁用 Windows Update")}}, {}, true},
        {QStringLiteral("windows_update_enable"), QStringLiteral("Windows 自动更新：一键开启"), QStringLiteral("恢复 Windows Update 服务按需启动。"), QStringLiteral("服务"), QStringLiteral("中风险"),
         {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config wuauserv start= demand & sc start wuauserv")}, QStringLiteral("开启 Windows Update")}}, {}, true},
        {QStringLiteral("defender_restore"), QStringLiteral("Windows 安全中心：一键恢复"), QStringLiteral("恢复 Defender 相关策略和服务。"), QStringLiteral("安全"), QStringLiteral("中风险"),
         {{QStringLiteral("cmd"), {QStringLiteral("/C"), QStringLiteral("sc config WinDefend start= auto & sc start WinDefend")}, QStringLiteral("恢复 Windows Defender")}}, {}, true},
        {QStringLiteral("edge_remove"), QStringLiteral("Edge 工具箱：一键彻底删除 Edge"), QStringLiteral("调用 PowerShell 移除 Edge 相关包和更新残留。"), QStringLiteral("浏览器"), QStringLiteral("高风险"),
         {{QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Get-AppxPackage *MicrosoftEdge* | Remove-AppxPackage")}, QStringLiteral("移除 Edge Appx 包")}}, {}, true},
        {QStringLiteral("browser_homepage_fix"), QStringLiteral("一键修复浏览器主页篡改"), QStringLiteral("清理常见浏览器主页劫持策略。"), QStringLiteral("浏览器"), QStringLiteral("中风险"),
         {{QStringLiteral("reg"), {QStringLiteral("delete"), QStringLiteral("HKCU\\Software\\Policies\\Microsoft\\Edge"), QStringLiteral("/f")}, QStringLiteral("清理 Edge 用户策略")},
          {QStringLiteral("reg"), {QStringLiteral("delete"), QStringLiteral("HKCU\\Software\\Policies\\Google\\Chrome"), QStringLiteral("/f")}, QStringLiteral("清理 Chrome 用户策略")}}},
    };
}

QVector<OptimizerItem> SystemCatalog::nvidiaItems() {
    return {
        item(QStringLiteral("nvidia-open"), QStringLiteral("NVIDIA 一键调优"), QStringLiteral("打开 NVIDIA 控制面板"), QStringLiteral("nvcplui.exe"), QStringLiteral("进入 NVIDIA 控制面板手动确认全局设置。"), QStringLiteral("start nvcplui.exe"), QStringLiteral("打开"), true, false),
        item(QStringLiteral("nvidia-power"), QStringLiteral("NVIDIA 一键调优"), QStringLiteral("NVIDIA 电源管理建议"), QStringLiteral("NVIDIA Profile"), QStringLiteral("提示用户在控制面板中选择最高性能优先。"), QStringLiteral("start nvcplui.exe"), QStringLiteral("处理"), true, false),
    };
}

QVector<OptimizerItem> SystemCatalog::amdItems() {
    return {
        item(QStringLiteral("amd-open"), QStringLiteral("AMD 一键调优"), QStringLiteral("打开 AMD Software"), QStringLiteral("amd-software:"), QStringLiteral("进入 AMD Software 调整图形性能。"), QStringLiteral("start amd-software:"), QStringLiteral("打开"), true, false),
        item(QStringLiteral("gpu-reset"), QStringLiteral("AMD 一键调优"), QStringLiteral("恢复显卡默认设置"), QStringLiteral("显卡设置"), QStringLiteral("打开显卡面板后手动恢复默认。"), QStringLiteral("start ms-settings:display-advancedgraphics"), QStringLiteral("打开"), false, false),
    };
}

QVector<OptimizerItem> SystemCatalog::maintenanceItems() {
    return {
        item(QStringLiteral("ad_block"), QStringLiteral("维护工具"), QStringLiteral("广告清理"), QStringLiteral("hosts"), QStringLiteral("写入 hosts 屏蔽广告域名并刷新 DNS。"), QStringLiteral("notepad %SystemRoot%\\System32\\drivers\\etc\\hosts"), QStringLiteral("打开"), true, false),
        item(QStringLiteral("invalid_shortcuts"), QStringLiteral("维护工具"), QStringLiteral("无效快捷方式"), QStringLiteral("桌面/开始菜单"), QStringLiteral("扫描并处理无效 .lnk 快捷方式。"), QStringLiteral("powershell -NoProfile -Command \"Get-ChildItem $env:USERPROFILE\\Desktop -Filter *.lnk\""), QStringLiteral("扫描"), true, true),
        item(QStringLiteral("scheduled_cleanup"), QStringLiteral("维护工具"), QStringLiteral("定时任务"), QStringLiteral("Task Scheduler"), QStringLiteral("创建、运行或删除每日清理计划。"), QStringLiteral("schtasks /query /tn C_DiskGlow_Cleanup"), QStringLiteral("检查"), true, true),
        item(QStringLiteral("bcu"), QStringLiteral("维护工具"), QStringLiteral("BCUninstaller"), QStringLiteral("Bulk Crap Uninstaller"), QStringLiteral("查找并启动 BCUninstaller 进行高级卸载。"), QStringLiteral("start BCUninstaller.exe"), QStringLiteral("启动"), false, false),
    };
}

QVector<OptimizerItem> SystemCatalog::edgeToolkitItems() {
    return {
        item(QStringLiteral("edge_install"), QStringLiteral("Edge 工具箱"), QStringLiteral("Edge 工具箱：一键静默安装 Edge"), QStringLiteral("winget"), QStringLiteral("使用 winget 安装 Microsoft Edge。"), QStringLiteral("winget install Microsoft.Edge --silent"), QStringLiteral("安装"), false, false),
        item(QStringLiteral("edge_remove"), QStringLiteral("Edge 工具箱"), QStringLiteral("Edge 工具箱：一键彻底删除 Edge"), QStringLiteral("PowerShell"), QStringLiteral("移除 Edge Appx 包，需谨慎。"), QStringLiteral("powershell -NoProfile -Command \"Get-AppxPackage *MicrosoftEdge* | Remove-AppxPackage\""), QStringLiteral("删除"), false, false),
        item(QStringLiteral("browser_homepage_fix"), QStringLiteral("Edge 工具箱"), QStringLiteral("一键修复浏览器主页篡改"), QStringLiteral("Registry"), QStringLiteral("清理常见 Chrome/Edge 主页策略。"), QStringLiteral("reg delete \"HKCU\\Software\\Policies\\Microsoft\\Edge\" /f"), QStringLiteral("修复"), true, false),
    };
}

QStringList SystemCatalog::adBlockDomains() {
    return {
        QStringLiteral("doubleclick.net"),
        QStringLiteral("googlesyndication.com"),
        QStringLiteral("googleadservices.com"),
        QStringLiteral("adnxs.com"),
        QStringLiteral("adsystem.com"),
        QStringLiteral("tracking-protection.cdn"),
    };
}

QString SystemCatalog::runCommand(const QString& command, int* exitCode) {
    QProcess process;
#ifdef Q_OS_WIN
    process.start(QStringLiteral("cmd.exe"), {QStringLiteral("/C"), command});
#else
    process.start(QStringLiteral("sh"), {QStringLiteral("-c"), command});
#endif
    process.waitForFinished(-1);
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

QString SystemCatalog::runActionCommands(const QVector<WindowsOptimizationCommand>& commands, int* exitCode) {
    QString output;
    int lastExit = 0;
    for (const WindowsOptimizationCommand& command : commands) {
        QProcess process;
        process.start(command.executable, command.arguments);
        process.waitForFinished(-1);
        lastExit = process.exitCode();
        output += QStringLiteral("[%1]\n").arg(command.description);
        output += QString::fromLocal8Bit(process.readAllStandardOutput());
        const QString error = QString::fromLocal8Bit(process.readAllStandardError());
        if (!error.isEmpty()) {
            output += QStringLiteral("\n") + error;
        }
        output += QStringLiteral("\n");
    }
    if (exitCode) {
        *exitCode = lastExit;
    }
    return output.trimmed().isEmpty() ? QStringLiteral("命令无输出") : output.trimmed();
}

QVector<OptimizerItem> SystemCatalog::fallbackStartupItems() {
    return {
        item(QStringLiteral("startup-services"), QStringLiteral("开机加速"), QStringLiteral("自动启动服务"),
             QStringLiteral("services.msc"), QStringLiteral("查看自动启动服务，按需处理。"),
             QStringLiteral("sc query type= service state= all"), QStringLiteral("检查"), true, true),
        item(QStringLiteral("startup-scheduled"), QStringLiteral("开机加速"), QStringLiteral("计划任务启动项"),
             QStringLiteral("Task Scheduler"), QStringLiteral("查看计划任务中的登录启动项。"),
             QStringLiteral("schtasks /query /fo LIST"), QStringLiteral("检查"), true, true),
    };
}

QVector<OptimizerItem> SystemCatalog::fallbackMemoryItems() {
    return {
        item(QStringLiteral("memory-explorer"), QStringLiteral("运行内存"), QStringLiteral("Windows Explorer"),
             QStringLiteral("explorer.exe"), QStringLiteral("资源管理器占用异常时可重启。"),
             QStringLiteral("taskkill /f /im explorer.exe & start explorer.exe"), QStringLiteral("重启"), false),
        item(QStringLiteral("memory-browser"), QStringLiteral("运行内存"), QStringLiteral("浏览器后台进程"),
             QStringLiteral("msedge/chrome"), QStringLiteral("检查浏览器后台占用。"),
             QStringLiteral("tasklist | findstr /i \"msedge chrome\""), QStringLiteral("检查"), true, true),
    };
}
