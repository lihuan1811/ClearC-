#include "MainWindow.h"

#include <QtConcurrent/QtConcurrent>

#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaObject>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

QTableWidgetItem* textItem(const QString& value) {
    auto* item = new QTableWidgetItem(value);
    item->setFlags(item->flags() ^ Qt::ItemIsEditable);
    return item;
}

QString cDriveRoot() {
#ifdef Q_OS_WIN
    return QStringLiteral("C:/");
#else
    return QDir::homePath();
#endif
}

void setTableStretch(QTableWidget* table) {
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
}

QString cleanResultMessage(const CleanResult& result) {
    QString message = QStringLiteral("处理完成: %1\n已处理文件: %2\n已跳过文件: %3")
        .arg(CleanupEngine::formatSize(result.cleanedBytes))
        .arg(result.deletedCount)
        .arg(result.skippedCount);
    if (!result.errors.isEmpty()) {
        message += QStringLiteral("\n\n失败明细:\n") + result.errors.mid(0, 12).join(QStringLiteral("\n"));
        if (result.errors.size() > 12) {
            message += QStringLiteral("\n... 还有 %1 条").arg(result.errors.size() - 12);
        }
    }
    return message;
}

bool confirmDestructiveAction(QWidget* parent, const QString& title, const QString& message) {
    return QMessageBox::question(
        parent,
        title,
        message,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    ) == QMessageBox::Yes;
}

QString adBlockHostsCommand(bool enable) {
    QStringList values = {QStringLiteral("# C DiskGlow ad block")};
    for (const QString& domain : SystemCatalog::adBlockDomains()) {
        values.push_back(QStringLiteral("0.0.0.0 %1").arg(domain));
    }
    const QString valueArray = values.join(QStringLiteral("','"));
    const QString domainArray = SystemCatalog::adBlockDomains().join(QStringLiteral("','"));

    if (enable) {
        return QStringLiteral("powershell -NoProfile -Command \"$p=Join-Path $env:SystemRoot 'System32\\drivers\\etc\\hosts'; Add-Content -Path $p -Value @('%1'); ipconfig /flushdns\"")
            .arg(valueArray);
    }
    return QStringLiteral("powershell -NoProfile -Command \"$p=Join-Path $env:SystemRoot 'System32\\drivers\\etc\\hosts'; $d=@('%1'); if (Test-Path $p) { $content=Get-Content -Path $p | Where-Object { $line=$_; -not ($line -match 'C DiskGlow ad block' -or ($d | Where-Object { $line -match [regex]::Escape($_) })) }; Set-Content -Path $p -Value $content }; ipconfig /flushdns\"")
        .arg(domainArray);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("C DiskGlow"));
    resize(1180, 760);

    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("appRoot"));
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(createSidebar());

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("contentArea"));
    pages_->addWidget(createCleanPage());
    pages_->addWidget(createOptimizePage());
    pages_->addWidget(createBxPage());
    pages_->addWidget(createUninstallPage());
    pages_->addWidget(createFilePage());
    pages_->addWidget(createRepairPage());
    pages_->addWidget(createAccountPage());
    layout->addWidget(pages_, 1);
    setCentralWidget(central);

    applyStyle();
    selectPage(0);
    refreshDiskInfo();
    populateStartupItems();
    populateMemoryItems();
    populateSystemOptimizationItems();
    populatePrivacyItems();
    populateRegistryItems();
    populateNvidiaItems();
    populateAmdItems();
    populateMaintenanceItems();
    populateEdgeToolkitItems();
    populateWindowsOptimizationActions();
    populateBxItems();
    refreshInstalledApps();
    populateRepairTable();
    refreshAccountState();
}

QWidget* MainWindow::createSidebar() {
    auto* sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(176);
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(16, 18, 16, 18);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("C DiskGlow"), sidebar);
    title->setObjectName(QStringLiteral("brandTitle"));
    layout->addWidget(title);
    layout->addSpacing(12);

    const QStringList labels = {
        QStringLiteral("C盘清理"),
        QStringLiteral("系统优化"),
        QStringLiteral("BX(优化)"),
        QStringLiteral("软件卸载"),
        QStringLiteral("文件管理"),
        QStringLiteral("系统修复"),
        QStringLiteral("账号会员"),
    };
    for (int i = 0; i < labels.size(); ++i) {
        auto* button = sidebarButton(labels.at(i), i);
        navButtons_.push_back(button);
        layout->addWidget(button);
    }
    layout->addStretch();
    return sidebar;
}

QWidget* MainWindow::createCleanPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    auto* heroPanel = new QFrame(page);
    heroPanel->setObjectName(QStringLiteral("heroPanel"));
    auto* heroLayout = new QHBoxLayout(heroPanel);
    heroLayout->setContentsMargins(18, 16, 18, 16);
    heroLayout->setSpacing(18);
    heroLayout->addWidget(pageHeader(
        QStringLiteral("C DiskGlow"),
        QStringLiteral("扫描缓存、日志、更新残留、EdgeCore、Dism++ 和 AppData 路径")
    ), 2);

    auto* stats = new QHBoxLayout();
    totalSpaceLabel_ = new QLabel(QStringLiteral("总量 --"), page);
    usedSpaceLabel_ = new QLabel(QStringLiteral("已用 --"), page);
    freeSpaceLabel_ = new QLabel(QStringLiteral("可用 --"), page);
    reclaimSpaceLabel_ = new QLabel(QStringLiteral("可释放 --"), page);
    for (QLabel* label : {totalSpaceLabel_, usedSpaceLabel_, freeSpaceLabel_, reclaimSpaceLabel_}) {
        label->setObjectName(QStringLiteral("statPill"));
        stats->addWidget(label);
    }
    stats->addStretch();
    heroLayout->addLayout(stats, 3);
    layout->addWidget(heroPanel);

    auto* controls = new QHBoxLayout();
    recommendedMode = new QCheckBox(QStringLiteral("推荐"), page);
    professionalMode = new QCheckBox(QStringLiteral("专业"), page);
    selectAllMode = new QCheckBox(QStringLiteral("全选"), page);
    recommendedMode->setChecked(true);
    connect(recommendedMode, &QCheckBox::clicked, this, [this] { updateModeSelection(recommendedMode); });
    connect(professionalMode, &QCheckBox::clicked, this, [this] { updateModeSelection(professionalMode); });
    connect(selectAllMode, &QCheckBox::clicked, this, [this] { updateModeSelection(selectAllMode); });
    controls->addWidget(recommendedMode);
    controls->addWidget(professionalMode);
    controls->addWidget(selectAllMode);
    simulateMode_ = new QCheckBox(QStringLiteral("模拟模式"), page);
    backupMode_ = new QCheckBox(QStringLiteral("删除前备份"), page);
    backupMode_->setChecked(true);
    controls->addWidget(simulateMode_);
    controls->addWidget(backupMode_);
    auto* backupDirButton = secondaryButton(QStringLiteral("备份目录"));
    connect(backupDirButton, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择备份目录"), CleanupEngine::backupRoot());
        if (!selected.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("备份目录"), QStringLiteral("本次清理备份目录将使用: %1").arg(selected));
        }
    });
    auto* backupManagerButton = secondaryButton(QStringLiteral("备份管理"));
    connect(backupManagerButton, &QPushButton::clicked, this, &MainWindow::openBackupManager);
    controls->addWidget(backupDirButton);
    controls->addWidget(backupManagerButton);
    controls->addStretch();

    auto* scanButton = primaryButton(QStringLiteral("一键扫描"));
    connect(scanButton, &QPushButton::clicked, this, &MainWindow::startScan);
    auto* cleanSelectedButton = secondaryButton(QStringLiteral("清理选中"));
    connect(cleanSelectedButton, &QPushButton::clicked, this, &MainWindow::cleanSelected);
    auto* cleanAllButton = primaryButton(QStringLiteral("一键清理"));
    connect(cleanAllButton, &QPushButton::clicked, this, &MainWindow::cleanAllForCurrentMode);
    controls->addWidget(scanButton);
    controls->addWidget(cleanSelectedButton);
    controls->addWidget(cleanAllButton);
    layout->addLayout(controls);

    currentScanPath = new QLabel(QStringLiteral("等待扫描。"), page);
    scanProgress_ = new QProgressBar(page);
    scanProgress_->setRange(0, 100);
    scanProgress_->setValue(0);
    layout->addWidget(currentScanPath);
    layout->addWidget(scanProgress_);

    auto* resultCard = new QFrame(page);
    resultCard->setObjectName(QStringLiteral("resultCard"));
    auto* resultLayout = new QVBoxLayout(resultCard);
    resultLayout->setContentsMargins(16, 14, 16, 14);
    resultLayout->setSpacing(10);

    cleanupTree_ = new QTreeWidget(resultCard);
    cleanupTree_->setObjectName(QStringLiteral("resultTree"));
    cleanupTree_->setColumnCount(4);
    cleanupTree_->setHeaderLabels({QStringLiteral("清理项"), QStringLiteral("大小"), QStringLiteral("模式"), QStringLiteral("路径")});
    cleanupTree_->header()->setStretchLastSection(true);
    resultLayout->addWidget(cleanupTree_, 1);
    layout->addWidget(resultCard, 1);

    auto* statusStrip = new QFrame(page);
    statusStrip->setObjectName(QStringLiteral("statusStrip"));
    auto* statusLayout = new QHBoxLayout(statusStrip);
    statusLayout->setContentsMargins(14, 8, 14, 8);
    auto* status = new QLabel(QStringLiteral("准备扫描 C 盘可清理路径"), statusStrip);
    status->setObjectName(QStringLiteral("statusLabel"));
    statusLayout->addWidget(status);
    layout->addWidget(statusStrip);

    scanWatcher_ = new QFutureWatcher<CleanupScanResult>(this);
    connect(scanWatcher_, &QFutureWatcher<CleanupScanResult>::finished, this, &MainWindow::finishScan);
    return page;
}

