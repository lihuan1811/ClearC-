from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT
SRC = CPP / "src"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_cpp_qt_project_has_build_entrypoints():
    cmake = read(CPP / "CMakeLists.txt")
    workflow = read(ROOT / ".github" / "workflows" / "build-cpp-qt.yml")

    for source in [
        "src/main.cpp",
        "src/CleanupEngine.cpp",
        "src/DismRuleScanner.cpp",
        "src/FileManagementEngine.cpp",
        "src/GpuOptimizationEngine.cpp",
        "src/SystemCatalog.cpp",
        "src/AccountStore.cpp",
        "src/MainWindow.cpp",
    ]:
        assert source in cmake

    assert "Qt6" in cmake
    assert "Widgets" in cmake
    assert "Concurrent" in cmake
    assert "CDriveCleanerQt" in cmake

    assert "jurplel/install-qt-action" in workflow
    assert "windows-2022" in workflow
    assert '"src/**"' in workflow
    assert '"CMakeLists.txt"' in workflow
    assert '"cpp_qt/**"' not in workflow
    assert "cmake -S ." in workflow
    assert "windeployqt --compiler-runtime" in workflow
    assert "Bundle MSVC runtime" in workflow
    assert "Microsoft.VisualStudio.Component.VC.Redist.14.Latest" in workflow
    assert "msvcp140.dll" in workflow
    assert "vcruntime140.dll" in workflow
    assert "vcruntime140_1.dll" in workflow
    assert "rules/dismpp" in workflow
    assert "CDriveCleanerQt.zip" in workflow
    assert "CDriveCleanerQt-Portable.exe" in workflow
    assert "choco install nsis" in workflow
    assert "makensis.exe" in workflow
    assert "gh release create" in workflow


def test_cleanup_engine_ports_c_drive_rules_and_cleaning_modes():
    source = read(SRC / "CleanupEngine.cpp")

    for token in [
        "Recommended",
        "Professional",
        "SelectAll",
        "allowScanOnly",
        "backupFile",
        "deletePath",
        "scanLargeFilesAsync",
        "scanDuplicateFilesAsync",
        "EdgeCore",
        "GameViewer",
        "webviewcache",
        "DrvPath",
        "Panther",
        "WindowsApps",
        "WinSxS",
        "Windows Defender",
        "catroot2",
        "SoftwareDistribution",
        "Prefetch",
        "SystemTemp",
        "Chrome 缓存",
        "Firefox 缓存",
        "浏览器 Cookie",
        "浏览器保存密码",
        "VS Code 缓存",
        "Cursor 缓存",
        "Discord 缓存",
        "Steam 网页缓存",
        "Notion 缓存",
        "OBS Studio 缓存",
        "C盘大型安装包/压缩包/镜像",
        "系统事件日志",
        "微信专清",
        "QQ专清",
        "Dism++",
    ]:
        assert token in source


def test_dismpp_rule_scanner_is_ported_and_packaged():
    header = read(SRC / "DismRuleScanner.h")
    source = read(SRC / "DismRuleScanner.cpp")
    cmake = read(CPP / "CMakeLists.txt")

    for token in [
        "class DismRuleScanner",
        "QXmlStreamReader",
        "RootPath",
        "Query",
        "Excluded",
        "isDynamicExpression",
        "expandEnvironmentPath",
    ]:
        assert token in header + source

    assert "rules/dismpp/Data.xml" in cmake
    assert (CPP / "rules" / "dismpp" / "Data.xml").exists()


def test_cleanup_engine_protects_scan_only_items_and_backup_failures():
    header = read(SRC / "CleanupEngine.h")
    source = read(SRC / "CleanupEngine.cpp")

    for token in [
        "struct CleanResult",
        "cleanEntriesDetailed",
        "attemptedCount",
        "skippedCount",
        "QStringList errors",
    ]:
        assert token in header

    for token in [
        "if (entry.scanOnly && !options.allowScanOnly)",
        "privacySensitive",
        "if (!entry.privacySensitive)",
        "browser_extensions",
        "const QStringList targets = entry.files;",
        "backupFile(path, backupRoot, &backupError)",
        "result.errors.push_back(backupError)",
        "result.errors.push_back(error)",
        "QCryptographicHash::hash(normalizedPath.toUtf8(), QCryptographicHash::Sha256)",
        "BackupInfo",
        "restoreBackupItem",
        "pruneBackups",
    ]:
        assert token in header + source


