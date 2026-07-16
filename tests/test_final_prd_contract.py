from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_navigation_exposes_only_the_five_final_prd_modules():
    source = read(SRC / "MainWindow.cpp")
    header = read(SRC / "MainWindow.h")
    cmake = read(ROOT / "CMakeLists.txt")

    navigation = source.split("QWidget* MainWindow::createTopNavigation", 1)[1].split(
        "QWidget* MainWindow::createBottomBar", 1
    )[0]
    for label in ["C盘深度清理", "软件强力卸载", "系统智能优化", "磁盘文件管理器", "CMD 系统修复"]:
        assert label in navigation
    for removed in ["QQ专清", "微信专清", "账号会员", "BX(优化)", "碎片整理", "规则商店"]:
        assert removed not in navigation

    assert "createSidebar" not in header + source
    assert "createAccountPage" not in header + source
    assert "AccountStore" not in cmake
    assert "DismRuleScanner" not in cmake


def test_cleanup_matches_the_final_prd_categories_and_safety_contract():
    header = read(SRC / "CleanupEngine.h")
    engine = read(SRC / "CleanupEngine.cpp")
    window = read(SRC / "MainWindow.cpp")

    for token in [
        "过期文件",
        "系统相关",
        "缓存文件",
        "应用程序",
        "临时文件",
        "Windows 更新下载缓存",
        "Windows 错误报告",
        "Windows 预读取文件",
        "Microsoft Edge 缓存",
        "Google Chrome 缓存",
        "Mozilla Firefox 缓存",
        "QQ 浏览器缓存",
        "Microsoft Store 缓存",
        "OneDrive 日志缓存",
        "下载未完成残留",
    ]:
        assert token in engine

    for token in [
        "scanRules",
        "minimumAgeDays",
        "description",
        "全不选",
        "默认",
        "刷新",
        "清理项",
        "文件",
        "状态",
        "说明",
        "扫描位置",
        "等待扫描",
        "仅分析",
        "setCleanupSelection",
        "restoreDefaultCleanupSelection",
        "selectedCleanupRuleIds",
        "options.backup = true",
        "风险等级",
        "添加白名单",
    ]:
        assert token in header + window

    assert "DismRuleScanner" not in engine
    assert "wechat_special_clean" not in engine
    assert "qq_special_clean" not in engine
    assert "浏览器保存密码" not in engine
    assert "allowScanOnly = true" not in window


def test_uninstall_supports_registry_uwp_search_sort_backup_batch_and_residuals():
    header = read(SRC / "SoftwareUninstallEngine.h")
    engine = read(SRC / "SoftwareUninstallEngine.cpp")
    window = read(SRC / "MainWindow.cpp")
    cmake = read(ROOT / "CMakeLists.txt")

    for token in [
        "InstalledApplication",
        "installLocation",
        "installDate",
        "sizeBytes",
        "storeApp",
        "installedApplications",
        "Get-AppxPackage",
        "Remove-AppxPackage -Package",
        "EstimatedSize",
        "InstallLocation",
        "InstallDate",
        "reg\"), {QStringLiteral(\"export\")",
        "registry_backups",
        "cleanResiduals",
        "CurrentVersion\\\\Run",
        "*.lnk",
    ]:
        assert token in header + engine

    for token in [
        "搜索软件名称、发布者或安装路径",
        "setSortingEnabled(true)",
        "批量卸载选中",
        "cleanApplicationResidualsBatch",
        "常规卸载",
        "强力粉碎卸载",
        "清理卸载残留",
    ]:
        assert token in window
    assert "SoftwareUninstallEngine.cpp" in cmake
    assert "startDetached" not in engine
    assert "Get-AppxPackage -PackageFullName" not in engine
    assert "singleShot(1000" not in window
    assert "waitForFinished(-1)" in engine
    assert "else if (result.completed)" in window
    assert "cleanApplicationResiduals(app);" in window
    assert "QFutureWatcher<UninstallResult>" in window


def test_system_optimization_contains_only_the_four_final_prd_sections():
    catalog = read(SRC / "SystemCatalog.cpp")
    window = read(SRC / "MainWindow.cpp")
    gpu = read(SRC / "GpuOptimizationEngine.cpp")

    for tab in ["办公稳定", "电竞提帧", "显卡专属", "高级管控"]:
        assert tab in window

    for token in [
        "关闭启动应用延迟",
        "关闭系统推荐与广告内容",
        "适度精简窗口动画",
        "清理后台临时缓存",
        "切换高性能电源并关闭 CPU 节能",
        "关闭系统通知",
        "禁用系统还原",
        "systemRestore/disabled",
        "关闭休眠",
        "禁用磁盘索引",
        "Windows 自动更新：一键禁用",
        "Windows 自动更新：一键开启",
        "Defender：临时终止实时防护",
        "Defender：永久禁用防护",
        "Defender：一键恢复开启",
        "Edge：一键静默安装",
        "Edge：一键彻底删除",
        "一键修复浏览器主页篡改",
    ]:
        assert token in catalog

    for token in [
        "NVIDIA 一键调优",
        "AMD 一键调优",
        "nvidia-smi",
        "NVAPI",
        "ADLX",
        "setTabVisible",
    ]:
        assert token in gpu + window
    assert "NvidiaNvapiBridge" in gpu
    assert "AmdAdlxBridge" in gpu
    assert "Get-WmiObject Win32_VideoController" in gpu
    assert '\\"{0}`t{1}`t{2}\\"' in gpu
    assert "'{0}`t{1}`t{2}'" not in gpu
    assert "Get-CimInstance Win32_VideoController" not in gpu
    assert "NV_Cache" not in gpu