QWidget* MainWindow::createOptimizePage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);

    layout->addWidget(pageHeader(
        QStringLiteral("系统优化"),
        QStringLiteral("开机启动、运行内存、Windows 设置优化、广告清理、显卡调优、Edge 工具箱和定时任务集中处理。")
    ));

    optimizerTabs_ = new QTabWidget(page);
    const QStringList tabs = {
        QStringLiteral("开机加速"),
        QStringLiteral("运行内存"),
        QStringLiteral("系统优化"),
        QStringLiteral("隐私清理"),
        QStringLiteral("注册表清理"),
        QStringLiteral("Windows 设置优化"),
        QStringLiteral("维护工具"),
        QStringLiteral("NVIDIA 一键调优"),
        QStringLiteral("AMD 一键调优"),
        QStringLiteral("Edge 工具箱"),
    };
    for (const QString& tab : tabs) {
        if (tab == QStringLiteral("Windows 设置优化")) {
            windowsOptimizationTable_ = new QTableWidget(page);
            windowsOptimizationTable_->setColumnCount(7);
            windowsOptimizationTable_->setHorizontalHeaderLabels({QStringLiteral("分类"), QStringLiteral("项目"), QStringLiteral("风险"), QStringLiteral("说明"), QStringLiteral("执行"), QStringLiteral("恢复"), QStringLiteral("管理员")});
            setTableStretch(windowsOptimizationTable_);
            optimizerTabs_->addTab(windowsOptimizationTable_, tab);
            continue;
        }
        auto* tree = new QTreeWidget(page);
        tree->setColumnCount(3);
        tree->setHeaderLabels({QStringLiteral("项目"), QStringLiteral("位置/说明"), QStringLiteral("操作")});
        tree->header()->setStretchLastSection(false);
        tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        optimizerTables_.insert(tab, tree);
        optimizerTabs_->addTab(tree, tab);
    }
    layout->addWidget(optimizerTabs_, 1);

    auto* buttonRow = new QHBoxLayout();
    auto* applyButton = primaryButton(QStringLiteral("一键优化"));
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyCurrentOptimizationTab);
    auto* restoreButton = secondaryButton(QStringLiteral("全局一键还原所有修改"));
    connect(restoreButton, &QPushButton::clicked, this, &MainWindow::runGlobalRestore);
    auto* logButton = secondaryButton(QStringLiteral("查看全部操作日志"));
    connect(logButton, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, QStringLiteral("查看全部操作日志"), operationLog_ ? operationLog_->toPlainText() : QStringLiteral("暂无日志。"));
    });
    buttonRow->addStretch();
    buttonRow->addWidget(logButton);
    buttonRow->addWidget(restoreButton);
    buttonRow->addWidget(applyButton);
    layout->addLayout(buttonRow);

    operationLog_ = new QTextEdit(page);
    operationLog_->setReadOnly(true);
    operationLog_->setObjectName(QStringLiteral("operationLog"));
    operationLog_->setMaximumHeight(96);
    operationLog_->setPlaceholderText(QStringLiteral("实时活动日志"));
    layout->addWidget(operationLog_);
    return page;
}

QWidget* MainWindow::createBxPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);

    auto* title = new QLabel(QStringLiteral("BX(优化)"), page);
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);

    auto* modes = new QHBoxLayout();
    auto* basic = secondaryButton(QStringLiteral("基本"));
    auto* best = primaryButton(QStringLiteral("最佳"));
    connect(basic, &QPushButton::clicked, this, [this] { applyBxMode(QStringLiteral("basic")); });
    connect(best, &QPushButton::clicked, this, [this] { applyBxMode(QStringLiteral("best")); });
    modes->addWidget(basic);
    modes->addWidget(best);
    modes->addStretch();
    layout->addLayout(modes);

    bxTable_ = new QTableWidget(page);
    bxTable_->setColumnCount(5);
    bxTable_->setHorizontalHeaderLabels({QStringLiteral("启用"), QStringLiteral("分类"), QStringLiteral("项目"), QStringLiteral("风险"), QStringLiteral("目标状态")});
    setTableStretch(bxTable_);
    layout->addWidget(bxTable_, 1);

    bxStatusLabel_ = new QLabel(QStringLiteral("BX(优化) 已就绪。"), page);
    auto* apply = primaryButton(QStringLiteral("应用 BX 优化"));
    connect(apply, &QPushButton::clicked, this, &MainWindow::applyBxOptimization);
    layout->addWidget(bxStatusLabel_);
    layout->addWidget(apply);
    return page;
}

QWidget* MainWindow::createUninstallPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("软件卸载"), page);
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);

    auto* refresh = secondaryButton(QStringLiteral("刷新软件列表"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshInstalledApps);
    layout->addWidget(refresh, 0, Qt::AlignLeft);

    uninstallTable_ = new QTableWidget(page);
    uninstallTable_->setColumnCount(5);
    uninstallTable_->setHorizontalHeaderLabels({QStringLiteral("软件"), QStringLiteral("发布者"), QStringLiteral("版本"), QStringLiteral("卸载命令"), QStringLiteral("操作")});
    setTableStretch(uninstallTable_);
    layout->addWidget(uninstallTable_, 1);
    return page;
}

