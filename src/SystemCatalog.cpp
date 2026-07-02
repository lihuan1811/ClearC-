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