def test_file_manager_has_disk_switch_filters_operations_visualization_and_only_prd_migrations():
    header = read(SRC / "FileManagementEngine.h")
    engine = read(SRC / "FileManagementEngine.cpp")
    window = read(SRC / "MainWindow.cpp")

    for token in [
        "刷新磁盘",
        "C盘",
        "D盘",
        "磁盘可视化",
        "文件筛选与批量操作",
        "视频",
        "图片",
        "安装包",
        "压缩包",
        "文档",
        "跨盘复制",
        "移动",
        "批量重命名",
        "批量删除",
        "文件粉碎",
        "文件夹权限修复",
        "迁移并生成快捷方式",
        "QGraphicsView",
        "drawTreemap",
    ]:
        assert token in header + engine + window

    migration_catalog = engine.split("QVector<MigrationFolder> FileManagementEngine::migrationCatalog", 1)[1].split(
        "QVector<MigrationFolder> FileManagementEngine::scanMigrationFolders", 1
    )[0]
    for key in ["desktop", "documents", "downloads", "pictures", "videos", "appdata_cache", "temp"]:
        assert f'QStringLiteral("{key}")' in migration_catalog
    for removed in ["favorites", "inetcache", "cookies", "contacts", "links", "searches", "music", "savedgames"]:
        assert f'QStringLiteral("{removed}")' not in migration_catalog

    for token in [
        "User Shell Folders",
        "Shell Folders",
        "TEMP",
        "TMP",
        "WM_SETTINGCHANGE",
        "restorePersonalFolder",
        "还原所有迁移目录",
        "目标磁盘为 %1 格式，不支持连接点",
        "迁移目标已存在内容，为避免混入旧文件和破坏回滚",
    ]:
        assert token in engine + window


def test_repair_global_navigation_logging_tooltips_and_admin_flow_are_present():
    catalog = read(SRC / "SystemCatalog.cpp")
    window = read(SRC / "MainWindow.cpp")
    main = read(SRC / "main.cpp")
    workflow = read(ROOT / ".github" / "workflows" / "build-cpp-qt.yml")

    for token in [
        "SFC 系统文件修复",
        "CHKDSK 磁盘安全扫描",
        "DNS 刷新",
        "Winsock 网络重置",
        "DISM 系统镜像修复",
        "磁盘错误深度修复",
        "系统更新组件修复",
        "缓存重置修复",
    ]:
        assert token in catalog

    for token in [
        "推荐安全修复",
        "深度系统修复",
        "单独执行",
        "createTopNavigation",
        "createBottomBar",
        "全部操作日志",
        "operations.log",
        "全局一键还原",
        "功能说明:",
        "适用场景:",
        "风险等级:",
        "打开文件位置",
        "复制完整路径",
        "查看底层命令",
        "添加白名单",
    ]:
        assert token in window

    for token in ["runas", "CheckTokenMembership", "管理员权限受限", "isElevated"]:
        assert token in main
    assert "RequestExecutionLevel admin" in workflow
    assert "ContextActionRole" in window
    assert "runContextAction" in window


def test_removed_features_are_absent_from_the_compiled_cpp_product():
    compiled_sources = "\n".join(
        read(path)
        for path in [
            SRC / "MainWindow.h",
            SRC / "MainWindow.cpp",
            SRC / "CleanupEngine.h",
            SRC / "CleanupEngine.cpp",
            SRC / "SystemCatalog.h",
            SRC / "SystemCatalog.cpp",
        ]
    )
    for token in [
        "QQ专清",
        "微信专清",
        "账号会员",
        "兑换卡密",
        "BX(优化)",
        "碎片整理",
        "规则商店",
        "重复文件",
        "空文件夹",
        "Dism++",
    ]:
        assert token not in compiled_sources


def test_windows_release_builds_qt5_x86_and_x64_packages():
    cmake = read(ROOT / "CMakeLists.txt")
    workflow = read(ROOT / ".github" / "workflows" / "build-cpp-qt.yml")

    for token in ["Qt6 Qt5", "Qt${QT_VERSION_MAJOR}::Widgets", "Qt${QT_VERSION_MAJOR}::Concurrent"]:
        assert token in cmake
    for token in [
        'version: "5.15.2"',
        "arch: x64",
        "arch: x86",
        "win64_msvc2019_64",
        "win32_msvc2019",
        "CDriveCleanerQt-x64-Portable.exe",
        "CDriveCleanerQt-x86-Portable.exe",
    ]:
        assert token in workflow
    assert "ctest --test-dir build" in workflow
    assert "CDriveCleanerEngineTests" in cmake
    assert cmake.count("target_compile_options(") >= 2
    assert cmake.count("/utf-8") >= 2