QWidget* MainWindow::createFilePage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);
    if (fileRoot_.isEmpty()) {
        fileRoot_ = cDriveRoot();
    }

    layout->addWidget(pageHeader(
        QStringLiteral("文件管理"),
        QStringLiteral("大文件、重复文件、文件夹占用、空文件夹、系统目录迁移、碎片整理和 Dism++ 规则商店集中处理。")
    ));

    auto* featureCard = new QFrame(page);
    featureCard->setObjectName(QStringLiteral("featureCard"));
    auto* featureLayout = new QVBoxLayout(featureCard);
    featureLayout->setContentsMargins(14, 12, 14, 12);
    featureLayout->setSpacing(10);

    auto* rootRow = new QHBoxLayout();
    fileRootLabel_ = new QLabel(QStringLiteral("当前目录: %1").arg(QDir::toNativeSeparators(fileRoot_)), featureCard);
    fileRootLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* chooseRoot = secondaryButton(QStringLiteral("选择目录"));
    connect(chooseRoot, &QPushButton::clicked, this, &MainWindow::chooseFileRoot);
    rootRow->addWidget(fileRootLabel_, 1);
    rootRow->addWidget(chooseRoot);
    featureLayout->addLayout(rootRow);

    auto* actionRow = new QHBoxLayout();
    auto* folderUsage = primaryButton(QStringLiteral("扫描文件夹占用"));
    auto* large = primaryButton(QStringLiteral("扫描大文件"));
    auto* duplicate = primaryButton(QStringLiteral("扫描重复文件"));
    auto* empty = secondaryButton(QStringLiteral("扫描空文件夹"));
    auto* deleteButton = secondaryButton(QStringLiteral("删除选中文件"));
    auto* shredButton = secondaryButton(QStringLiteral("彻底删除文件"));
    connect(folderUsage, &QPushButton::clicked, this, &MainWindow::scanFolderUsage);
    connect(large, &QPushButton::clicked, this, &MainWindow::scanLargeFilesAsync);
    connect(duplicate, &QPushButton::clicked, this, &MainWindow::scanDuplicateFilesAsync);
    connect(empty, &QPushButton::clicked, this, &MainWindow::scanEmptyFolders);
    connect(deleteButton, &QPushButton::clicked, this, &MainWindow::deleteSelectedFileItems);
    connect(shredButton, &QPushButton::clicked, this, &MainWindow::shredSelectedFileItems);
    actionRow->addWidget(folderUsage);
    actionRow->addWidget(large);
    actionRow->addWidget(duplicate);
    actionRow->addWidget(empty);
    actionRow->addWidget(deleteButton);
    actionRow->addWidget(shredButton);
    actionRow->addStretch();
    featureLayout->addLayout(actionRow);
    layout->addWidget(featureCard);

    fileTabs_ = new QTabWidget(page);
    folderUsageTree_ = new QTreeWidget(page);
    folderUsageTree_->setObjectName(QStringLiteral("folderUsageTree"));
    folderUsageTree_->setColumnCount(4);
    folderUsageTree_->setHeaderLabels({QStringLiteral("文件夹"), QStringLiteral("占用大小"), QStringLiteral("文件数"), QStringLiteral("占比")});
    folderUsageTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    folderUsageTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    folderUsageTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    folderUsageTree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    largeFileTable_ = new QTableWidget(page);
    largeFileTable_->setColumnCount(3);
    largeFileTable_->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("大小"), QStringLiteral("路径")});
    setTableStretch(largeFileTable_);

    duplicateFileTable_ = new QTableWidget(page);
    duplicateFileTable_->setColumnCount(4);
    duplicateFileTable_->setHorizontalHeaderLabels({QStringLiteral("组"), QStringLiteral("文件"), QStringLiteral("大小"), QStringLiteral("路径")});
    setTableStretch(duplicateFileTable_);

    emptyFolderTable_ = new QTableWidget(page);
    emptyFolderTable_->setColumnCount(1);
    emptyFolderTable_->setHorizontalHeaderLabels({QStringLiteral("空文件夹")});
    setTableStretch(emptyFolderTable_);

    auto* migrationPage = new QWidget(page);
    auto* migrationLayout = new QVBoxLayout(migrationPage);
    migrationLayout->setContentsMargins(12, 12, 12, 12);
    migrationLayout->setSpacing(10);
    auto* migrationTitle = new QLabel(QStringLiteral("系统目录一键迁移专区"), migrationPage);
    migrationTitle->setObjectName(QStringLiteral("sectionTitle"));
    migrationLayout->addWidget(migrationTitle);
    auto* migrationControls = new QHBoxLayout();
    migrationTargetEdit_ = new QLineEdit(migrationPage);
    migrationTargetEdit_->setPlaceholderText(QStringLiteral("目标根目录，例如 D:\\C_DiskGlow_Moved"));
    migrationTargetEdit_->setText(QStringLiteral("D:\\C_DiskGlow_Moved"));
    migrationMoveFiles_ = new QCheckBox(QStringLiteral("迁移时移动原文件"), migrationPage);
    migrationMoveFiles_->setChecked(true);
    auto* refreshMigration = secondaryButton(QStringLiteral("刷新迁移状态"));
    auto* migrate = primaryButton(QStringLiteral("开始迁移"));
    auto* restore = secondaryButton(QStringLiteral("还原选中"));
    connect(refreshMigration, &QPushButton::clicked, this, &MainWindow::refreshMigrationFolders);
    connect(migrate, &QPushButton::clicked, this, &MainWindow::migrateSelectedFolders);
    connect(restore, &QPushButton::clicked, this, &MainWindow::restoreSelectedFolders);
    migrationControls->addWidget(migrationTargetEdit_, 1);
    migrationControls->addWidget(migrationMoveFiles_);
    migrationControls->addWidget(refreshMigration);
    migrationControls->addWidget(migrate);
    migrationControls->addWidget(restore);
    migrationLayout->addLayout(migrationControls);
    migrationTable_ = new QTableWidget(migrationPage);
    migrationTable_->setColumnCount(6);
    migrationTable_->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("目录"), QStringLiteral("状态"), QStringLiteral("占用"), QStringLiteral("原路径"), QStringLiteral("目标路径")});
    setTableStretch(migrationTable_);
    migrationLayout->addWidget(migrationTable_, 1);

    auto* fragmentPage = new QWidget(page);
    auto* fragmentLayout = new QVBoxLayout(fragmentPage);
    fragmentLayout->setContentsMargins(12, 12, 12, 12);
    fragmentLayout->addWidget(new QLabel(QStringLiteral("碎片数 / 碎片文件 / 碎片率 会在扫描后显示在命令输出中。"), fragmentPage));
    auto* scanFragment = secondaryButton(QStringLiteral("扫描碎片"));
    auto* optimizeFragment = primaryButton(QStringLiteral("整理碎片"));
    connect(scanFragment, &QPushButton::clicked, this, &MainWindow::scanFragments);
    connect(optimizeFragment, &QPushButton::clicked, this, &MainWindow::optimizeFragments);
    fragmentLayout->addWidget(scanFragment);
    fragmentLayout->addWidget(optimizeFragment);
    fragmentLayout->addStretch();

    auto* rulesStorePage = new QWidget(page);
    auto* rulesStoreLayout = new QVBoxLayout(rulesStorePage);
    rulesStoreLayout->setContentsMargins(12, 12, 12, 12);
    rulesStoreLayout->setSpacing(10);
    rulesStoreLayout->addWidget(new QLabel(QStringLiteral("规则商店"), rulesStorePage));
    auto* ruleSummary = new QLabel(QStringLiteral("内置 Dism++ Data.xml 规则会随程序打包，清理页的专业模式会读取这些规则。"), rulesStorePage);
    ruleSummary->setWordWrap(true);
    rulesStoreLayout->addWidget(ruleSummary);
    auto* openRules = secondaryButton(QStringLiteral("打开规则目录"));
    connect(openRules, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("rules/dismpp"))));
    });
    rulesStoreLayout->addWidget(openRules, 0, Qt::AlignLeft);
    rulesStoreLayout->addStretch();

    fileTabs_->addTab(folderUsageTree_, QStringLiteral("文件夹占用"));
    fileTabs_->addTab(largeFileTable_, QStringLiteral("大文件"));
    fileTabs_->addTab(duplicateFileTable_, QStringLiteral("重复文件"));
    fileTabs_->addTab(emptyFolderTable_, QStringLiteral("空文件夹"));
    fileTabs_->addTab(migrationPage, QStringLiteral("文件迁移"));
    fileTabs_->addTab(fragmentPage, QStringLiteral("碎片整理"));
    fileTabs_->addTab(rulesStorePage, QStringLiteral("规则商店"));
    layout->addWidget(fileTabs_, 1);

    fileStatusLabel_ = new QLabel(QStringLiteral("文件管理已就绪。"), page);
    layout->addWidget(fileStatusLabel_);
    refreshMigrationFolders();
    return page;
}

QWidget* MainWindow::createRepairPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("系统修复"), page);
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);

    repairTable_ = new QTableWidget(page);
    repairTable_->setColumnCount(5);
    repairTable_->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("项目"), QStringLiteral("风险"), QStringLiteral("说明"), QStringLiteral("命令")});
    setTableStretch(repairTable_);
    layout->addWidget(repairTable_, 1);

    auto* run = primaryButton(QStringLiteral("执行选中修复"));
    connect(run, &QPushButton::clicked, this, &MainWindow::runSelectedRepairs);
    layout->addWidget(run, 0, Qt::AlignRight);
    repairLog_ = new QTextEdit(page);
    repairLog_->setReadOnly(true);
    layout->addWidget(repairLog_);
    return page;
}

QWidget* MainWindow::createAccountPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("账号会员"), page);
    title->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(title);
    accountStateLabel_ = new QLabel(QStringLiteral("未登录"), page);
    layout->addWidget(accountStateLabel_);

    accountEmailEdit_ = new QLineEdit(page);
    accountEmailEdit_->setPlaceholderText(QStringLiteral("账号名称"));
    accountNameEdit_ = new QLineEdit(page);
    accountNameEdit_->setPlaceholderText(QStringLiteral("显示名称"));
    accountPasswordEdit_ = new QLineEdit(page);
    accountPasswordEdit_->setPlaceholderText(QStringLiteral("密码"));
    accountPasswordEdit_->setEchoMode(QLineEdit::Password);
    cardCodeEdit_ = new QLineEdit(page);
    cardCodeEdit_->setPlaceholderText(QStringLiteral("兑换卡密，例如 WINCLEANER-MONTH-DEMO"));
    layout->addWidget(accountEmailEdit_);
    layout->addWidget(accountNameEdit_);
    layout->addWidget(accountPasswordEdit_);
    layout->addWidget(cardCodeEdit_);

    auto* row = new QHBoxLayout();
    auto* registerButton = secondaryButton(QStringLiteral("注册"));
    auto* loginButton = primaryButton(QStringLiteral("登录"));
    auto* redeemButton = primaryButton(QStringLiteral("兑换卡密"));
    auto* logoutButton = secondaryButton(QStringLiteral("退出登录"));
    connect(registerButton, &QPushButton::clicked, this, &MainWindow::registerAccount);
    connect(loginButton, &QPushButton::clicked, this, &MainWindow::loginAccount);
    connect(redeemButton, &QPushButton::clicked, this, &MainWindow::redeemCard);
    connect(logoutButton, &QPushButton::clicked, this, &MainWindow::logoutAccount);
    row->addWidget(registerButton);
    row->addWidget(loginButton);
    row->addWidget(redeemButton);
    row->addWidget(logoutButton);
    row->addStretch();
    layout->addLayout(row);
    layout->addStretch();
    return page;
}