def test_system_catalog_ports_optimization_bx_and_repair_commands():
    source = read(SRC / "SystemCatalog.cpp")

    for token in [
        "开机加速",
        "运行内存",
        "windowsStartupRegistryItems",
        "windowsStartupFolderItems",
        "windowsMemoryProcessItems",
        "当前用户 / 登录启动项",
        "运行进程",
        "tasklist /FO CSV",
        "系统优化",
        "隐私清理",
        "浏览器账号与插件数据",
        "注册表清理",
        "BX(优化)",
        "基本",
        "最佳",
        "全局全屏优化",
        "交付优化",
        "SFC 系统文件修复",
        "DISM 系统镜像修复",
        "CHKDSK",
        "Winsock",
        "DNS",
        "SoftwareDistribution",
        "catroot2",
        "runCommand",
        "WindowsOptimizationAction",
        "刷新 DNS 缓存",
        "重置 Winsock",
        "重置 TCP/IP",
        "切换高性能电源计划",
        "关闭休眠释放 hiberfil.sys",
        "关闭 Game DVR 录制",
        "关闭启动应用延迟",
        "关闭透明效果",
        "开启存储感知",
        "关闭搜索高亮",
        "调整为最佳性能视觉效果",
        "NVIDIA 一键调优",
        "AMD 一键调优",
        "Windows 自动更新：一键禁用",
        "Windows 安全中心：一键恢复",
        "Edge 工具箱：一键彻底删除 Edge",
        "一键修复浏览器主页篡改",
        "广告清理",
        "无效快捷方式",
        "定时任务",
        "BCUninstaller",
    ]:
        assert token in source


def test_account_store_ports_login_register_and_card_redemption():
    source = read(SRC / "AccountStore.cpp")

    for token in [
        "registerUser",
        "login",
        "redeemCard",
        "logout",
        "WINCLEANER-WEEK-DEMO",
        "WINCLEANER-MONTH-DEMO",
        "WINCLEANER-QUARTER-DEMO",
        "WINCLEANER-YEAR-DEMO",
        "WINCLEANER-VIP-30D",
        "WINCLEANER-VIP-90D",
        "WINCLEANER-VIP-365D",
        "deviceId",
        "remoteAccountApiBaseUrl",
        "QCryptographicHash",
        "account.json",
    ]:
        assert token in source


def test_main_window_ports_all_expected_pages_and_async_workflows():
    source = read(SRC / "MainWindow.cpp")

    for token in [
        "C DiskGlow",
        "C盘清理",
        "系统优化",
        "BX(优化)",
        "软件卸载",
        "文件管理",
        "系统修复",
        "账号会员",
        "推荐",
        "专业",
        "全选",
        "一键扫描",
        "清理选中",
        "一键清理",
        "扫描大文件",
        "扫描重复文件",
        "扫描碎片",
        "整理碎片",
        "登录",
        "注册",
        "兑换卡密",
        "QtConcurrent::run",
        "QFutureWatcher",
        "heroPanel",
        "resultCard",
        "statusStrip",
        "featureCard",
        "备份管理",
        "文件夹占用",
        "空文件夹",
        "文件迁移",
        "用户安装程序",
        "微软商店应用",
        "storeUninstallTable_",
        "populateUninstallTable",
        "广告清理",
        "系统目录一键迁移专区",
        "Windows 设置优化",
        "规则商店",
        "查看全部操作日志",
        "全局一键还原所有修改",
    ]:
        assert token in source


def test_main_window_guards_destructive_cleanup_and_file_deletes():
    source = read(SRC / "MainWindow.cpp")

    for token in [
        "bool MainWindow::allowScanOnly() const {\n    return currentCleanMode() != CleanupEngine::CleanMode::Recommended;",
        "confirmDestructiveAction",
        "当前模式包含专业项",
        "推荐模式只处理推荐清理项",
        "scanWatcher_->isRunning()",
        "cleanEntriesDetailed",
        "fileTabs_->currentWidget() == largeFileTable_",
        "fileTabs_->currentWidget() == duplicateFileTable_",
        "fileTabs_->currentWidget() == emptyFolderTable_",
        "将永久删除选中的 %1 个文件",
        "CleanupEngine::deletePath(path, &error)",
        "table->removeRow(row)",
    ]:
        assert token in source


def test_file_management_engine_ports_flutter_file_tools():
    header = read(SRC / "FileManagementEngine.h")
    source = read(SRC / "FileManagementEngine.cpp")

    for token in [
        "class FileManagementEngine",
        "FolderUsageEntry",
        "ManagedFileEntry",
        "EmptyFolderEntry",
        "MigrationFolder",
        "scanFolderUsage",
        "scanEmptyFolders",
        "shredFiles",
        "copyFiles",
        "moveFiles",
        "renameFile",
        "migratePersonalFolder",
        "restorePersonalFolder",
        "createJunction",
        "repairFolderPermission",
    ]:
        assert token in header + source


def test_cpp_gpu_optimization_module_detects_supported_gpu_actions():
    header = read(SRC / "GpuOptimizationEngine.h")
    source = read(SRC / "GpuOptimizationEngine.cpp")
    main_window_header = read(SRC / "MainWindow.h")
    main_window = read(SRC / "MainWindow.cpp")

    for token in [
        "class GpuOptimizationEngine",
        "struct GpuDeviceInfo",
        "struct GpuOptimizationAction",
        "detectDevices",
        "supportedActions",
        "runAction",
        "restoreAction",
        "Win32_VideoController",
        "nvidia-smi",
        "nvapi64.dll",
        "NVAPI",
        "ADLX",
        "AMDSoftware",
        "Intel",
        "driverVersion",
        "memoryMB",
        "temperatureC",
        "loadPercent",
        "supported",
        "requiresConfirmation",
        "revertCommands",
    ]:
        assert token in header + source

    for token in [
        "显卡优化",
        "createGpuPage",
        "gpuInfoTable_",
        "gpuActionTable_",
        "gpuLog_",
        "refreshGpuInfo",
        "populateGpuActions",
        "runGpuAction",
        "confirmDestructiveAction",
        "showOperationLog",
    ]:
        assert token in main_window_header + main_window


