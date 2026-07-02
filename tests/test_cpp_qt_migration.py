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
    assert "windeployqt" in workflow
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
        "微信专清",
        "QQ专清",
    ]:
        assert token in source


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
        "const QStringList targets = entry.files;",
        "backupFile(path, backupRoot, &backupError)",
        "result.errors.push_back(backupError)",
        "result.errors.push_back(error)",
        "QCryptographicHash::hash(normalizedPath.toUtf8(), QCryptographicHash::Sha256)",
    ]:
        assert token in source


def test_system_catalog_ports_optimization_bx_and_repair_commands():
    source = read(SRC / "SystemCatalog.cpp")

    for token in [
        "开机加速",
        "运行内存",
        "系统优化",
        "隐私清理",
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
    ]:
        assert token in source


def test_main_window_guards_destructive_cleanup_and_file_deletes():
    source = read(SRC / "MainWindow.cpp")

    for token in [
        "bool MainWindow::allowScanOnly() const {\n    return false;",
        "confirmDestructiveAction",
        "仅统计项目不会被删除",
        "scanWatcher_->isRunning()",
        "cleanEntriesDetailed",
        "currentTab != 0 && currentTab != 1",
        "将永久删除选中的 %1 个文件",
        "CleanupEngine::deletePath(path, &error)",
        "table->removeRow(row)",
    ]:
        assert token in source