void MainWindow::applyStyle() {
    qApp->setStyleSheet(QStringLiteral(R"(
        QWidget { font-family: "Microsoft YaHei", "Segoe UI", sans-serif; font-size: 13px; color: #1f2937; }
        #appRoot, #contentArea { background: #f4f7f5; }
        #sidebar { background: #0f8f5f; }
        #brandTitle { color: white; font-size: 20px; font-weight: 700; }
        QPushButton { border: 0; border-radius: 6px; padding: 8px 12px; background: #e5e7eb; }
        QPushButton[nav="true"] { color: white; text-align: left; background: transparent; }
        QPushButton[active="true"] { background: rgba(255,255,255,0.22); }
        #primaryButton { background: #10b981; color: white; font-weight: 600; }
        #secondaryButton { background: #e5f7ef; color: #047857; font-weight: 600; }
        #pageTitle { font-size: 24px; font-weight: 700; }
        #pageSubtitle { color: #4b5563; line-height: 1.35; }
        #sectionTitle { font-size: 16px; font-weight: 700; }
        #heroPanel, #resultCard, #featureCard, #statusStrip {
            background: white;
            border: 1px solid #dbe4dd;
            border-radius: 8px;
        }
        #heroPanel { background: #eefaf4; }
        #statusStrip { background: #f9fafb; }
        #statPill { background: white; border: 1px solid #d1d5db; border-radius: 6px; padding: 8px 12px; }
        QTreeWidget, QTableWidget, QTextEdit, QLineEdit {
            background: white;
            border: 1px solid #d1d5db;
            border-radius: 6px;
            padding: 4px;
        }
        QTabWidget::pane { border: 1px solid #d1d5db; background: white; border-radius: 6px; }
        QTabBar::tab { padding: 8px 12px; margin-right: 2px; }
        QTabBar::tab:selected { color: #047857; font-weight: 700; }
    )"));
}

void MainWindow::selectPage(int index) {
    pages_->setCurrentIndex(index);
    for (int i = 0; i < navButtons_.size(); ++i) {
        navButtons_[i]->setProperty("active", i == index);
        navButtons_[i]->style()->unpolish(navButtons_[i]);
        navButtons_[i]->style()->polish(navButtons_[i]);
    }
}

QPushButton* MainWindow::sidebarButton(const QString& text, int index) {
    auto* button = new QPushButton(text, this);
    button->setProperty("nav", true);
    button->setMinimumHeight(38);
    connect(button, &QPushButton::clicked, this, [this, index] { selectPage(index); });
    return button;
}

QPushButton* MainWindow::primaryButton(const QString& text) const {
    auto* button = new QPushButton(text);
    button->setObjectName(QStringLiteral("primaryButton"));
    return button;
}

QPushButton* MainWindow::secondaryButton(const QString& text) const {
    auto* button = new QPushButton(text);
    button->setObjectName(QStringLiteral("secondaryButton"));
    return button;
}

QWidget* MainWindow::pageHeader(const QString& title, const QString& subtitle) const {
    auto* header = new QWidget();
    auto* layout = new QVBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto* titleLabel = new QLabel(title, header);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(subtitle, header);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    subtitleLabel->setWordWrap(true);
    layout->addWidget(titleLabel);
    layout->addWidget(subtitleLabel);
    return header;
}

void MainWindow::showOperationLog(const QString& message) {
    if (!operationLog_) {
        return;
    }
    operationLog_->append(QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), message));
}

void MainWindow::refreshDiskInfo() {
    const DiskInfo info = cleanupEngine_.diskInfo();
    totalSpaceLabel_->setText(QStringLiteral("总量 %1").arg(CleanupEngine::formatSize(info.totalBytes)));
    usedSpaceLabel_->setText(QStringLiteral("已用 %1").arg(CleanupEngine::formatSize(info.usedBytes)));
    freeSpaceLabel_->setText(QStringLiteral("可用 %1").arg(CleanupEngine::formatSize(info.freeBytes)));
}

void MainWindow::startScan() {
    if (scanWatcher_->isRunning()) {
        return;
    }
    cleanupEntries_.clear();
    cleanupTree_->clear();
    scanProgress_->setRange(0, 0);
    currentScanPath->setText(QStringLiteral("正在扫描 C 盘路径..."));
    scanWatcher_->setFuture(QtConcurrent::run([this] {
        return cleanupEngine_.scanSystem([this](const QString& path, int count) {
            QMetaObject::invokeMethod(this, [this, path, count] {
                currentScanPath->setText(QStringLiteral("正在扫描: %1 (%2)").arg(path).arg(count));
            }, Qt::QueuedConnection);
        });
    }));
}

void MainWindow::finishScan() {
    const CleanupScanResult result = scanWatcher_->result();
    cleanupEntries_ = result.entries;
    scanProgress_->setRange(0, 100);
    scanProgress_->setValue(100);
    reclaimSpaceLabel_->setText(QStringLiteral("可释放 %1").arg(CleanupEngine::formatSize(
        currentCleanMode() == CleanupEngine::CleanMode::Recommended ? result.recommendedBytes : result.professionalBytes
    )));
    currentScanPath->setText(QStringLiteral("扫描完成，共 %1 项。").arg(result.entries.size()));
    populateCleanupTree();
    refreshDiskInfo();
}

void MainWindow::populateCleanupTree() {
    cleanupTree_->clear();
    const QVector<CleanupEntry> visible = cleanupEngine_.entriesForMode(cleanupEntries_, currentCleanMode());
    for (int i = 0; i < visible.size(); ++i) {
        const CleanupEntry& entry = visible.at(i);
        auto* row = new QTreeWidgetItem(cleanupTree_);
        row->setText(0, entry.title);
        row->setText(1, CleanupEngine::formatSize(entry.size));
        row->setText(2, entry.scanOnly ? QStringLiteral("仅统计/专业") : QStringLiteral("可清理"));
        row->setText(3, entry.path);
        row->setCheckState(0, entry.scanOnly && !allowScanOnly() ? Qt::Unchecked : Qt::Checked);
        row->setData(0, Qt::UserRole, i);
        if (entry.scanOnly && !allowScanOnly()) {
            row->setDisabled(true);
        }
    }
}

void MainWindow::updateModeSelection(QCheckBox* changed) {
    for (QCheckBox* box : {recommendedMode, professionalMode, selectAllMode}) {
        box->setChecked(box == changed);
    }
    populateCleanupTree();
}

CleanupEngine::CleanMode MainWindow::currentCleanMode() const {
    if (selectAllMode && selectAllMode->isChecked()) {
        return CleanupEngine::CleanMode::SelectAll;
    }
    if (professionalMode && professionalMode->isChecked()) {
        return CleanupEngine::CleanMode::Professional;
    }
    return CleanupEngine::CleanMode::Recommended;
}

QVector<CleanupEntry> MainWindow::selectedCleanupEntries() const {
    QVector<CleanupEntry> modeEntries = cleanupEngine_.entriesForMode(cleanupEntries_, currentCleanMode());
    QVector<CleanupEntry> selected;
    for (int i = 0; i < cleanupTree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = cleanupTree_->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked) {
            continue;
        }
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < modeEntries.size()) {
            selected.push_back(modeEntries.at(index));
        }
    }
    return selected;
}

bool MainWindow::allowScanOnly() const {
    return false;
}

void MainWindow::cleanSelected() {
    if (scanWatcher_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("清理选中"), QStringLiteral("扫描进行中，请等待扫描完成后再清理。"));
        return;
    }
    const QVector<CleanupEntry> selected = selectedCleanupEntries();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("清理选中"), QStringLiteral("请先勾选需要清理的项目。"));
        return;
    }
    CleanOptions options;
    options.simulate = simulateMode_->isChecked();
    options.backup = backupMode_->isChecked();
    options.allowScanOnly = allowScanOnly();
    if (!options.simulate && !confirmDestructiveAction(
            this,
            QStringLiteral("清理选中"),
            QStringLiteral("将删除选中清理项中的可清理文件。仅统计项目不会被删除。\n\n删除前备份: %1")
                .arg(options.backup ? QStringLiteral("开启") : QStringLiteral("关闭"))
        )) {
        return;
    }
    const CleanResult result = cleanupEngine_.cleanEntriesDetailed(selected, options);
    const QString message = cleanResultMessage(result);
    if (result.errors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("清理选中"), message);
    } else {
        QMessageBox::warning(this, QStringLiteral("清理选中"), message);
    }
    startScan();
}