def test_file_migration_handles_cross_volume_and_junction_restore_safely():
    source = read(SRC / "FileManagementEngine.cpp")

    for token in [
        "copyDirectoryContents",
        "removeDirectoryTree",
        "junctionTarget",
        "removeJunction",
        "GetFinalPathNameByHandleW",
        "RemoveDirectoryW",
    ]:
        assert token in source

    assert "copyOrMove({folder.path}, targetRoot, true)" not in source
    assert "QDir(folder.path).removeRecursively();" not in source


def test_backup_directory_selection_is_used_by_cleaning_and_manager():
    header = read(SRC / "MainWindow.h")
    source = read(SRC / "MainWindow.cpp")

    for token in [
        "QString backupRoot_",
        "backupRoot_ = selected;",
        "options.backupRoot = backupRoot_;",
        "CleanupEngine::backupInfo(backupRoot_)",
        "CleanupEngine::pruneBackups(backupRoot_)",
    ]:
        assert token in header + source


def test_docx_change_requests_are_reflected_in_cpp_ui():
    header = read(SRC / "MainWindow.h")
    source = read(SRC / "MainWindow.cpp")

    for token in [
        "backupMode_->setChecked(true)",
        "refreshDiskInfo();\n    startScan();",
        "return currentCleanMode() != CleanupEngine::CleanMode::Recommended;",
        "table->topLevelItem(i)->setExpanded(false)",
        "defaultMigrationTargetRoot",
        "选择目标",
        "defaultMigrationKey",
        "请先勾选需要迁移的个人文件夹",
        "Get-AppxPackage",
        "Remove-AppxPackage",
        "uninstallTabs_",
        "storeUninstallTable_",
    ]:
        assert token in header + source


def test_clean_page_is_split_into_scoped_modules_and_fixed_window():
    header = read(SRC / "MainWindow.h")
    source = read(SRC / "MainWindow.cpp")
    cleanup_header = read(SRC / "CleanupEngine.h")
    cleanup_source = read(SRC / "CleanupEngine.cpp")

    for token in [
        "setWindowFlag(Qt::WindowMaximizeButtonHint, false)",
        "setFixedSize(windowSize)",
        "QApplication::primaryScreen()",
        "cleanSidebarButton",
        "CleanModule::CDrive",
        "CleanModule::QQ",
        "CleanModule::WeChat",
        "QQ专清",
        "微信专清",
        "不包含 QQ/微信专清",
        "currentScanScope",
        "entriesForCurrentCleanModule",
        "cleanupEntryMatchesCurrentModule",
        "CleanupEngine::ScanScope::QQ",
        "CleanupEngine::ScanScope::WeChat",
    ]:
        assert token in header + source

    for token in [
        "enum class ScanScope",
        "ruleMatchesScanScope",
        "scope == ScanScope::QQ",
        "scope == ScanScope::WeChat",
        "scope == ScanScope::All || scope == ScanScope::CDrive",
        "rule.id.startsWith(QStringLiteral(\"qq_\"))",
        "rule.id.startsWith(QStringLiteral(\"wechat_\"))",
    ]:
        assert token in cleanup_header + cleanup_source


def test_bx_matches_python_qt_location_and_file_usage_has_treemap():
    header = read(SRC / "MainWindow.h")
    source = read(SRC / "MainWindow.cpp")
    sidebar = source.split("QWidget* MainWindow::createSidebar", 1)[1].split("QWidget* MainWindow::createCleanPage", 1)[0]
    optimize_page = source.split("QWidget* MainWindow::createOptimizePage", 1)[1].split("QWidget* MainWindow::createGpuPage", 1)[0]

    assert "BX(优化)" not in sidebar
    for token in [
        "tab == QStringLiteral(\"系统优化\")",
        "optimizerTabs_->addTab(createBxPage(), tab)",
        "tab == QStringLiteral(\"系统优化\") && bxTable_",
        "populateBxItems",
        "applyBxOptimization",
    ]:
        assert token in optimize_page + source

    for token in [
        "folderUsagePage_",
        "folderUsageMapScene_",
        "folderUsageMapView_",
        "QGraphicsScene",
        "QGraphicsView#folderUsageTreemap",
        "文件大小方格可视化",
        "drawUsageTreemap",
        "populateFolderUsageTreemap(entries)",
        "fileTabs_->addTab(folderUsagePage_, QStringLiteral(\"文件夹占用\"))",
        "fileTabs_->setCurrentWidget(folderUsagePage_)",
    ]:
        assert token in header + source