void MainWindow::cleanAllForCurrentMode() {
    if (scanWatcher_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("一键清理"), QStringLiteral("扫描进行中，请等待扫描完成后再清理。"));
        return;
    }
    CleanOptions options;
    options.simulate = simulateMode_->isChecked();
    options.backup = backupMode_->isChecked();
    options.allowScanOnly = allowScanOnly();
    const QVector<CleanupEntry> entries = cleanupEngine_.entriesForMode(cleanupEntries_, currentCleanMode());
    if (entries.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("一键清理"), QStringLiteral("请先扫描出可清理项目。"));
        return;
    }
    if (!options.simulate && !confirmDestructiveAction(
            this,
            QStringLiteral("一键清理"),
            QStringLiteral("将按当前模式删除可清理文件。仅统计项目不会被删除。\n\n删除前备份: %1")
                .arg(options.backup ? QStringLiteral("开启") : QStringLiteral("关闭"))
        )) {
        return;
    }
    const CleanResult result = cleanupEngine_.cleanEntriesDetailed(entries, options);
    const QString message = cleanResultMessage(result);
    if (result.errors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("一键清理"), message);
    } else {
        QMessageBox::warning(this, QStringLiteral("一键清理"), message);
    }
    startScan();
}

void MainWindow::openBackupManager() {
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("备份管理"));
    dialog->resize(820, 520);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    BackupInfo info = CleanupEngine::backupInfo();
    auto* summary = new QLabel(QStringLiteral("备份目录: %1\n备份数量: %2 / 备份总大小: %3")
        .arg(info.backupRoot)
        .arg(info.backups.size())
        .arg(CleanupEngine::formatSize(info.totalBytes)), dialog);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summary);

    auto* table = new QTableWidget(dialog);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("大小"), QStringLiteral("原始路径"), QStringLiteral("备份路径")});
    setTableStretch(table);
    table->setRowCount(info.backups.size());
    for (int row = 0; row < info.backups.size(); ++row) {
        const BackupRecord& record = info.backups.at(row);
        auto* timeItem = textItem(record.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        timeItem->setData(Qt::UserRole, row);
        table->setItem(row, 0, timeItem);
        table->setItem(row, 1, textItem(CleanupEngine::formatSize(record.size)));
        table->setItem(row, 2, textItem(record.sourcePath));
        table->setItem(row, 3, textItem(record.backupPath));
    }
    layout->addWidget(table, 1);

    auto selectedRecord = [table, info]() -> BackupRecord {
        const QModelIndexList rows = table->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            return {};
        }
        const int row = rows.first().row();
        if (row < 0 || row >= info.backups.size()) {
            return {};
        }
        return info.backups.at(row);
    };

    auto* buttons = new QHBoxLayout();
    auto* restoreButton = primaryButton(QStringLiteral("恢复选中"));
    auto* deleteButton = secondaryButton(QStringLiteral("删除选中"));
    auto* pruneButton = secondaryButton(QStringLiteral("清理旧备份"));
    auto* closeButton = secondaryButton(QStringLiteral("关闭"));
    connect(restoreButton, &QPushButton::clicked, dialog, [this, selectedRecord] {
        const BackupRecord record = selectedRecord();
        if (record.backupPath.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("备份恢复"), QStringLiteral("请先选择要恢复的备份。"));
            return;
        }
        if (!confirmDestructiveAction(this, QStringLiteral("备份恢复"), QStringLiteral("恢复备份将覆盖当前文件:\n%1").arg(record.sourcePath))) {
            return;
        }
        QString error;
        if (CleanupEngine::restoreBackupItem(record, &error)) {
            QMessageBox::information(this, QStringLiteral("备份恢复"), QStringLiteral("恢复成功。"));
        } else {
            QMessageBox::warning(this, QStringLiteral("备份恢复"), error);
        }
    });
    connect(deleteButton, &QPushButton::clicked, dialog, [this, selectedRecord] {
        const BackupRecord record = selectedRecord();
        if (record.backupPath.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("删除备份"), QStringLiteral("请先选择要删除的备份。"));
            return;
        }
        if (!confirmDestructiveAction(this, QStringLiteral("删除备份"), QStringLiteral("确定删除选中的备份吗？此操作无法撤销。"))) {
            return;
        }
        QString error;
        if (CleanupEngine::deleteBackupItem(record, &error)) {
            QMessageBox::information(this, QStringLiteral("删除备份"), QStringLiteral("删除成功，重新打开备份管理可刷新列表。"));
        } else {
            QMessageBox::warning(this, QStringLiteral("删除备份"), error);
        }
    });
    connect(pruneButton, &QPushButton::clicked, dialog, [this] {
        CleanupEngine::pruneBackups();
        QMessageBox::information(this, QStringLiteral("清理旧备份"), QStringLiteral("已按默认限制清理旧备份。"));
    });
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);
    buttons->addWidget(restoreButton);
    buttons->addWidget(deleteButton);
    buttons->addWidget(pruneButton);
    buttons->addStretch();
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::populateStartupItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("开机加速")), SystemCatalog::populateStartupItems());
}

void MainWindow::populateMemoryItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("运行内存")), SystemCatalog::populateMemoryItems());
}

void MainWindow::populateSystemOptimizationItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("系统优化")), SystemCatalog::populateSystemOptimizationItems());
}

void MainWindow::populatePrivacyItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("隐私清理")), SystemCatalog::populatePrivacyItems());
}

void MainWindow::populateRegistryItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("注册表清理")), SystemCatalog::populateRegistryItems());
}

void MainWindow::populateNvidiaItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("NVIDIA 一键调优")), SystemCatalog::nvidiaItems());
}

void MainWindow::populateAmdItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("AMD 一键调优")), SystemCatalog::amdItems());
}

void MainWindow::populateMaintenanceItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("维护工具")), SystemCatalog::maintenanceItems());
}

void MainWindow::populateEdgeToolkitItems() {
    populateOptimizerTable(optimizerTables_.value(QStringLiteral("Edge 工具箱")), SystemCatalog::edgeToolkitItems());
}

void MainWindow::populateOptimizerTable(QTreeWidget* table, const QVector<OptimizerItem>& items) {
    if (!table) {
        return;
    }
    table->clear();
    for (const OptimizerItem& row : items) {
        auto* itemNode = new QTreeWidgetItem(table);
        itemNode->setText(0, row.title);
        itemNode->setText(1, row.location + QStringLiteral(" - ") + row.description);
        itemNode->setText(2, row.actionLabel);
        itemNode->setCheckState(0, row.recommended ? Qt::Checked : Qt::Unchecked);
        itemNode->setData(0, Qt::UserRole, row.command);
        itemNode->setData(1, Qt::UserRole, row.title);
        itemNode->setData(2, Qt::UserRole, row.id);
        for (const QString& child : row.children) {
            auto* childNode = new QTreeWidgetItem(itemNode);
            childNode->setText(0, child);
            childNode->setText(1, row.tab);
            childNode->setText(2, row.checkOnly ? QStringLiteral("检查") : QStringLiteral("明细"));
        }
    }
    table->expandAll();
}

void MainWindow::runOptimizerAction(const OptimizerItem& item) {
    int exitCode = 0;
    const QString output = SystemCatalog::runCommand(item.command, &exitCode);
    QMessageBox::information(this, item.title, QStringLiteral("退出码 %1\n%2").arg(exitCode).arg(output));
}

void MainWindow::applyCurrentOptimizationTab() {
    const QString tab = optimizerTabs_->tabText(optimizerTabs_->currentIndex());
    QTreeWidget* table = optimizerTables_.value(tab);
    if (!table) {
        if (optimizerTabs_->currentWidget() == windowsOptimizationTable_) {
            QMessageBox::information(this, QStringLiteral("Windows 设置优化"), QStringLiteral("请使用每行的执行/恢复按钮，避免一次性应用高风险设置。"));
        }
        return;
    }
    QString log;
    for (int i = 0; i < table->topLevelItemCount(); ++i) {
        QTreeWidgetItem* node = table->topLevelItem(i);
        if (node->checkState(0) != Qt::Checked) {
            continue;
        }
        int exitCode = 0;
        const QString command = node->data(0, Qt::UserRole).toString();
        const QString title = node->data(1, Qt::UserRole).toString();
        const QString id = node->data(2, Qt::UserRole).toString();
        if (id == QStringLiteral("ad_block")) {
            runAdBlockAction(true);
            log += QStringLiteral("[%1]\n广告清理已执行\n\n").arg(title);
            continue;
        }
        log += QStringLiteral("[%1]\n%2\n\n").arg(title, SystemCatalog::runCommand(command, &exitCode));
    }
    QMessageBox::information(this, QStringLiteral("一键优化"), log.isEmpty() ? QStringLiteral("没有勾选优化项。") : log);
    showOperationLog(QStringLiteral("执行优化页签: %1").arg(tab));
}

void MainWindow::populateWindowsOptimizationActions() {
    if (!windowsOptimizationTable_) {
        return;
    }
    const QVector<WindowsOptimizationAction> actions = SystemCatalog::windowsOptimizationActions();
    windowsOptimizationTable_->setRowCount(0);
    for (const WindowsOptimizationAction& action : actions) {
        const int row = windowsOptimizationTable_->rowCount();
        windowsOptimizationTable_->insertRow(row);
        windowsOptimizationTable_->setItem(row, 0, textItem(action.category));
        windowsOptimizationTable_->setItem(row, 1, textItem(action.title));
        windowsOptimizationTable_->setItem(row, 2, textItem(action.riskLabel));
        windowsOptimizationTable_->setItem(row, 3, textItem(action.description));
        auto* apply = primaryButton(QStringLiteral("执行"));
        connect(apply, &QPushButton::clicked, this, [this, row] { runWindowsOptimizationAction(row, false); });
        windowsOptimizationTable_->setCellWidget(row, 4, apply);
        auto* revert = secondaryButton(QStringLiteral("恢复"));
        revert->setEnabled(!action.revertCommands.isEmpty());
        connect(revert, &QPushButton::clicked, this, [this, row] { runWindowsOptimizationAction(row, true); });
        windowsOptimizationTable_->setCellWidget(row, 5, revert);
        windowsOptimizationTable_->setItem(row, 6, textItem(action.requiresAdmin ? QStringLiteral("需要") : QStringLiteral("否")));
    }
}

void MainWindow::runWindowsOptimizationAction(int row, bool revert) {
    const QVector<WindowsOptimizationAction> actions = SystemCatalog::windowsOptimizationActions();
    if (row < 0 || row >= actions.size()) {
        return;
    }
    const WindowsOptimizationAction action = actions.at(row);
    const QVector<WindowsOptimizationCommand> commands = revert ? action.revertCommands : action.commands;
    if (commands.isEmpty()) {
        QMessageBox::information(this, action.title, QStringLiteral("该项目没有可执行命令。"));
        return;
    }
    if (!confirmDestructiveAction(this, revert ? QStringLiteral("确认恢复默认") : QStringLiteral("确认执行优化"),
            QStringLiteral("%1\n\n%2\n\n风险: %3").arg(action.title, action.description, action.riskLabel))) {
        return;
    }
    int exitCode = 0;
    const QString output = SystemCatalog::runActionCommands(commands, &exitCode);
    showOperationLog(QStringLiteral("%1 %2，退出码 %3").arg(revert ? QStringLiteral("恢复") : QStringLiteral("执行"), action.title).arg(exitCode));
    QMessageBox::information(this, action.title, QStringLiteral("退出码 %1\n%2").arg(exitCode).arg(output));
}

void MainWindow::runAdBlockAction(bool enable) {
    int exitCode = 0;
    const QString command = adBlockHostsCommand(enable);
    const QString output = SystemCatalog::runCommand(command, &exitCode);
    showOperationLog(enable ? QStringLiteral("广告清理已执行") : QStringLiteral("广告清理已还原"));
    QMessageBox::information(this, QStringLiteral("广告清理"), QStringLiteral("退出码 %1\n%2").arg(exitCode).arg(output));
}

void MainWindow::runGlobalRestore() {
    if (!confirmDestructiveAction(this, QStringLiteral("全局一键还原所有修改"), QStringLiteral("将尝试恢复广告清理、Windows 设置优化和文件迁移记录。是否继续？"))) {
        return;
    }
    int exitCode = 0;
    QString output;
    output += QStringLiteral("[广告清理]\n") + SystemCatalog::runCommand(adBlockHostsCommand(false), &exitCode) + QStringLiteral("\n\n");
    for (const WindowsOptimizationAction& action : SystemCatalog::windowsOptimizationActions()) {
        if (action.revertCommands.isEmpty()) {
            continue;
        }
        output += QStringLiteral("[%1]\n%2\n\n").arg(action.title, SystemCatalog::runActionCommands(action.revertCommands, &exitCode));
    }
    for (const MigrationFolder& folder : fileEngine_.scanMigrationFolders()) {
        if (!folder.migrated) {
            continue;
        }
        const FileOperationResult result = fileEngine_.restorePersonalFolder(folder.key);
        output += QStringLiteral("[文件迁移] %1: %2\n")
            .arg(folder.name, result.errors.isEmpty() ? QStringLiteral("已还原") : result.errors.join(QStringLiteral("; ")));
    }
    refreshMigrationFolders();
    showOperationLog(QStringLiteral("全局一键还原所有修改"));
    QMessageBox::information(this, QStringLiteral("全局一键还原所有修改"), output);
}

void MainWindow::populateBxItems() {
    applyBxMode(bxMode_);
}

void MainWindow::applyBxMode(const QString& mode) {
    bxMode_ = mode == QStringLiteral("best") ? QStringLiteral("best") : QStringLiteral("basic");
    const QVector<BxItem> items = SystemCatalog::bxItems();
    bxTable_->setRowCount(0);
    for (const BxItem& item : items) {
        if ((bxMode_ == QStringLiteral("basic") && !item.basic) || (bxMode_ == QStringLiteral("best") && !item.best)) {
            continue;
        }
        const int row = bxTable_->rowCount();
        bxTable_->insertRow(row);
        auto* check = new QTableWidgetItem();
        check->setCheckState(Qt::Checked);
        check->setData(Qt::UserRole, item.command);
        bxTable_->setItem(row, 0, check);
        bxTable_->setItem(row, 1, textItem(item.category));
        bxTable_->setItem(row, 2, textItem(item.title));
        bxTable_->setItem(row, 3, textItem(item.risk));
        bxTable_->setItem(row, 4, textItem(item.targetState));
    }
    bxStatusLabel_->setText(QStringLiteral("BX(优化) %1 模式已加载。").arg(bxMode_ == QStringLiteral("best") ? QStringLiteral("最佳") : QStringLiteral("基本")));
}

void MainWindow::applyBxOptimization() {
    QString log;
    for (int row = 0; row < bxTable_->rowCount(); ++row) {
        QTableWidgetItem* check = bxTable_->item(row, 0);
        if (!check || check->checkState() != Qt::Checked) {
            continue;
        }
        int exitCode = 0;
        const QString command = check->data(Qt::UserRole).toString();
        log += SystemCatalog::runCommand(command, &exitCode) + QStringLiteral("\n");
    }
    bxStatusLabel_->setText(QStringLiteral("BX(优化) 执行完成。"));
    QMessageBox::information(this, QStringLiteral("BX(优化)"), log.isEmpty() ? QStringLiteral("没有启用项目。") : log);
}

void MainWindow::refreshInstalledApps() {
    uninstallTable_->setRowCount(0);
    QVector<QJsonObject> apps;
#ifdef Q_OS_WIN
    const QStringList roots = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
    };
    for (const QString& root : roots) {
        QSettings settings(root, QSettings::NativeFormat);
        for (const QString& group : settings.childGroups()) {
            settings.beginGroup(group);
            const QString name = settings.value(QStringLiteral("DisplayName")).toString();
            if (!name.isEmpty()) {
                QJsonObject app;
                app.insert(QStringLiteral("name"), name);
                app.insert(QStringLiteral("publisher"), settings.value(QStringLiteral("Publisher")).toString());
                app.insert(QStringLiteral("version"), settings.value(QStringLiteral("DisplayVersion")).toString());
                app.insert(QStringLiteral("command"), settings.value(QStringLiteral("UninstallString")).toString());
                apps.push_back(app);
            }
            settings.endGroup();
        }
    }
#endif
    if (apps.isEmpty()) {
        QJsonObject app;
        app.insert(QStringLiteral("name"), QStringLiteral("当前环境未读取到软件列表"));
        app.insert(QStringLiteral("publisher"), QStringLiteral("Windows 上会读取卸载注册表"));
        app.insert(QStringLiteral("version"), QStringLiteral("--"));
        app.insert(QStringLiteral("command"), QString());
        apps.push_back(app);
    }
    for (const QJsonObject& app : apps) {
        const int row = uninstallTable_->rowCount();
        uninstallTable_->insertRow(row);
        uninstallTable_->setItem(row, 0, textItem(app.value(QStringLiteral("name")).toString()));
        uninstallTable_->setItem(row, 1, textItem(app.value(QStringLiteral("publisher")).toString()));
        uninstallTable_->setItem(row, 2, textItem(app.value(QStringLiteral("version")).toString()));
        uninstallTable_->setItem(row, 3, textItem(app.value(QStringLiteral("command")).toString()));
        auto* button = secondaryButton(QStringLiteral("卸载"));
        const QString command = app.value(QStringLiteral("command")).toString();
        connect(button, &QPushButton::clicked, this, [this, command] { runUninstallCommand(command); });
        uninstallTable_->setCellWidget(row, 4, button);
    }
}

void MainWindow::runUninstallCommand(const QString& command) {
    if (command.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("软件卸载"), QStringLiteral("没有可执行的卸载命令。"));
        return;
    }
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("cmd.exe"), {QStringLiteral("/C"), command});
#else
    Q_UNUSED(command);
#endif
    QTimer::singleShot(4000, this, &MainWindow::refreshInstalledApps);
}

void MainWindow::chooseFileRoot() {
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择文件管理扫描目录"),
        fileRoot_.isEmpty() ? cDriveRoot() : fileRoot_
    );
    if (selected.isEmpty()) {
        return;
    }
    fileRoot_ = selected;
    if (fileRootLabel_) {
        fileRootLabel_->setText(QStringLiteral("当前目录: %1").arg(QDir::toNativeSeparators(fileRoot_)));
    }
    if (fileStatusLabel_) {
        fileStatusLabel_->setText(QStringLiteral("已切换扫描目录: %1").arg(QDir::toNativeSeparators(fileRoot_)));
    }
}

void MainWindow::scanFolderUsage() {
    const QString root = fileRoot_.isEmpty() ? cDriveRoot() : fileRoot_;
    fileTabs_->setCurrentWidget(folderUsageTree_);
    fileStatusLabel_->setText(QStringLiteral("正在扫描文件夹占用: %1").arg(QDir::toNativeSeparators(root)));
    auto* watcher = new QFutureWatcher<QVector<FolderUsageEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<FolderUsageEntry>>::finished, this, [this, watcher] {
        populateFolderUsage(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([root] {
        return FileManagementEngine().scanFolderUsage(root);
    }));
}

void MainWindow::scanLargeFilesAsync() {
    const QString root = fileRoot_.isEmpty() ? cDriveRoot() : fileRoot_;
    fileTabs_->setCurrentWidget(largeFileTable_);
    fileStatusLabel_->setText(QStringLiteral("正在扫描大文件..."));
    auto* watcher = new QFutureWatcher<QVector<FileEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<FileEntry>>::finished, this, [this, watcher] {
        populateLargeFiles(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([root] {
        return CleanupEngine::scanLargeFilesAsync(root);
    }));
}

void MainWindow::scanDuplicateFilesAsync() {
    const QString root = fileRoot_.isEmpty() ? cDriveRoot() : fileRoot_;
    fileTabs_->setCurrentWidget(duplicateFileTable_);
    fileStatusLabel_->setText(QStringLiteral("正在扫描重复文件..."));
    auto* watcher = new QFutureWatcher<QVector<QVector<FileEntry>>>(this);
    connect(watcher, &QFutureWatcher<QVector<QVector<FileEntry>>>::finished, this, [this, watcher] {
        populateDuplicateFiles(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([root] {
        return CleanupEngine::scanDuplicateFilesAsync(root);
    }));
}

void MainWindow::scanEmptyFolders() {
    const QString root = fileRoot_.isEmpty() ? cDriveRoot() : fileRoot_;
    fileTabs_->setCurrentWidget(emptyFolderTable_);
    fileStatusLabel_->setText(QStringLiteral("正在扫描空文件夹: %1").arg(QDir::toNativeSeparators(root)));
    auto* watcher = new QFutureWatcher<QVector<EmptyFolderEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<EmptyFolderEntry>>::finished, this, [this, watcher] {
        populateEmptyFolders(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([root] {
        return FileManagementEngine().scanEmptyFolders(root);
    }));
}

void MainWindow::refreshMigrationFolders() {
    if (!migrationTable_) {
        return;
    }
    populateMigrationFolders(fileEngine_.scanMigrationFolders());
}

void MainWindow::migrateSelectedFolders() {
    const QString targetRoot = migrationTargetEdit_ ? migrationTargetEdit_->text().trimmed() : QString();
    if (targetRoot.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("系统目录一键迁移专区"), QStringLiteral("请先填写目标根目录。"));
        return;
    }
    if (!confirmDestructiveAction(
            this,
            QStringLiteral("系统目录一键迁移专区"),
            QStringLiteral("将把勾选的系统目录迁移到:\n%1\n\n请确认目标磁盘空间充足。").arg(targetRoot)
        )) {
        return;
    }

    QStringList errors;
    int success = 0;
    for (int row = 0; row < migrationTable_->rowCount(); ++row) {
        QTableWidgetItem* check = migrationTable_->item(row, 0);
        if (!check || check->checkState() != Qt::Checked) {
            continue;
        }
        const QString key = check->data(Qt::UserRole).toString();
        const FileOperationResult result = fileEngine_.migratePersonalFolder(
            key,
            targetRoot,
            migrationMoveFiles_ ? migrationMoveFiles_->isChecked() : true
        );
        if (result.errors.isEmpty()) {
            ++success;
        } else {
            errors.append(result.errors);
        }
    }
    refreshMigrationFolders();
    const QString summary = QStringLiteral("迁移完成: %1 项，失败: %2 项。").arg(success).arg(errors.size());
    fileStatusLabel_->setText(summary);
    if (errors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("文件迁移"), summary);
    } else {
        QMessageBox::warning(this, QStringLiteral("文件迁移"), summary + QStringLiteral("\n\n") + errors.mid(0, 12).join(QStringLiteral("\n")));
    }
}

void MainWindow::restoreSelectedFolders() {
    if (!confirmDestructiveAction(this, QStringLiteral("还原选中"), QStringLiteral("将还原勾选的已迁移系统目录。是否继续？"))) {
        return;
    }

    QStringList errors;
    int success = 0;
    for (int row = 0; row < migrationTable_->rowCount(); ++row) {
        QTableWidgetItem* check = migrationTable_->item(row, 0);
        if (!check || check->checkState() != Qt::Checked) {
            continue;
        }
        const QString key = check->data(Qt::UserRole).toString();
        const FileOperationResult result = fileEngine_.restorePersonalFolder(key);
        if (result.errors.isEmpty()) {
            ++success;
        } else {
            errors.append(result.errors);
        }
    }
    refreshMigrationFolders();
    const QString summary = QStringLiteral("还原完成: %1 项，失败: %2 项。").arg(success).arg(errors.size());
    fileStatusLabel_->setText(summary);
    if (errors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("还原选中"), summary);
    } else {
        QMessageBox::warning(this, QStringLiteral("还原选中"), summary + QStringLiteral("\n\n") + errors.mid(0, 12).join(QStringLiteral("\n")));
    }
}

void MainWindow::populateLargeFiles(const QVector<FileEntry>& files) {
    largeFileTable_->setRowCount(0);
    for (const FileEntry& file : files) {
        const int row = largeFileTable_->rowCount();
        largeFileTable_->insertRow(row);
        largeFileTable_->setItem(row, 0, textItem(file.name));
        largeFileTable_->setItem(row, 1, textItem(CleanupEngine::formatSize(file.size)));
        largeFileTable_->setItem(row, 2, textItem(file.path));
    }
    fileStatusLabel_->setText(QStringLiteral("大文件扫描完成: %1 个").arg(files.size()));
}

void MainWindow::populateDuplicateFiles(const QVector<QVector<FileEntry>>& groups) {
    duplicateFileTable_->setRowCount(0);
    int groupIndex = 1;
    for (const QVector<FileEntry>& group : groups) {
        for (const FileEntry& file : group) {
            const int row = duplicateFileTable_->rowCount();
            duplicateFileTable_->insertRow(row);
            duplicateFileTable_->setItem(row, 0, textItem(QString::number(groupIndex)));
            duplicateFileTable_->setItem(row, 1, textItem(file.name));
            duplicateFileTable_->setItem(row, 2, textItem(CleanupEngine::formatSize(file.size)));
            duplicateFileTable_->setItem(row, 3, textItem(file.path));
        }
        ++groupIndex;
    }
    fileStatusLabel_->setText(QStringLiteral("重复文件扫描完成: %1 组").arg(groups.size()));
}

void MainWindow::populateFolderUsage(const QVector<FolderUsageEntry>& entries) {
    folderUsageTree_->clear();
    qint64 total = 0;
    for (const FolderUsageEntry& entry : entries) {
        total += entry.sizeBytes;
    }
    for (const FolderUsageEntry& entry : entries) {
        auto* row = new QTreeWidgetItem(folderUsageTree_);
        row->setText(0, QDir::toNativeSeparators(entry.path));
        row->setText(1, CleanupEngine::formatSize(entry.sizeBytes));
        row->setText(2, QString::number(entry.fileCount));
        row->setText(3, total > 0 ? QStringLiteral("%1%").arg(entry.sizeBytes * 100.0 / total, 0, 'f', 1) : QStringLiteral("--"));
        row->setData(0, Qt::UserRole, entry.path);
    }
    fileStatusLabel_->setText(QStringLiteral("文件夹占用扫描完成: %1 项").arg(entries.size()));
}

void MainWindow::populateEmptyFolders(const QVector<EmptyFolderEntry>& folders) {
    emptyFolderTable_->setRowCount(0);
    for (const EmptyFolderEntry& folder : folders) {
        const int row = emptyFolderTable_->rowCount();
        emptyFolderTable_->insertRow(row);
        emptyFolderTable_->setItem(row, 0, textItem(QDir::toNativeSeparators(folder.path)));
    }
    fileStatusLabel_->setText(QStringLiteral("空文件夹扫描完成: %1 个").arg(folders.size()));
}

void MainWindow::populateMigrationFolders(const QVector<MigrationFolder>& folders) {
    migrationTable_->setRowCount(0);
    for (const MigrationFolder& folder : folders) {
        const int row = migrationTable_->rowCount();
        migrationTable_->insertRow(row);
        auto* check = new QTableWidgetItem();
        check->setCheckState(Qt::Unchecked);
        check->setData(Qt::UserRole, folder.key);
        migrationTable_->setItem(row, 0, check);
        migrationTable_->setItem(row, 1, textItem(folder.name));
        migrationTable_->setItem(row, 2, textItem(folder.migrated ? QStringLiteral("已迁移") : (folder.exists ? QStringLiteral("未迁移") : QStringLiteral("不存在"))));
        migrationTable_->setItem(row, 3, textItem(CleanupEngine::formatSize(folder.sizeBytes)));
        migrationTable_->setItem(row, 4, textItem(QDir::toNativeSeparators(folder.path)));
        migrationTable_->setItem(row, 5, textItem(QDir::toNativeSeparators(folder.target)));
    }
}

void MainWindow::deleteSelectedFileItems() {
    QTableWidget* table = nullptr;
    int pathColumn = -1;
    if (fileTabs_->currentWidget() == largeFileTable_) {
        table = largeFileTable_;
        pathColumn = 2;
    } else if (fileTabs_->currentWidget() == duplicateFileTable_) {
        table = duplicateFileTable_;
        pathColumn = 3;
    } else if (fileTabs_->currentWidget() == emptyFolderTable_) {
        table = emptyFolderTable_;
        pathColumn = 0;
    }

    if (!table || pathColumn < 0) {
        QMessageBox::information(this, QStringLiteral("删除选中文件"), QStringLiteral("请在大文件、重复文件或空文件夹页签中选择项目。"));
        return;
    }

    const QModelIndexList rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("删除选中文件"), QStringLiteral("请先选择要删除的文件。"));
        return;
    }

    if (!confirmDestructiveAction(
            this,
            QStringLiteral("删除选中文件"),
            QStringLiteral("将永久删除选中的 %1 个文件/文件夹。此操作不会自动备份。").arg(rows.size())
        )) {
        return;
    }

    QSet<int> rowNumbers;
    for (const QModelIndex& index : rows) {
        rowNumbers.insert(index.row());
    }

    QStringList errors;
    QVector<int> deletedRows;
    for (int row : rowNumbers) {
        QTableWidgetItem* pathItem = table->item(row, pathColumn);
        if (!pathItem) {
            errors.push_back(QStringLiteral("缺少文件路径: 第 %1 行").arg(row + 1));
            continue;
        }
        const QString path = pathItem->text();
        QString error;
        if (CleanupEngine::deletePath(path, &error)) {
            deletedRows.push_back(row);
        } else {
            errors.push_back(error);
        }
    }

    std::sort(deletedRows.begin(), deletedRows.end(), std::greater<int>());
    for (int row : deletedRows) {
        table->removeRow(row);
    }

    const QString summary = QStringLiteral("删除完成: %1 个，失败: %2 个。")
        .arg(deletedRows.size())
        .arg(errors.size());
    fileStatusLabel_->setText(summary);
    if (!errors.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("删除选中文件"), summary + QStringLiteral("\n\n") + errors.mid(0, 12).join(QStringLiteral("\n")));
    } else {
        QMessageBox::information(this, QStringLiteral("删除选中文件"), summary);
    }
}

void MainWindow::shredSelectedFileItems() {
    QTableWidget* table = nullptr;
    int pathColumn = -1;
    if (fileTabs_->currentWidget() == largeFileTable_) {
        table = largeFileTable_;
        pathColumn = 2;
    } else if (fileTabs_->currentWidget() == duplicateFileTable_) {
        table = duplicateFileTable_;
        pathColumn = 3;
    }

    if (!table || pathColumn < 0) {
        QMessageBox::information(this, QStringLiteral("彻底删除文件"), QStringLiteral("请在大文件或重复文件页签中选择文件。"));
        return;
    }

    const QModelIndexList rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("彻底删除文件"), QStringLiteral("请先选择要彻底删除的文件。"));
        return;
    }
    if (!confirmDestructiveAction(
            this,
            QStringLiteral("彻底删除文件"),
            QStringLiteral("将覆写并删除选中的 %1 个文件。此操作无法恢复。").arg(rows.size())
        )) {
        return;
    }

    QSet<int> rowNumbers;
    QStringList paths;
    for (const QModelIndex& index : rows) {
        rowNumbers.insert(index.row());
    }
    for (int row : rowNumbers) {
        QTableWidgetItem* pathItem = table->item(row, pathColumn);
        if (pathItem) {
            paths.push_back(pathItem->text());
        }
    }

    const FileOperationResult result = fileEngine_.shredFiles(paths);
    QVector<int> deletedRows;
    for (int row : rowNumbers) {
        QTableWidgetItem* pathItem = table->item(row, pathColumn);
        if (pathItem && result.affectedPaths.contains(pathItem->text())) {
            deletedRows.push_back(row);
        }
    }
    std::sort(deletedRows.begin(), deletedRows.end(), std::greater<int>());
    for (int row : deletedRows) {
        table->removeRow(row);
    }

    const QString summary = QStringLiteral("彻底删除完成: %1 个，失败: %2 个。")
        .arg(result.affectedPaths.size())
        .arg(result.errors.size());
    fileStatusLabel_->setText(summary);
    if (result.errors.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("彻底删除文件"), summary);
    } else {
        QMessageBox::warning(this, QStringLiteral("彻底删除文件"), summary + QStringLiteral("\n\n") + result.errors.mid(0, 12).join(QStringLiteral("\n")));
    }
}

void MainWindow::scanFragments() {
    int exitCode = 0;
    fileStatusLabel_->setText(SystemCatalog::runCommand(QStringLiteral("defrag C: /A /V"), &exitCode));
}

void MainWindow::optimizeFragments() {
    int exitCode = 0;
    fileStatusLabel_->setText(SystemCatalog::runCommand(QStringLiteral("defrag C: /U /V"), &exitCode));
}

void MainWindow::populateRepairTable() {
    const QVector<RepairItem> items = SystemCatalog::repairActions();
    repairTable_->setRowCount(0);
    for (const RepairItem& item : items) {
        const int row = repairTable_->rowCount();
        repairTable_->insertRow(row);
        auto* check = new QTableWidgetItem();
        check->setCheckState(item.recommended ? Qt::Checked : Qt::Unchecked);
        check->setData(Qt::UserRole, item.command);
        repairTable_->setItem(row, 0, check);
        repairTable_->setItem(row, 1, textItem(item.title));
        repairTable_->setItem(row, 2, textItem(item.risk));
        repairTable_->setItem(row, 3, textItem(item.description));
        repairTable_->setItem(row, 4, textItem(item.command));
    }
}

void MainWindow::runRepairCommand(const RepairItem& item) {
    int exitCode = 0;
    repairLog_->append(QStringLiteral("[%1]").arg(item.title));
    repairLog_->append(SystemCatalog::runCommand(item.command, &exitCode));
}

void MainWindow::runSelectedRepairs() {
    for (int row = 0; row < repairTable_->rowCount(); ++row) {
        QTableWidgetItem* check = repairTable_->item(row, 0);
        if (!check || check->checkState() != Qt::Checked) {
            continue;
        }
        int exitCode = 0;
        const QString title = repairTable_->item(row, 1)->text();
        const QString command = check->data(Qt::UserRole).toString();
        repairLog_->append(QStringLiteral("[%1]").arg(title));
        repairLog_->append(SystemCatalog::runCommand(command, &exitCode));
    }
}

void MainWindow::refreshAccountState() {
    const AccountState state = accountStore_.currentState();
    accountStateLabel_->setText(state.loggedIn
        ? QStringLiteral("已登录: %1 / %2 / 到期 %3").arg(state.displayName, state.plan, state.expiresAt)
        : QStringLiteral("未登录"));
}

void MainWindow::registerAccount() {
    const AccountState state = accountStore_.registerUser(accountEmailEdit_->text(), accountPasswordEdit_->text(), accountNameEdit_->text());
    accountStateLabel_->setText(state.message);
    refreshAccountState();
}

void MainWindow::loginAccount() {
    const AccountState state = accountStore_.login(accountEmailEdit_->text(), accountPasswordEdit_->text());
    accountStateLabel_->setText(state.message);
    refreshAccountState();
}

void MainWindow::redeemCard() {
    const AccountState state = accountStore_.redeemCard(cardCodeEdit_->text());
    accountStateLabel_->setText(state.message);
    refreshAccountState();
}

void MainWindow::logoutAccount() {
    accountStore_.logout();
    refreshAccountState();
}
