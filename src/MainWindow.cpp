#include "MainWindow.h"

#include <QtConcurrent/QtConcurrent>

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int PathRole = Qt::UserRole + 10;
constexpr int CommandRole = Qt::UserRole + 11;
constexpr int AppIndexRole = Qt::UserRole + 12;
const QString OptimizationStateKey = QStringLiteral("state/applied_optimizations");
const QString GpuStateKey = QStringLiteral("state/applied_gpu_actions");

struct CommandBatchResult {
    QString output;
    QMap<QString, int> exitCodes;
};

QSet<QString> appliedActionIds(const QString& key) {
    QSet<QString> result;
    for (const QString& value : QSettings().value(key).toStringList()) {
        result.insert(value);
    }
    return result;
}

void setActionApplied(const QString& key, const QString& id, bool applied) {
    QSet<QString> values = appliedActionIds(key);
    if (applied) {
        values.insert(id);
    } else {
        values.remove(id);
    }
    QStringList sorted = values.values();
    sorted.sort(Qt::CaseInsensitive);
    QSettings().setValue(key, sorted);
}

QTableWidgetItem* textItem(const QString& value) {
    auto* item = new QTableWidgetItem(value);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

void configureTable(QTableWidget* table) {
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setAlternatingRowColors(true);
}

void reserveActionColumn(QTableWidget* table, int column) {
    table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    table->setColumnWidth(column, 82);
}

QString defaultDiskRoot() {
#ifdef Q_OS_WIN
    return QStringLiteral("C:/");
#else
    return QDir::homePath();
#endif
}

QString defaultMigrationTargetRoot() {
#ifdef Q_OS_WIN
    const QString systemDrive = qEnvironmentVariable("SystemDrive", "C:").toUpper();
    for (const QFileInfo& drive : QDir::drives()) {
        const QString root = QDir::toNativeSeparators(drive.absoluteFilePath());
        if (root.left(2).toUpper() != systemDrive) {
            return QDir(root).filePath(QStringLiteral("C_DiskGlow_Moved"));
        }
    }
    return {};
#else
    return QDir::home().filePath(QStringLiteral("C_DiskGlow_Moved"));
#endif
}

bool confirmAction(QWidget* parent, const QString& title, const QString& message) {
    return QMessageBox::question(
        parent,
        title,
        message,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    ) == QMessageBox::Yes;
}

QString cleanResultMessage(const CleanResult& result) {
    QString message = QStringLiteral("处理完成: %1\n已处理文件: %2\n已跳过文件: %3")
        .arg(CleanupEngine::formatSize(result.cleanedBytes))
        .arg(result.deletedCount)
        .arg(result.skippedCount);
    if (!result.errors.isEmpty()) {
        message += QStringLiteral("\n\n失败明细:\n") + result.errors.mid(0, 12).join(QStringLiteral("\n"));
    }
    return message;
}

QString commandsText(const QVector<WindowsOptimizationCommand>& commands) {
    QStringList lines;
    for (const WindowsOptimizationCommand& command : commands) {
        QStringList arguments;
        for (const QString& argument : command.arguments) {
            arguments.push_back(argument.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(argument) : argument);
        }
        lines.push_back(QStringLiteral("%1 %2").arg(command.executable, arguments.join(QLatin1Char(' '))).trimmed());
    }
    return lines.join(QStringLiteral("\n"));
}

QString operationLogPath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString directory = QDir(base.isEmpty() ? QDir::homePath() : base).filePath(QStringLiteral("logs"));
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("operations.log"));
}

QColor riskColor(const QString& risk) {
    if (risk.contains(QStringLiteral("高危")) || risk.contains(QStringLiteral("谨慎"))) {
        return QColor(QStringLiteral("#fff3cd"));
    }
    return QColor(QStringLiteral("#ecfdf5"));
}

QColor extensionColor(const QString& extension) {
    static const QVector<QColor> palette = {
        QColor(QStringLiteral("#3346a8")), QColor(QStringLiteral("#a23a3a")),
        QColor(QStringLiteral("#2f8f46")), QColor(QStringLiteral("#008c95")),
        QColor(QStringLiteral("#8a258f")), QColor(QStringLiteral("#a27d00")),
        QColor(QStringLiteral("#1f5f8f")), QColor(QStringLiteral("#a00070")),
        QColor(QStringLiteral("#8b4b12")), QColor(QStringLiteral("#535a63")),
    };
    uint seed = 0;
    for (const QChar ch : extension.toLower()) {
        seed = seed * 33 + ch.unicode();
    }
    return palette.at(static_cast<int>(seed % palette.size()));
}

qint64 treemapTotal(const QVector<FileUsageEntry>& entries, int first, int last) {
    qint64 total = 0;
    for (int i = first; i <= last; ++i) {
        total += qMax<qint64>(1, entries.at(i).sizeBytes);
    }
    return total;
}

void drawTreemap(QGraphicsScene* scene, const QVector<FileUsageEntry>& entries, int first, int last, const QRectF& rect) {
    if (!scene || first > last || rect.width() <= 1 || rect.height() <= 1) {
        return;
    }
    if (first == last || rect.width() < 7 || rect.height() < 7) {
        FileUsageEntry entry = entries.at(first);
        if (first != last) {
            entry.path = QStringLiteral("其他文件");
            entry.extension = QStringLiteral("(其他)");
            entry.sizeBytes = treemapTotal(entries, first, last);
            entry.fileCount = last - first + 1;
        }
        const QRectF cell = rect.adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor color = extensionColor(entry.extension);
        auto* rectangle = scene->addRect(cell, QPen(color.darker(150), 0), QBrush(color));
        rectangle->setToolTip(QStringLiteral("%1\n%2\n%3")
            .arg(QDir::toNativeSeparators(entry.path), CleanupEngine::formatSize(entry.sizeBytes), entry.extension));
        if (cell.width() > 105 && cell.height() > 42) {
            const QString name = QFileInfo(entry.path).fileName().isEmpty() ? entry.path : QFileInfo(entry.path).fileName();
            auto* label = scene->addText(QStringLiteral("%1\n%2").arg(name, CleanupEngine::formatSize(entry.sizeBytes)));
            label->setDefaultTextColor(Qt::white);
            label->setTextWidth(cell.width() - 8);
            label->setPos(cell.left() + 4, cell.top() + 3);
        }
        return;
    }

    const qint64 total = treemapTotal(entries, first, last);
    qint64 firstHalf = 0;
    int split = first;
    while (split < last) {
        firstHalf += qMax<qint64>(1, entries.at(split).sizeBytes);
        if (firstHalf >= total / 2) {
            break;
        }
        ++split;
    }
    if (split >= last) {
        split = first;
        firstHalf = qMax<qint64>(1, entries.at(first).sizeBytes);
    }
    if (rect.width() >= rect.height()) {
        const qreal width = qBound<qreal>(1.0, rect.width() * static_cast<qreal>(firstHalf) / total, rect.width() - 1.0);
        drawTreemap(scene, entries, first, split, QRectF(rect.left(), rect.top(), width, rect.height()));
        drawTreemap(scene, entries, split + 1, last, QRectF(rect.left() + width, rect.top(), rect.width() - width, rect.height()));
    } else {
        const qreal height = qBound<qreal>(1.0, rect.height() * static_cast<qreal>(firstHalf) / total, rect.height() - 1.0);
        drawTreemap(scene, entries, first, split, QRectF(rect.left(), rect.top(), rect.width(), height));
        drawTreemap(scene, entries, split + 1, last, QRectF(rect.left(), rect.top() + height, rect.width(), rect.height() - height));
    }
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("C 盘清理大师"));
    setWindowFlag(Qt::WindowMaximizeButtonHint, false);
    QSize windowSize(1080, 660);
    if (QScreen* screen = QApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        windowSize = QSize(
            qBound(980, available.width() / 2 + 100, 1180),
            qBound(600, available.height() * 2 / 3, 720)
        );
        move(available.center() - QPoint(windowSize.width() / 2, windowSize.height() / 2));
    }
    setFixedSize(windowSize);

    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("appRoot"));
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(createTopNavigation());

    pages_ = new QStackedWidget(central);
    pages_->setObjectName(QStringLiteral("contentArea"));
    pages_->addWidget(scrollablePage(createCleanPage()));
    pages_->addWidget(scrollablePage(createUninstallPage()));
    pages_->addWidget(scrollablePage(createOptimizePage()));
    pages_->addWidget(scrollablePage(createFilePage()));
    pages_->addWidget(scrollablePage(createRepairPage()));
    layout->addWidget(pages_, 1);
    layout->addWidget(createBottomBar());
    setCentralWidget(central);

    applyStyle();
    selectPage(0);
    refreshDiskInfo();
    refreshInstalledApps();
    refreshGpuInfo();
    refreshDisks();
    refreshMigrationFolders();
    populateRepairTable(false);
    showOperationLog(QStringLiteral("程序启动"));
}

QWidget* MainWindow::createTopNavigation() {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("topNavigation"));
    bar->setFixedHeight(60);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(18, 10, 18, 10);
    layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("C 盘清理大师"), bar);
    title->setObjectName(QStringLiteral("brandTitle"));
    layout->addWidget(title);
    layout->addSpacing(14);

    const QStringList labels = {
        QStringLiteral("C盘深度清理"),
        QStringLiteral("软件强力卸载"),
        QStringLiteral("系统智能优化"),
        QStringLiteral("磁盘文件管理器"),
        QStringLiteral("CMD 系统修复"),
    };
    for (int i = 0; i < labels.size(); ++i) {
        QPushButton* button = navigationButton(labels.at(i), i);
        navButtons_.push_back(button);
        layout->addWidget(button);
    }
    layout->addStretch();
    return bar;
}

QWidget* MainWindow::createBottomBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("bottomBar"));
    bar->setFixedHeight(44);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 6, 16, 6);
    layout->setSpacing(10);

    const bool elevated = qApp->property("isElevated").toBool();
    privilegeStatusLabel_ = new QLabel(
        elevated ? QStringLiteral("管理员权限：已获取") : QStringLiteral("管理员权限：受限"),
        bar
    );
    privilegeStatusLabel_->setObjectName(elevated ? QStringLiteral("safeStatus") : QStringLiteral("warningStatus"));
    globalStatusLabel_ = new QLabel(QStringLiteral("已就绪"), bar);
    layout->addWidget(privilegeStatusLabel_);
    layout->addWidget(globalStatusLabel_, 1);

    auto* logButton = secondaryButton(QStringLiteral("操作日志"));
    auto* restoreButton = primaryButton(QStringLiteral("全局一键还原"));
    connect(logButton, &QPushButton::clicked, this, &MainWindow::showOperationLogDialog);
    connect(restoreButton, &QPushButton::clicked, this, &MainWindow::runGlobalRestore);
    layout->addWidget(logButton);
    layout->addWidget(restoreButton);
    return bar;
}

QWidget* MainWindow::scrollablePage(QWidget* page) {
    auto* area = new QScrollArea(this);
    area->setObjectName(QStringLiteral("pageScrollArea"));
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setWidget(page);
    return area;
}

QWidget* MainWindow::createCleanPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    auto* header = new QFrame(page);
    header->setObjectName(QStringLiteral("heroPanel"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 14, 16, 14);
    headerLayout->addWidget(pageHeader(
        QStringLiteral("C 盘深度清理"),
        QStringLiteral("扫描系统临时文件、更新缓存、日志、DUMP、缩略图、回收站、浏览器缓存和冗余安装包。")
    ), 2);

    auto* stats = new QHBoxLayout();
    totalSpaceLabel_ = new QLabel(QStringLiteral("总量 --"), header);
    usedSpaceLabel_ = new QLabel(QStringLiteral("已用 --"), header);
    freeSpaceLabel_ = new QLabel(QStringLiteral("可用 --"), header);
    reclaimSpaceLabel_ = new QLabel(QStringLiteral("可释放 --"), header);
    for (QLabel* label : {totalSpaceLabel_, usedSpaceLabel_, freeSpaceLabel_, reclaimSpaceLabel_}) {
        label->setObjectName(QStringLiteral("statPill"));
        stats->addWidget(label);
    }
    headerLayout->addLayout(stats, 3);
    layout->addWidget(header);

    auto* controls = new QHBoxLayout();
    recommendedMode_ = new QCheckBox(QStringLiteral("推荐选项"), page);
    deepMode_ = new QCheckBox(QStringLiteral("全选 / 深度"), page);
    recommendedMode_->setChecked(true);
    connect(recommendedMode_, &QCheckBox::clicked, this, [this] { updateCleanMode(false); });
    connect(deepMode_, &QCheckBox::clicked, this, [this] { updateCleanMode(true); });
    auto* scanButton = primaryButton(QStringLiteral("开始扫描"));
    auto* backupButton = secondaryButton(QStringLiteral("备份与恢复"));
    auto* cleanButton = primaryButton(QStringLiteral("清理选中"));
    connect(scanButton, &QPushButton::clicked, this, &MainWindow::startScan);
    connect(backupButton, &QPushButton::clicked, this, &MainWindow::openBackupManager);
    connect(cleanButton, &QPushButton::clicked, this, &MainWindow::cleanSelected);
    controls->addWidget(recommendedMode_);
    controls->addWidget(deepMode_);
    controls->addSpacing(12);
    controls->addWidget(scanButton);
    controls->addWidget(backupButton);
    controls->addStretch();
    controls->addWidget(cleanButton);
    layout->addLayout(controls);

    currentScanPath_ = new QLabel(QStringLiteral("等待扫描。"), page);
    scanProgress_ = new QProgressBar(page);
    scanProgress_->setRange(0, 100);
    scanProgress_->setValue(0);
    layout->addWidget(currentScanPath_);
    layout->addWidget(scanProgress_);

    cleanupTree_ = new QTreeWidget(page);
    cleanupTree_->setColumnCount(5);
    cleanupTree_->setHeaderLabels({QStringLiteral("文件名 / 清理项"), QStringLiteral("占用大小"), QStringLiteral("风险"), QStringLiteral("状态"), QStringLiteral("完整存储路径")});
    cleanupTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    cleanupTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    cleanupTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    cleanupTree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    cleanupTree_->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    installTreeContextMenu(cleanupTree_, 4);
    layout->addWidget(cleanupTree_, 1);

    cleanStatusLabel_ = new QLabel(QStringLiteral("准备扫描 C 盘可清理路径"), page);
    cleanStatusLabel_->setObjectName(QStringLiteral("statusStrip"));
    layout->addWidget(cleanStatusLabel_);

    scanWatcher_ = new QFutureWatcher<CleanupScanResult>(this);
    connect(scanWatcher_, &QFutureWatcher<CleanupScanResult>::finished, this, &MainWindow::finishScan);
    cleanWatcher_ = new QFutureWatcher<CleanResult>(this);
    connect(cleanWatcher_, &QFutureWatcher<CleanResult>::finished, this, [this] {
        const CleanResult result = cleanWatcher_->result();
        const QString message = cleanResultMessage(result);
        showOperationLog(QStringLiteral("清理完成: %1，失败 %2").arg(CleanupEngine::formatSize(result.cleanedBytes)).arg(result.errors.size()));
        if (result.errors.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("清理完成"), message);
        } else {
            QMessageBox::warning(this, QStringLiteral("清理完成"), message);
        }
        refreshDiskInfo();
        startScan();
    });
    return page;
}

QWidget* MainWindow::createUninstallPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    layout->addWidget(pageHeader(
        QStringLiteral("软件强力卸载"),
        QStringLiteral("读取本地程序和微软商店应用；支持搜索、空间排序、批量卸载、注册表备份和残留清理。")
    ));

    auto* actions = new QHBoxLayout();
    uninstallSearchEdit_ = new QLineEdit(page);
    uninstallSearchEdit_->setPlaceholderText(QStringLiteral("搜索软件名称、发布者或安装路径"));
    auto* refresh = secondaryButton(QStringLiteral("刷新列表"));
    auto* batch = primaryButton(QStringLiteral("批量卸载选中"));
    connect(uninstallSearchEdit_, &QLineEdit::textChanged, this, &MainWindow::filterInstalledApps);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshInstalledApps);
    connect(batch, &QPushButton::clicked, this, &MainWindow::uninstallSelectedApplications);
    actions->addWidget(uninstallSearchEdit_, 1);
    actions->addWidget(refresh);
    actions->addWidget(batch);
    layout->addLayout(actions);

    uninstallTable_ = new QTableWidget(page);
    uninstallTable_->setColumnCount(10);
    uninstallTable_->setHorizontalHeaderLabels({
        QStringLiteral("选择"), QStringLiteral("软件"), QStringLiteral("类型"), QStringLiteral("占用空间"),
        QStringLiteral("安装路径"), QStringLiteral("安装时间"), QStringLiteral("版本"), QStringLiteral("发布者"),
        QStringLiteral("常规卸载"), QStringLiteral("强力粉碎"),
    });
    configureTable(uninstallTable_);
    reserveActionColumn(uninstallTable_, 8);
    reserveActionColumn(uninstallTable_, 9);
    uninstallTable_->setSortingEnabled(true);
    installTableContextMenu(uninstallTable_, 4);
    layout->addWidget(uninstallTable_, 1);

    uninstallWatcher_ = new QFutureWatcher<QVector<InstalledApplication>>(this);
    connect(uninstallWatcher_, &QFutureWatcher<QVector<InstalledApplication>>::finished, this, [this] {
        installedApps_ = uninstallWatcher_->result();
        populateUninstallTable();
        filterInstalledApps(uninstallSearchEdit_ ? uninstallSearchEdit_->text() : QString());
        showOperationLog(QStringLiteral("读取软件列表，共 %1 项").arg(installedApps_.size()));
    });
    return page;
}

QTableWidget* MainWindow::createOptimizationTable(QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setColumnCount(7);
    table->setHorizontalHeaderLabels({
        QStringLiteral("选择"), QStringLiteral("分类"), QStringLiteral("项目"), QStringLiteral("风险"),
        QStringLiteral("说明"), QStringLiteral("执行"), QStringLiteral("还原"),
    });
    configureTable(table);
    reserveActionColumn(table, 5);
    reserveActionColumn(table, 6);
    installTableContextMenu(table, -1);
    return table;
}

QWidget* MainWindow::createOptimizePage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    layout->addWidget(pageHeader(
        QStringLiteral("系统智能优化"),
        QStringLiteral("办公稳定、电竞提帧、显卡专属和高级管控。所有可逆设置均提供日志与还原。")
    ));

    optimizationTabs_ = new QTabWidget(page);

    auto createPresetPage = [this, page](
        const QString& description,
        QTableWidget** table,
        const QVector<WindowsOptimizationAction>& actions,
        const QString& buttonText,
        bool deep
    ) {
        auto* presetPage = new QWidget(page);
        auto* presetLayout = new QVBoxLayout(presetPage);
        presetLayout->setContentsMargins(10, 10, 10, 10);
        auto* label = new QLabel(description, presetPage);
        label->setWordWrap(true);
        presetLayout->addWidget(label);
        *table = createOptimizationTable(presetPage);
        populateOptimizationTable(*table, actions);
        presetLayout->addWidget(*table, 1);
        auto* apply = primaryButton(buttonText);
        connect(apply, &QPushButton::clicked, this, [this, table, actions, buttonText, deep] {
            applyOptimizationPreset(*table, actions, buttonText, deep);
        });
        presetLayout->addWidget(apply, 0, Qt::AlignRight);
        return presetPage;
    };

    optimizationTabs_->addTab(createPresetPage(
        QStringLiteral("默认办公稳定模式：无激进修改，适合日常办公。"),
        &officeOptimizationTable_, SystemCatalog::officeOptimizationActions(), QStringLiteral("应用办公稳定优化"), false
    ), QStringLiteral("办公稳定"));
    optimizationTabs_->addTab(createPresetPage(
        QStringLiteral("电竞提帧模式会限制通知、搜索、休眠和系统还原等办公能力。"),
        &gamingOptimizationTable_, SystemCatalog::gamingOptimizationActions(), QStringLiteral("应用电竞提帧优化"), true
    ), QStringLiteral("电竞提帧"));

    gpuTabIndex_ = optimizationTabs_->addTab(createGpuPanel(), QStringLiteral("显卡专属"));

    auto* advancedPage = new QWidget(page);
    auto* advancedLayout = new QVBoxLayout(advancedPage);
    advancedLayout->setContentsMargins(10, 10, 10, 10);
    auto* advancedNote = new QLabel(QStringLiteral("高风险开关必须逐项确认；受 Windows 防篡改或系统版本限制时会显示命令错误。"), advancedPage);
    advancedNote->setWordWrap(true);
    advancedLayout->addWidget(advancedNote);
    advancedControlTable_ = createOptimizationTable(advancedPage);
    populateOptimizationTable(advancedControlTable_, SystemCatalog::advancedControlActions());
    advancedLayout->addWidget(advancedControlTable_, 1);
    optimizationTabs_->addTab(advancedPage, QStringLiteral("高级管控"));

    layout->addWidget(optimizationTabs_, 1);
    return page;
}

QWidget* MainWindow::createGpuPanel() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* row = new QHBoxLayout();
    auto* note = new QLabel(QStringLiteral("自动识别 NVIDIA / AMD；核显机器会隐藏本页。"), page);
    auto* refresh = secondaryButton(QStringLiteral("刷新检测"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshGpuInfo);
    row->addWidget(note, 1);
    row->addWidget(refresh);
    layout->addLayout(row);

    gpuInfoTable_ = new QTableWidget(page);
    gpuInfoTable_->setColumnCount(7);
    gpuInfoTable_->setHorizontalHeaderLabels({QStringLiteral("厂商"), QStringLiteral("显卡"), QStringLiteral("驱动"), QStringLiteral("显存"), QStringLiteral("温度"), QStringLiteral("负载"), QStringLiteral("官方支持入口")});
    configureTable(gpuInfoTable_);
    installTableContextMenu(gpuInfoTable_, -1);
    layout->addWidget(gpuInfoTable_);

    gpuActionTable_ = new QTableWidget(page);
    gpuActionTable_->setColumnCount(6);
    gpuActionTable_->setHorizontalHeaderLabels({QStringLiteral("厂商"), QStringLiteral("支持操作"), QStringLiteral("风险"), QStringLiteral("说明"), QStringLiteral("执行"), QStringLiteral("还原")});
    configureTable(gpuActionTable_);
    reserveActionColumn(gpuActionTable_, 4);
    reserveActionColumn(gpuActionTable_, 5);
    installTableContextMenu(gpuActionTable_, -1);
    layout->addWidget(gpuActionTable_);

    gpuLog_ = new QTextEdit(page);
    gpuLog_->setReadOnly(true);
    gpuLog_->setMaximumHeight(90);
    gpuLog_->setPlaceholderText(QStringLiteral("显卡操作日志"));
    layout->addWidget(gpuLog_);

    gpuWatcher_ = new QFutureWatcher<QVector<GpuDeviceInfo>>(this);
    connect(gpuWatcher_, &QFutureWatcher<QVector<GpuDeviceInfo>>::finished, this, &MainWindow::finishGpuRefresh);
    return page;
}

QWidget* MainWindow::createFilePage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);
    fileRoot_ = defaultDiskRoot();

    layout->addWidget(pageHeader(
        QStringLiteral("磁盘文件管理器"),
        QStringLiteral("全磁盘空间图、文件类型筛选、批量操作、安全工具、普通文件迁移和系统目录迁移。")
    ));

    auto* diskRow = new QHBoxLayout();
    diskCombo_ = new QComboBox(page);
    diskCombo_->setMinimumWidth(360);
    auto* cDrive = secondaryButton(QStringLiteral("C盘"));
    auto* dDrive = secondaryButton(QStringLiteral("D盘"));
    auto* refresh = secondaryButton(QStringLiteral("刷新磁盘"));
    fileDiskInfoLabel_ = new QLabel(page);
    connect(diskCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        selectDiskRoot(diskCombo_->currentData().toString());
    });
    connect(cDrive, &QPushButton::clicked, this, [this] { selectDiskRoot(QStringLiteral("C:/")); });
    connect(dDrive, &QPushButton::clicked, this, [this] { selectDiskRoot(QStringLiteral("D:/")); });
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDisks);
    diskRow->addWidget(diskCombo_);
    diskRow->addWidget(cDrive);
    diskRow->addWidget(dDrive);
    diskRow->addWidget(refresh);
    diskRow->addWidget(fileDiskInfoLabel_, 1);
    layout->addLayout(diskRow);

    fileTabs_ = new QTabWidget(page);

    folderUsagePage_ = new QWidget(page);
    auto* usageLayout = new QVBoxLayout(folderUsagePage_);
    usageLayout->setContentsMargins(8, 8, 8, 8);
    auto* scanUsage = primaryButton(QStringLiteral("扫描磁盘空间"));
    connect(scanUsage, &QPushButton::clicked, this, &MainWindow::scanFolderUsage);
    usageLayout->addWidget(scanUsage, 0, Qt::AlignLeft);
    auto* usageSplitter = new QSplitter(Qt::Horizontal, folderUsagePage_);
    folderUsageTree_ = new QTreeWidget(folderUsagePage_);
    folderUsageTree_->setColumnCount(4);
    folderUsageTree_->setHeaderLabels({QStringLiteral("文件夹"), QStringLiteral("占用大小"), QStringLiteral("文件数"), QStringLiteral("百分比")});
    folderUsageTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    installTreeContextMenu(folderUsageTree_, 0);
    folderExtensionTable_ = new QTableWidget(folderUsagePage_);
    folderExtensionTable_->setColumnCount(6);
    folderExtensionTable_->setHorizontalHeaderLabels({QStringLiteral("扩展名"), QStringLiteral("颜色"), QStringLiteral("描述"), QStringLiteral("字节"), QStringLiteral("占比"), QStringLiteral("文件")});
    configureTable(folderExtensionTable_);
    folderExtensionTable_->setMinimumWidth(330);
    folderExtensionTable_->setMaximumWidth(430);
    installTableContextMenu(folderExtensionTable_, -1);
    usageSplitter->addWidget(folderUsageTree_);
    usageSplitter->addWidget(folderExtensionTable_);
    usageSplitter->setStretchFactor(0, 3);
    usageSplitter->setStretchFactor(1, 2);
    usageLayout->addWidget(usageSplitter, 2);
    folderUsageMapScene_ = new QGraphicsScene(folderUsagePage_);
    folderUsageMapView_ = new QGraphicsView(folderUsageMapScene_, folderUsagePage_);
    folderUsageMapView_->setObjectName(QStringLiteral("folderUsageTreemap"));
    folderUsageMapView_->setMinimumHeight(240);
    folderUsageMapView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    folderUsageMapView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    usageLayout->addWidget(folderUsageMapView_, 3);
    fileTabs_->addTab(folderUsagePage_, QStringLiteral("磁盘可视化"));

    auto* filesPage = new QWidget(page);
    auto* filesLayout = new QVBoxLayout(filesPage);
    filesLayout->setContentsMargins(8, 8, 8, 8);
    auto* filterRow = new QHBoxLayout();
    fileTypeCombo_ = new QComboBox(filesPage);
    const QVector<ManagedFileType> types = {ManagedFileType::All, ManagedFileType::Video, ManagedFileType::Image, ManagedFileType::Installer, ManagedFileType::Archive, ManagedFileType::Document};
    for (ManagedFileType type : types) {
        fileTypeCombo_->addItem(FileManagementEngine::typeLabel(type), static_cast<int>(type));
    }
    auto* scanFiles = primaryButton(QStringLiteral("扫描文件"));
    connect(scanFiles, &QPushButton::clicked, this, &MainWindow::scanManagedFiles);
    filterRow->addWidget(new QLabel(QStringLiteral("文件筛选:"), filesPage));
    filterRow->addWidget(fileTypeCombo_);
    filterRow->addWidget(scanFiles);
    filterRow->addStretch();
    filesLayout->addLayout(filterRow);

    managedFileTable_ = new QTableWidget(filesPage);
    managedFileTable_->setColumnCount(4);
    managedFileTable_->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("类型"), QStringLiteral("大小"), QStringLiteral("完整路径")});
    configureTable(managedFileTable_);
    managedFileTable_->setSortingEnabled(true);
    installTableContextMenu(managedFileTable_, 3);
    filesLayout->addWidget(managedFileTable_, 1);

    auto* fileActions = new QGridLayout();
    auto* copy = secondaryButton(QStringLiteral("跨盘复制"));
    auto* move = secondaryButton(QStringLiteral("移动"));
    auto* rename = secondaryButton(QStringLiteral("批量重命名"));
    auto* remove = secondaryButton(QStringLiteral("批量删除"));
    auto* shred = secondaryButton(QStringLiteral("文件粉碎"));
    auto* permission = secondaryButton(QStringLiteral("文件夹权限修复"));
    auto* migrate = primaryButton(QStringLiteral("迁移并生成快捷方式"));
    connect(copy, &QPushButton::clicked, this, &MainWindow::copySelectedFiles);
    connect(move, &QPushButton::clicked, this, &MainWindow::moveSelectedFiles);
    connect(rename, &QPushButton::clicked, this, &MainWindow::renameSelectedFile);
    connect(remove, &QPushButton::clicked, this, &MainWindow::deleteSelectedFiles);
    connect(shred, &QPushButton::clicked, this, &MainWindow::shredSelectedFiles);
    connect(permission, &QPushButton::clicked, this, &MainWindow::repairSelectedFolderPermission);
    connect(migrate, &QPushButton::clicked, this, &MainWindow::migrateSelectedFiles);
    fileActions->addWidget(copy, 0, 0);
    fileActions->addWidget(move, 0, 1);
    fileActions->addWidget(rename, 0, 2);
    fileActions->addWidget(remove, 0, 3);
    fileActions->addWidget(shred, 1, 0);
    fileActions->addWidget(permission, 1, 1);
    fileActions->addWidget(migrate, 1, 2, 1, 2);
    filesLayout->addLayout(fileActions);
    fileTabs_->addTab(filesPage, QStringLiteral("文件筛选与批量操作"));

    auto* migrationPage = new QWidget(page);
    auto* migrationLayout = new QVBoxLayout(migrationPage);
    migrationLayout->setContentsMargins(8, 8, 8, 8);
    auto* warning = new QLabel(QStringLiteral("系统目录迁移前请关闭微信、QQ 和其它占用文件的软件。目标磁盘必须为 NTFS。"), migrationPage);
    warning->setObjectName(QStringLiteral("warningStatus"));
    warning->setWordWrap(true);
    migrationLayout->addWidget(warning);
    auto* targetRow = new QHBoxLayout();
    migrationTargetEdit_ = new QLineEdit(defaultMigrationTargetRoot(), migrationPage);
    migrationTargetEdit_->setPlaceholderText(QStringLiteral("选择非系统盘目标目录"));
    auto* chooseTarget = secondaryButton(QStringLiteral("选择目标"));
    connect(chooseTarget, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择迁移目标"), migrationTargetEdit_->text());
        if (!selected.isEmpty()) {
            migrationTargetEdit_->setText(QDir::toNativeSeparators(selected));
        }
    });
    targetRow->addWidget(migrationTargetEdit_, 1);
    targetRow->addWidget(chooseTarget);
    migrationLayout->addLayout(targetRow);

    migrationTable_ = new QTableWidget(migrationPage);
    migrationTable_->setColumnCount(7);
    migrationTable_->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("目录"), QStringLiteral("风险"), QStringLiteral("状态"), QStringLiteral("占用"), QStringLiteral("原路径"), QStringLiteral("目标路径")});
    configureTable(migrationTable_);
    installTableContextMenu(migrationTable_, 5);
    migrationLayout->addWidget(migrationTable_, 1);
    migrationWatcher_ = new QFutureWatcher<QVector<MigrationFolder>>(this);
    connect(migrationWatcher_, &QFutureWatcher<QVector<MigrationFolder>>::finished, this, &MainWindow::finishMigrationRefresh);
    auto* migrationActions = new QHBoxLayout();
    auto* refreshMigration = secondaryButton(QStringLiteral("刷新状态"));
    auto* migrateFolders = primaryButton(QStringLiteral("迁移选中"));
    auto* restoreFolders = secondaryButton(QStringLiteral("还原选中"));
    auto* restoreAll = secondaryButton(QStringLiteral("还原所有迁移目录"));
    connect(refreshMigration, &QPushButton::clicked, this, &MainWindow::refreshMigrationFolders);
    connect(migrateFolders, &QPushButton::clicked, this, &MainWindow::migrateSelectedFolders);
    connect(restoreFolders, &QPushButton::clicked, this, &MainWindow::restoreSelectedFolders);
    connect(restoreAll, &QPushButton::clicked, this, &MainWindow::restoreAllFolders);
    migrationActions->addWidget(refreshMigration);
    migrationActions->addStretch();
    migrationActions->addWidget(restoreAll);
    migrationActions->addWidget(restoreFolders);
    migrationActions->addWidget(migrateFolders);
    migrationLayout->addLayout(migrationActions);
    fileTabs_->addTab(migrationPage, QStringLiteral("系统目录一键迁移专区"));

    layout->addWidget(fileTabs_, 1);
    fileStatusLabel_ = new QLabel(QStringLiteral("文件管理已就绪。"), page);
    fileStatusLabel_->setObjectName(QStringLiteral("statusStrip"));
    layout->addWidget(fileStatusLabel_);
    return page;
}

QWidget* MainWindow::createRepairPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    layout->addWidget(pageHeader(
        QStringLiteral("CMD 系统修复工具箱"),
        QStringLiteral("仅调用微软原生命令，提供推荐安全修复、深度故障修复和独立可选项。")
    ));

    auto* modeRow = new QHBoxLayout();
    recommendedRepairMode_ = new QCheckBox(QStringLiteral("推荐安全修复"), page);
    deepRepairMode_ = new QCheckBox(QStringLiteral("深度系统修复"), page);
    recommendedRepairMode_->setChecked(true);
    connect(recommendedRepairMode_, &QCheckBox::clicked, this, [this] { updateRepairMode(false); });
    connect(deepRepairMode_, &QCheckBox::clicked, this, [this] { updateRepairMode(true); });
    modeRow->addWidget(recommendedRepairMode_);
    modeRow->addWidget(deepRepairMode_);
    modeRow->addStretch();
    layout->addLayout(modeRow);

    repairTable_ = new QTableWidget(page);
    repairTable_->setColumnCount(6);
    repairTable_->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("修复项"), QStringLiteral("风险"), QStringLiteral("说明"), QStringLiteral("底层命令"), QStringLiteral("单独执行")});
    configureTable(repairTable_);
    reserveActionColumn(repairTable_, 5);
    installTableContextMenu(repairTable_, -1, 4);
    layout->addWidget(repairTable_, 1);

    auto* run = primaryButton(QStringLiteral("执行选中修复"));
    connect(run, &QPushButton::clicked, this, &MainWindow::runSelectedRepairs);
    layout->addWidget(run, 0, Qt::AlignRight);
    repairLog_ = new QTextEdit(page);
    repairLog_->setReadOnly(true);
    repairLog_->setMaximumHeight(130);
    layout->addWidget(repairLog_);
    return page;
}

void MainWindow::applyStyle() {
    qApp->setStyleSheet(QStringLiteral(R"(
        QWidget { font-family: "Microsoft YaHei", "Segoe UI", sans-serif; font-size: 13px; color: #172033; }
        #appRoot, #contentArea { background: #f5f7f6; }
        QScrollArea#pageScrollArea { border: 0; background: #f5f7f6; }
        QScrollArea#pageScrollArea > QWidget > QWidget { background: #f5f7f6; }
        #topNavigation { background: #0f8f5f; }
        #brandTitle { color: white; font-size: 19px; font-weight: 700; }
        #bottomBar { background: white; border-top: 1px solid #d9e1dc; }
        QPushButton { border: 0; border-radius: 6px; padding: 8px 12px; background: #e5e7eb; }
        QPushButton[nav="true"] { color: white; background: transparent; }
        QPushButton[nav="true"][active="true"] { background: rgba(255,255,255,0.22); }
        #primaryButton { background: #10a96f; color: white; font-weight: 600; }
        #secondaryButton { background: #e5f7ef; color: #06754f; font-weight: 600; }
        #pageTitle { font-size: 23px; font-weight: 700; }
        #pageSubtitle { color: #526070; }
        #heroPanel, #statusStrip { background: white; border: 1px solid #d9e1dc; border-radius: 6px; }
        #heroPanel { background: #eef9f3; }
        #statusStrip { padding: 7px 10px; }
        #statPill { background: white; border: 1px solid #ccd7d0; border-radius: 6px; padding: 8px 10px; }
        #safeStatus { color: #06754f; font-weight: 600; }
        #warningStatus { color: #9a5b00; font-weight: 600; }
        QTreeWidget, QTableWidget, QTextEdit, QLineEdit, QComboBox {
            background: white; border: 1px solid #cfd8d2; border-radius: 5px; padding: 3px;
        }
        QGraphicsView#folderUsageTreemap { background: #111827; border: 1px solid #9ca3af; border-radius: 2px; }
        QTabWidget::pane { border: 1px solid #cfd8d2; background: white; border-radius: 5px; }
        QTabBar::tab { padding: 8px 12px; margin-right: 2px; }
        QTabBar::tab:selected { color: #06754f; font-weight: 700; }
    )"));
}

void MainWindow::selectPage(int index) {
    if (!pages_ || index < 0 || index >= pages_->count()) {
        return;
    }
    pages_->setCurrentIndex(index);
    for (QPushButton* button : navButtons_) {
        const bool active = button->property("pageIndex").toInt() == index;
        button->setProperty("active", active);
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
}

QPushButton* MainWindow::navigationButton(const QString& text, int index) {
    auto* button = new QPushButton(text, this);
    button->setProperty("nav", true);
    button->setProperty("pageIndex", index);
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
    layout->setSpacing(4);
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
    const QString line = QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), message);
    QFile file(operationLogPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(line.toUtf8());
        file.write("\n");
    }
    if (globalStatusLabel_) {
        globalStatusLabel_->setText(message.left(120));
    }
}

void MainWindow::showOperationLogDialog() {
    QFile file(operationLogPath());
    QString content = QStringLiteral("暂无日志。");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        content = QString::fromUtf8(file.readAll());
    }
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("全部操作日志"));
    dialog->resize(820, 500);
    auto* layout = new QVBoxLayout(dialog);
    auto* editor = new QTextEdit(dialog);
    editor->setReadOnly(true);
    editor->setPlainText(content);
    auto* close = secondaryButton(QStringLiteral("关闭"));
    connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(editor, 1);
    layout->addWidget(close, 0, Qt::AlignRight);
    dialog->exec();
    dialog->deleteLater();
}

bool MainWindow::isWhitelisted(const QString& path) const {
    if (path.trimmed().isEmpty()) {
        return false;
    }
    QSettings settings;
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const QString& value : settings.value(QStringLiteral("safety/whitelist")).toStringList()) {
        const QString root = QDir::cleanPath(QFileInfo(value).absoluteFilePath());
        if (normalized.compare(root, Qt::CaseInsensitive) == 0
            || normalized.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

void MainWindow::addToWhitelist(const QString& path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    QSettings settings;
    QStringList whitelist = settings.value(QStringLiteral("safety/whitelist")).toStringList();
    const QString normalized = QFileInfo(path).absoluteFilePath();
    if (!whitelist.contains(normalized, Qt::CaseInsensitive)) {
        whitelist.push_back(normalized);
        settings.setValue(QStringLiteral("safety/whitelist"), whitelist);
        showOperationLog(QStringLiteral("加入白名单: %1").arg(QDir::toNativeSeparators(normalized)));
    }
    populateCleanupTree();
}

void MainWindow::setTableRowMetadata(
    QTableWidget* table,
    int row,
    const QString& description,
    const QString& risk,
    const QString& path,
    const QString& command
) {
    const QString tooltip = QStringLiteral("功能说明: %1\n适用场景: 当前列表项目\n风险等级: %2")
        .arg(description, risk);
    for (int column = 0; column < table->columnCount(); ++column) {
        if (QTableWidgetItem* item = table->item(row, column)) {
            item->setToolTip(tooltip);
        }
    }
    QTableWidgetItem* anchor = table->item(row, 0);
    if (anchor) {
        anchor->setData(PathRole, path);
        anchor->setData(CommandRole, command);
    }
}

void MainWindow::installTableContextMenu(QTableWidget* table, int pathColumn, int commandColumn) {
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QTableWidget::customContextMenuRequested, this, [this, table, pathColumn, commandColumn](const QPoint& position) {
        const QModelIndex index = table->indexAt(position);
        if (!index.isValid()) {
            return;
        }
        const int row = index.row();
        QTableWidgetItem* anchor = table->item(row, 0);
        const QString path = pathColumn >= 0 && table->item(row, pathColumn)
            ? table->item(row, pathColumn)->text()
            : (anchor ? anchor->data(PathRole).toString() : QString());
        const QString command = commandColumn >= 0 && table->item(row, commandColumn)
            ? table->item(row, commandColumn)->text()
            : (anchor ? anchor->data(CommandRole).toString() : QString());

        QMenu menu(this);
        QAction* open = menu.addAction(QStringLiteral("打开文件位置"));
        QAction* copy = menu.addAction(QStringLiteral("复制完整路径"));
        QAction* viewCommand = menu.addAction(QStringLiteral("查看底层命令"));
        QAction* run = menu.addAction(QStringLiteral("单独执行"));
        QAction* whitelist = menu.addAction(QStringLiteral("添加白名单"));
        menu.addSeparator();
        QAction* logs = menu.addAction(QStringLiteral("查看操作日志"));
        open->setEnabled(!path.isEmpty());
        copy->setEnabled(!path.isEmpty());
        whitelist->setEnabled(!path.isEmpty());
        viewCommand->setEnabled(!command.isEmpty());
        run->setEnabled(!command.isEmpty());
        QAction* selected = menu.exec(table->viewport()->mapToGlobal(position));
        if (selected == open) {
            const QFileInfo info(path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath()));
        } else if (selected == copy) {
            qApp->clipboard()->setText(QDir::toNativeSeparators(path));
        } else if (selected == viewCommand) {
            QMessageBox::information(this, QStringLiteral("底层命令"), command);
        } else if (selected == run && confirmAction(this, QStringLiteral("单独执行"), QStringLiteral("确认执行以下命令？\n\n%1").arg(command))) {
            int exitCode = 0;
            const QString output = SystemCatalog::runCommand(command, &exitCode);
            showOperationLog(QStringLiteral("右键单独执行命令，退出码 %1").arg(exitCode));
            QMessageBox::information(this, QStringLiteral("执行结果"), output);
        } else if (selected == whitelist) {
            addToWhitelist(path);
        } else if (selected == logs) {
            showOperationLogDialog();
        }
    });
}

void MainWindow::installTreeContextMenu(QTreeWidget* tree, int pathColumn, int commandColumn) {
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, tree, pathColumn, commandColumn](const QPoint& position) {
        QTreeWidgetItem* item = tree->itemAt(position);
        if (!item) {
            return;
        }
        const QString path = pathColumn >= 0 ? item->text(pathColumn) : item->data(0, PathRole).toString();
        const QString command = commandColumn >= 0 ? item->text(commandColumn) : item->data(0, CommandRole).toString();
        QMenu menu(this);
        QAction* open = menu.addAction(QStringLiteral("打开文件位置"));
        QAction* copy = menu.addAction(QStringLiteral("复制完整路径"));
        QAction* viewCommand = menu.addAction(QStringLiteral("查看底层命令"));
        QAction* run = menu.addAction(QStringLiteral("单独执行"));
        QAction* whitelist = menu.addAction(QStringLiteral("添加白名单"));
        menu.addSeparator();
        QAction* logs = menu.addAction(QStringLiteral("查看操作日志"));
        open->setEnabled(!path.isEmpty());
        copy->setEnabled(!path.isEmpty());
        whitelist->setEnabled(!path.isEmpty());
        viewCommand->setEnabled(!command.isEmpty());
        run->setEnabled(!command.isEmpty());
        QAction* selected = menu.exec(tree->viewport()->mapToGlobal(position));
        if (selected == open) {
            const QFileInfo info(path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath()));
        } else if (selected == copy) {
            qApp->clipboard()->setText(QDir::toNativeSeparators(path));
        } else if (selected == viewCommand) {
            QMessageBox::information(this, QStringLiteral("底层命令"), command);
        } else if (selected == run && confirmAction(this, QStringLiteral("单独执行"), QStringLiteral("确认执行以下命令？\n\n%1").arg(command))) {
            int exitCode = 0;
            QMessageBox::information(this, QStringLiteral("执行结果"), SystemCatalog::runCommand(command, &exitCode));
            showOperationLog(QStringLiteral("右键单独执行命令，退出码 %1").arg(exitCode));
        } else if (selected == whitelist) {
            addToWhitelist(path);
        } else if (selected == logs) {
            showOperationLogDialog();
        }
    });
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
    currentScanPath_->setText(QStringLiteral("正在扫描 C 盘清理路径..."));
    scanWatcher_->setFuture(QtConcurrent::run([this] {
        return cleanupEngine_.scanSystem([this](const QString& path, int count) {
            QMetaObject::invokeMethod(this, [this, path, count] {
                currentScanPath_->setText(QStringLiteral("正在扫描: %1 (%2)").arg(QDir::toNativeSeparators(path)).arg(count));
            }, Qt::QueuedConnection);
        });
    }));
}

void MainWindow::finishScan() {
    const CleanupScanResult result = scanWatcher_->result();
    cleanupEntries_ = result.entries;
    scanProgress_->setRange(0, 100);
    scanProgress_->setValue(100);
    populateCleanupTree();
    qint64 reclaim = 0;
    for (const CleanupEntry& entry : cleanupEngine_.entriesForMode(cleanupEntries_, currentCleanMode())) {
        reclaim += entry.size;
    }
    reclaimSpaceLabel_->setText(QStringLiteral("可释放 %1").arg(CleanupEngine::formatSize(reclaim)));
    currentScanPath_->setText(QStringLiteral("扫描完成，共 %1 项。").arg(cleanupEntries_.size()));
    cleanStatusLabel_->setText(QStringLiteral("扫描完成，清理前会自动备份可恢复文件。"));
    refreshDiskInfo();
    showOperationLog(QStringLiteral("C 盘清理扫描完成，共 %1 项").arg(cleanupEntries_.size()));
}

void MainWindow::populateCleanupTree() {
    if (!cleanupTree_) {
        return;
    }
    cleanupTree_->clear();
    const QVector<CleanupEntry> entries = cleanupEngine_.entriesForMode(cleanupEntries_, currentCleanMode());
    QMap<QString, QTreeWidgetItem*> groups;
    for (int i = 0; i < entries.size(); ++i) {
        const CleanupEntry& entry = entries.at(i);
        QTreeWidgetItem* group = groups.value(entry.category);
        if (!group) {
            group = new QTreeWidgetItem(cleanupTree_);
            group->setText(0, entry.category);
            group->setFirstColumnSpanned(false);
            group->setExpanded(true);
            groups.insert(entry.category, group);
        }
        auto* row = new QTreeWidgetItem(group);
        row->setText(0, entry.title);
        row->setText(1, CleanupEngine::formatSize(entry.size));
        row->setText(2, entry.riskLabel);
        row->setText(3, entry.scanOnly ? QStringLiteral("需深度确认") : QStringLiteral("可清理"));
        row->setText(4, QDir::toNativeSeparators(entry.path));
        row->setData(0, Qt::UserRole, i);
        row->setData(0, CommandRole, QStringLiteral("清理引擎删除所列路径并在删除前自动备份"));
        const bool whitelisted = isWhitelisted(entry.path);
        row->setCheckState(0, whitelisted ? Qt::Unchecked : Qt::Checked);
        row->setDisabled(whitelisted);
        const QString tooltip = QStringLiteral("功能说明: %1\n适用场景: %2\n风险等级: %3")
            .arg(entry.title, entry.category, entry.riskLabel);
        for (int column = 0; column < cleanupTree_->columnCount(); ++column) {
            row->setToolTip(column, tooltip);
        }
        if (entry.riskLabel != QStringLiteral("安全")) {
            for (int column = 0; column < cleanupTree_->columnCount(); ++column) {
                row->setBackground(column, QBrush(QColor(QStringLiteral("#fff3cd"))));
            }
        }
    }
}

void MainWindow::updateCleanMode(bool deep) {
    if (deep && deepMode_->isChecked()) {
        if (!confirmAction(
                this,
                QStringLiteral("全选 / 深度风险确认"),
                QStringLiteral("深度模式包含更新缓存、回收站和安装包等谨慎项。请核对完整路径并确认备份后再清理。是否继续？")
            )) {
            deep = false;
        }
    }
    recommendedMode_->setChecked(!deep);
    deepMode_->setChecked(deep);
    populateCleanupTree();
}

CleanupEngine::CleanMode MainWindow::currentCleanMode() const {
    return deepMode_ && deepMode_->isChecked()
        ? CleanupEngine::CleanMode::Deep
        : CleanupEngine::CleanMode::Recommended;
}

QVector<CleanupEntry> MainWindow::selectedCleanupEntries() const {
    const QVector<CleanupEntry> visible = cleanupEngine_.entriesForMode(cleanupEntries_, currentCleanMode());
    QVector<CleanupEntry> selected;
    for (int groupIndex = 0; groupIndex < cleanupTree_->topLevelItemCount(); ++groupIndex) {
        QTreeWidgetItem* group = cleanupTree_->topLevelItem(groupIndex);
        for (int childIndex = 0; childIndex < group->childCount(); ++childIndex) {
            QTreeWidgetItem* item = group->child(childIndex);
            if (item->checkState(0) != Qt::Checked) {
                continue;
            }
            const int index = item->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < visible.size() && !isWhitelisted(visible.at(index).path)) {
                selected.push_back(visible.at(index));
            }
        }
    }
    return selected;
}

void MainWindow::cleanSelected() {
    if (scanWatcher_->isRunning() || (cleanWatcher_ && cleanWatcher_->isRunning())) {
        QMessageBox::information(this, QStringLiteral("清理选中"), QStringLiteral("扫描进行中，请等待完成。"));
        return;
    }
    const QVector<CleanupEntry> selected = selectedCleanupEntries();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("清理选中"), QStringLiteral("请先扫描并勾选需要清理的项目。"));
        return;
    }
    if (!confirmAction(
            this,
            QStringLiteral("清理确认"),
            QStringLiteral("将清理 %1 项。可恢复文件会在删除前自动备份；回收站清空无法备份。是否继续？").arg(selected.size())
        )) {
        return;
    }
    CleanOptions options;
    options.backup = true;
    options.simulate = false;
    options.allowScanOnly = currentCleanMode() == CleanupEngine::CleanMode::Deep;
    options.backupRoot = backupRoot_;
    cleanStatusLabel_->setText(QStringLiteral("正在清理并备份选中项目..."));
    cleanWatcher_->setFuture(QtConcurrent::run([selected, options] {
        return CleanupEngine().cleanEntriesDetailed(selected, options);
    }));
}

void MainWindow::openBackupManager() {
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("备份与恢复"));
    dialog->resize(820, 480);
    auto* layout = new QVBoxLayout(dialog);
    BackupInfo info = CleanupEngine::backupInfo(backupRoot_);
    auto* summary = new QLabel(QStringLiteral("备份目录: %1\n文件数: %2，大小: %3")
        .arg(QDir::toNativeSeparators(info.backupRoot))
        .arg(info.backups.size())
        .arg(CleanupEngine::formatSize(info.totalBytes)), dialog);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summary);
    auto* table = new QTableWidget(dialog);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("大小"), QStringLiteral("原路径"), QStringLiteral("备份路径")});
    configureTable(table);
    table->setRowCount(info.backups.size());
    for (int row = 0; row < info.backups.size(); ++row) {
        table->setItem(row, 0, textItem(info.backups.at(row).createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        table->setItem(row, 1, textItem(CleanupEngine::formatSize(info.backups.at(row).size)));
        table->setItem(row, 2, textItem(info.backups.at(row).sourcePath));
        table->setItem(row, 3, textItem(info.backups.at(row).backupPath));
    }
    layout->addWidget(table, 1);
    auto* buttons = new QHBoxLayout();
    auto* chooseRoot = secondaryButton(QStringLiteral("更改备份目录"));
    auto* restore = primaryButton(QStringLiteral("恢复选中"));
    auto* close = secondaryButton(QStringLiteral("关闭"));
    connect(chooseRoot, &QPushButton::clicked, dialog, [this, dialog] {
        const QString selected = QFileDialog::getExistingDirectory(dialog, QStringLiteral("选择备份目录"), CleanupEngine::backupRoot());
        if (!selected.isEmpty()) {
            backupRoot_ = selected;
            showOperationLog(QStringLiteral("更改清理备份目录: %1").arg(selected));
        }
    });
    connect(restore, &QPushButton::clicked, dialog, [this, table, info] {
        const QModelIndexList rows = table->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("恢复备份"), QStringLiteral("请先选择备份文件。"));
            return;
        }
        int restored = 0;
        QStringList errors;
        for (const QModelIndex& index : rows) {
            QString error;
            if (CleanupEngine::restoreBackupItem(info.backups.at(index.row()), &error)) {
                ++restored;
            } else {
                errors.push_back(error);
            }
        }
        showOperationLog(QStringLiteral("恢复清理备份 %1 个，失败 %2 个").arg(restored).arg(errors.size()));
        QMessageBox::information(this, QStringLiteral("恢复备份"), QStringLiteral("恢复 %1 个，失败 %2 个。\n%3").arg(restored).arg(errors.size()).arg(errors.join(QStringLiteral("\n"))));
    });
    connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    buttons->addWidget(chooseRoot);
    buttons->addStretch();
    buttons->addWidget(restore);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::refreshInstalledApps() {
    if (!uninstallTable_ || !uninstallWatcher_ || uninstallWatcher_->isRunning()) {
        return;
    }
    globalStatusLabel_->setText(QStringLiteral("正在读取本地软件和微软商店应用..."));
    uninstallTable_->setRowCount(0);
    uninstallWatcher_->setFuture(QtConcurrent::run([] {
        return SoftwareUninstallEngine().installedApplications();
    }));
}

void MainWindow::populateUninstallTable() {
    uninstallTable_->setSortingEnabled(false);
    uninstallTable_->setRowCount(0);
    for (int i = 0; i < installedApps_.size(); ++i) {
        const InstalledApplication& app = installedApps_.at(i);
        const int row = uninstallTable_->rowCount();
        uninstallTable_->insertRow(row);
        auto* check = new QTableWidgetItem();
        check->setCheckState(Qt::Unchecked);
        check->setData(AppIndexRole, i);
        uninstallTable_->setItem(row, 0, check);
        uninstallTable_->setItem(row, 1, textItem(app.name));
        uninstallTable_->setItem(row, 2, textItem(app.storeApp ? QStringLiteral("微软商店") : QStringLiteral("桌面程序")));
        auto* size = textItem(app.sizeBytes > 0 ? CleanupEngine::formatSize(app.sizeBytes) : QStringLiteral("--"));
        size->setData(Qt::UserRole, app.sizeBytes);
        uninstallTable_->setItem(row, 3, size);
        uninstallTable_->setItem(row, 4, textItem(app.installLocation));
        uninstallTable_->setItem(row, 5, textItem(app.installDate.isEmpty() ? QStringLiteral("--") : app.installDate));
        uninstallTable_->setItem(row, 6, textItem(app.version));
        uninstallTable_->setItem(row, 7, textItem(app.publisher));
        auto* normal = secondaryButton(QStringLiteral("卸载"));
        auto* strong = primaryButton(QStringLiteral("强力"));
        normal->setEnabled(!app.uninstallCommand.isEmpty());
        strong->setEnabled(!app.uninstallCommand.isEmpty());
        connect(normal, &QPushButton::clicked, this, [this, i] { uninstallApplication(i, false); });
        connect(strong, &QPushButton::clicked, this, [this, i] { uninstallApplication(i, true); });
        uninstallTable_->setCellWidget(row, 8, normal);
        uninstallTable_->setCellWidget(row, 9, strong);
        setTableRowMetadata(uninstallTable_, row, QStringLiteral("卸载 %1；卸载前自动备份注册表，完成后可扫描残留。").arg(app.name), QStringLiteral("谨慎"), app.installLocation, app.uninstallCommand);
    }
    uninstallTable_->setSortingEnabled(true);
}

void MainWindow::filterInstalledApps(const QString& text) {
    if (!uninstallTable_) {
        return;
    }
    const QString query = text.trimmed();
    for (int row = 0; row < uninstallTable_->rowCount(); ++row) {
        QString content;
        for (int column : {1, 4, 7}) {
            if (QTableWidgetItem* item = uninstallTable_->item(row, column)) {
                content += QLatin1Char(' ') + item->text();
            }
        }
        uninstallTable_->setRowHidden(row, !query.isEmpty() && !content.contains(query, Qt::CaseInsensitive));
    }
}

void MainWindow::uninstallApplication(int appIndex, bool strong) {
    if (appIndex < 0 || appIndex >= installedApps_.size()) {
        return;
    }
    const InstalledApplication app = installedApps_.at(appIndex);
    const QString prompt = strong
        ? QStringLiteral("将启动卸载程序，并在随后删除安装目录、残留文件、卸载注册表项、启动项和快捷方式。此操作风险较高。")
        : QStringLiteral("将启动标准卸载流程。卸载前会自动导出注册表项。");
    if (!confirmAction(this, strong ? QStringLiteral("强力粉碎卸载") : QStringLiteral("常规卸载"), QStringLiteral("%1\n\n软件: %2").arg(prompt, app.name))) {
        return;
    }
    const UninstallResult result = uninstallEngine_.startUninstall(app);
    if (!result.errors.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("软件卸载"), result.errors.join(QStringLiteral("\n")));
        return;
    }
    showOperationLog(QStringLiteral("启动%1: %2，注册表备份: %3")
        .arg(strong ? QStringLiteral("强力卸载") : QStringLiteral("常规卸载"), app.name, result.registryBackup));
    if (strong) {
        QTimer::singleShot(1000, this, [this, app] { cleanApplicationResiduals(app); });
    }
    QTimer::singleShot(7000, this, &MainWindow::refreshInstalledApps);
}

void MainWindow::uninstallSelectedApplications() {
    QVector<int> indexes;
    for (int row = 0; row < uninstallTable_->rowCount(); ++row) {
        QTableWidgetItem* check = uninstallTable_->item(row, 0);
        if (check && check->checkState() == Qt::Checked) {
            indexes.push_back(check->data(AppIndexRole).toInt());
        }
    }
    if (indexes.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("批量卸载"), QStringLiteral("请先勾选软件。"));
        return;
    }
    if (!confirmAction(this, QStringLiteral("批量卸载"), QStringLiteral("将依次启动 %1 个软件的标准卸载程序。是否继续？").arg(indexes.size()))) {
        return;
    }
    int started = 0;
    QStringList errors;
    for (int index : indexes) {
        const UninstallResult result = uninstallEngine_.startUninstall(installedApps_.at(index));
        if (result.started) {
            ++started;
        }
        errors.append(result.errors);
    }
    showOperationLog(QStringLiteral("批量卸载启动 %1 项，失败 %2 项").arg(started).arg(errors.size()));
    QMessageBox::information(this, QStringLiteral("批量卸载"), QStringLiteral("已启动 %1 项，失败 %2 项。\n%3").arg(started).arg(errors.size()).arg(errors.join(QStringLiteral("\n"))));
}

void MainWindow::cleanApplicationResiduals(const InstalledApplication& app) {
    const QStringList residuals = uninstallEngine_.findResidualPaths(app);
    if (residuals.isEmpty()) {
        showOperationLog(QStringLiteral("卸载残留扫描完成: %1，无残留").arg(app.name));
        return;
    }
    if (!confirmAction(
            this,
            QStringLiteral("清理卸载残留"),
            QStringLiteral("请先在卸载窗口中完成卸载并关闭卸载程序。\n\n检测到 %1 的残留路径:\n%2\n\n确认卸载已完成，并删除这些残留？")
                .arg(app.name, residuals.join(QStringLiteral("\n")))
        )) {
        return;
    }
    const UninstallResult result = uninstallEngine_.cleanResiduals(app);
    showOperationLog(QStringLiteral("清理卸载残留: %1，删除 %2，失败 %3").arg(app.name).arg(result.removedPaths.size()).arg(result.errors.size()));
    QMessageBox::information(this, QStringLiteral("残留清理"), QStringLiteral("删除 %1 项，失败 %2 项。\n%3").arg(result.removedPaths.size()).arg(result.errors.size()).arg(result.errors.join(QStringLiteral("\n"))));
}

void MainWindow::populateOptimizationTable(QTableWidget* table, const QVector<WindowsOptimizationAction>& actions) {
    table->setRowCount(0);
    for (int i = 0; i < actions.size(); ++i) {
        const WindowsOptimizationAction& action = actions.at(i);
        const int row = table->rowCount();
        table->insertRow(row);
        auto* check = new QTableWidgetItem();
        check->setCheckState(action.recommended ? Qt::Checked : Qt::Unchecked);
        check->setData(Qt::UserRole, i);
        table->setItem(row, 0, check);
        table->setItem(row, 1, textItem(action.category));
        table->setItem(row, 2, textItem(action.title));
        table->setItem(row, 3, textItem(action.riskLabel));
        table->setItem(row, 4, textItem(action.description));
        auto* apply = primaryButton(QStringLiteral("执行"));
        auto* restore = secondaryButton(QStringLiteral("还原"));
        restore->setEnabled(!action.revertCommands.isEmpty());
        connect(apply, &QPushButton::clicked, this, [this, action] { runOptimizationAction(action, false); });
        connect(restore, &QPushButton::clicked, this, [this, action] { runOptimizationAction(action, true); });
        table->setCellWidget(row, 5, apply);
        table->setCellWidget(row, 6, restore);
        setTableRowMetadata(table, row, action.description, action.riskLabel, {}, commandsText(action.commands));
        if (action.riskLabel.contains(QStringLiteral("高危")) || action.riskLabel.contains(QStringLiteral("谨慎"))) {
            for (int column = 0; column < 5; ++column) {
                table->item(row, column)->setBackground(QBrush(riskColor(action.riskLabel)));
            }
        }
    }
}

void MainWindow::runOptimizationAction(const WindowsOptimizationAction& action, bool revert) {
    const QVector<WindowsOptimizationCommand> commands = revert ? action.revertCommands : action.commands;
    if (commands.isEmpty()) {
        return;
    }
    if (!confirmAction(
            this,
            revert ? QStringLiteral("确认还原") : QStringLiteral("确认执行"),
            QStringLiteral("%1\n\n%2\n风险等级: %3\n\n底层命令:\n%4")
                .arg(action.title, action.description, action.riskLabel, commandsText(commands))
        )) {
        return;
    }
    globalStatusLabel_->setText(QStringLiteral("正在%1: %2").arg(revert ? QStringLiteral("还原") : QStringLiteral("执行"), action.title));
    auto* watcher = new QFutureWatcher<CommandBatchResult>(this);
    connect(watcher, &QFutureWatcher<CommandBatchResult>::finished, this, [this, watcher, action, revert] {
        const CommandBatchResult result = watcher->result();
        const int exitCode = result.exitCodes.value(action.id, -1);
        if (exitCode == 0) {
            setActionApplied(OptimizationStateKey, action.id, !revert);
        }
        showOperationLog(QStringLiteral("%1优化项 %2，退出码 %3").arg(revert ? QStringLiteral("还原") : QStringLiteral("执行"), action.title).arg(exitCode));
        QMessageBox::information(this, action.title, QStringLiteral("退出码 %1\n%2").arg(exitCode).arg(result.output));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([action, commands] {
        CommandBatchResult result;
        int exitCode = 0;
        result.output = SystemCatalog::runActionCommands(commands, &exitCode);
        result.exitCodes.insert(action.id, exitCode);
        return result;
    }));
}

void MainWindow::applyOptimizationPreset(
    QTableWidget* table,
    const QVector<WindowsOptimizationAction>& actions,
    const QString& title,
    bool deep
) {
    QVector<WindowsOptimizationAction> selected;
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* check = table->item(row, 0);
        if (check && check->checkState() == Qt::Checked) {
            const int index = check->data(Qt::UserRole).toInt();
            if (index >= 0 && index < actions.size()) {
                selected.push_back(actions.at(index));
            }
        }
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, title, QStringLiteral("没有勾选优化项。"));
        return;
    }
    const QString notice = deep
        ? QStringLiteral("电竞模式会限制通知、搜索、休眠、系统还原等办公功能。")
        : QStringLiteral("办公模式只执行当前勾选的稳定优化项。");
    if (!confirmAction(this, title, QStringLiteral("%1\n\n将执行 %2 项，是否继续？").arg(notice).arg(selected.size()))) {
        return;
    }
    globalStatusLabel_->setText(QStringLiteral("正在执行: %1").arg(title));
    auto* watcher = new QFutureWatcher<CommandBatchResult>(this);
    connect(watcher, &QFutureWatcher<CommandBatchResult>::finished, this, [this, watcher, selected, title] {
        const CommandBatchResult result = watcher->result();
        for (const WindowsOptimizationAction& action : selected) {
            const int exitCode = result.exitCodes.value(action.id, -1);
            if (exitCode == 0) {
                setActionApplied(OptimizationStateKey, action.id, true);
            }
            showOperationLog(QStringLiteral("%1: %2，退出码 %3").arg(title, action.title).arg(exitCode));
        }
        QMessageBox::information(this, title, result.output.left(12000));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([selected] {
        CommandBatchResult result;
        QStringList outputs;
        for (const WindowsOptimizationAction& action : selected) {
            int exitCode = 0;
            outputs.push_back(QStringLiteral("[%1]\n%2")
                .arg(action.title, SystemCatalog::runActionCommands(action.commands, &exitCode)));
            result.exitCodes.insert(action.id, exitCode);
        }
        result.output = outputs.join(QStringLiteral("\n\n"));
        return result;
    }));
}

void MainWindow::refreshGpuInfo() {
    if (!gpuInfoTable_ || !gpuActionTable_ || !gpuWatcher_ || gpuWatcher_->isRunning()) {
        return;
    }
    gpuLog_->append(QStringLiteral("[%1] 正在检测显卡与官方支持入口...")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    gpuWatcher_->setFuture(QtConcurrent::run([] {
        return GpuOptimizationEngine().detectDevices();
    }));
}

void MainWindow::finishGpuRefresh() {
    gpuDevices_ = gpuWatcher_->result();
    bool dedicated = false;
    gpuInfoTable_->setRowCount(0);
    for (const GpuDeviceInfo& device : gpuDevices_) {
        dedicated = dedicated || device.vendor == QStringLiteral("NVIDIA") || device.vendor == QStringLiteral("AMD");
        const int row = gpuInfoTable_->rowCount();
        gpuInfoTable_->insertRow(row);
        gpuInfoTable_->setItem(row, 0, textItem(device.vendor));
        gpuInfoTable_->setItem(row, 1, textItem(device.name));
        gpuInfoTable_->setItem(row, 2, textItem(device.driverVersion.isEmpty() ? QStringLiteral("--") : device.driverVersion));
        gpuInfoTable_->setItem(row, 3, textItem(device.memoryMB > 0 ? QStringLiteral("%1 MB").arg(device.memoryMB) : QStringLiteral("--")));
        gpuInfoTable_->setItem(row, 4, textItem(device.temperatureC >= 0 ? QStringLiteral("%1 C").arg(device.temperatureC) : QStringLiteral("--")));
        gpuInfoTable_->setItem(row, 5, textItem(device.loadPercent >= 0 ? QStringLiteral("%1%").arg(device.loadPercent) : QStringLiteral("--")));
        gpuInfoTable_->setItem(row, 6, textItem(device.capabilities.isEmpty() ? QStringLiteral("基础检测") : device.capabilities.join(QStringLiteral(", "))));
        setTableRowMetadata(gpuInfoTable_, row, QStringLiteral("显示驱动、显存、温度、负载和厂商支持入口。"), QStringLiteral("只读"));
    }
    if (optimizationTabs_ && gpuTabIndex_ >= 0) {
        optimizationTabs_->setTabVisible(gpuTabIndex_, dedicated);
    }
    populateGpuActions();
    if (gpuLog_) {
        gpuLog_->append(QStringLiteral("[%1] 检测到 %2 个设备，%3 个可执行操作。")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
            .arg(gpuDevices_.size())
            .arg(gpuActions_.size()));
    }
}

void MainWindow::populateGpuActions() {
    gpuActions_ = gpuEngine_.supportedActions(gpuDevices_);
    gpuActionTable_->setRowCount(0);
    for (int i = 0; i < gpuActions_.size(); ++i) {
        const GpuOptimizationAction& action = gpuActions_.at(i);
        if (!action.supported) {
            continue;
        }
        const int row = gpuActionTable_->rowCount();
        gpuActionTable_->insertRow(row);
        gpuActionTable_->setItem(row, 0, textItem(action.vendor));
        gpuActionTable_->setItem(row, 1, textItem(action.title));
        gpuActionTable_->setItem(row, 2, textItem(action.riskLabel));
        gpuActionTable_->setItem(row, 3, textItem(action.description));
        auto* apply = primaryButton(action.modifiesSystem ? QStringLiteral("执行") : QStringLiteral("查看"));
        auto* restore = secondaryButton(QStringLiteral("还原"));
        restore->setEnabled(!action.revertCommands.isEmpty());
        connect(apply, &QPushButton::clicked, this, [this, i] { runGpuAction(i, false); });
        connect(restore, &QPushButton::clicked, this, [this, i] { runGpuAction(i, true); });
        gpuActionTable_->setCellWidget(row, 4, apply);
        gpuActionTable_->setCellWidget(row, 5, restore);
        setTableRowMetadata(gpuActionTable_, row, action.description, action.riskLabel, {}, commandsText(action.commands));
    }
}

void MainWindow::runGpuAction(int actionIndex, bool revert) {
    if (actionIndex < 0 || actionIndex >= gpuActions_.size()) {
        return;
    }
    const GpuOptimizationAction action = gpuActions_.at(actionIndex);
    const QVector<WindowsOptimizationCommand> commands = revert ? action.revertCommands : action.commands;
    if (commands.isEmpty()) {
        return;
    }
    if ((action.modifiesSystem || revert) && !confirmAction(
            this,
            revert ? QStringLiteral("还原显卡设置") : QStringLiteral("执行显卡优化"),
            QStringLiteral("%1\n\n%2\n风险: %3").arg(action.title, action.description, action.riskLabel)
        )) {
        return;
    }
    globalStatusLabel_->setText(QStringLiteral("正在%1: %2").arg(revert ? QStringLiteral("还原") : QStringLiteral("执行"), action.title));
    auto* watcher = new QFutureWatcher<CommandBatchResult>(this);
    connect(watcher, &QFutureWatcher<CommandBatchResult>::finished, this, [this, watcher, action, revert] {
        const CommandBatchResult result = watcher->result();
        const int exitCode = result.exitCodes.value(action.id, -1);
        if (exitCode == 0) {
            if (action.id == QStringLiteral("gpu_restore_defaults")) {
                QSettings().remove(GpuStateKey);
            } else {
                setActionApplied(GpuStateKey, action.id, !revert);
            }
        }
        const QString log = QStringLiteral("%1显卡操作 %2，退出码 %3").arg(revert ? QStringLiteral("还原") : QStringLiteral("执行"), action.title).arg(exitCode);
        gpuLog_->append(log);
        showOperationLog(log);
        QMessageBox::information(this, action.title, QStringLiteral("退出码 %1\n%2").arg(exitCode).arg(result.output));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([action, commands] {
        CommandBatchResult result;
        int exitCode = 0;
        result.output = SystemCatalog::runActionCommands(commands, &exitCode);
        result.exitCodes.insert(action.id, exitCode);
        return result;
    }));
}

void MainWindow::refreshDisks() {
    if (!diskCombo_) {
        return;
    }
    const QString previous = fileRoot_;
    const QSignalBlocker blocker(diskCombo_);
    diskCombo_->clear();
    for (QStorageInfo storage : QStorageInfo::mountedVolumes()) {
        storage.refresh();
        if (!storage.isValid() || !storage.isReady() || storage.rootPath().isEmpty()) {
            continue;
        }
        const QString root = QDir::toNativeSeparators(storage.rootPath());
        const qint64 used = qMax<qint64>(0, storage.bytesTotal() - storage.bytesAvailable());
        diskCombo_->addItem(
            QStringLiteral("%1  总量 %2 / 已用 %3 / 剩余 %4")
                .arg(root, CleanupEngine::formatSize(storage.bytesTotal()), CleanupEngine::formatSize(used), CleanupEngine::formatSize(storage.bytesAvailable())),
            storage.rootPath()
        );
    }
    int index = diskCombo_->findData(previous);
    if (index < 0 && diskCombo_->count() > 0) {
        index = 0;
    }
    if (index >= 0) {
        diskCombo_->setCurrentIndex(index);
        selectDiskRoot(diskCombo_->itemData(index).toString());
    }
    showOperationLog(QStringLiteral("刷新磁盘列表，共 %1 个").arg(diskCombo_->count()));
}

void MainWindow::selectDiskRoot(const QString& rootPath) {
    if (rootPath.trimmed().isEmpty() || !QFileInfo(rootPath).isDir()) {
        return;
    }
    fileRoot_ = QDir::cleanPath(rootPath);
    QStorageInfo storage(fileRoot_);
    storage.refresh();
    const qint64 used = qMax<qint64>(0, storage.bytesTotal() - storage.bytesAvailable());
    if (fileDiskInfoLabel_) {
        fileDiskInfoLabel_->setText(QStringLiteral("%1 / %2 可用")
            .arg(CleanupEngine::formatSize(used), CleanupEngine::formatSize(storage.bytesAvailable())));
    }
    if (fileStatusLabel_) {
        fileStatusLabel_->setText(QStringLiteral("当前磁盘: %1").arg(QDir::toNativeSeparators(fileRoot_)));
    }
    if (diskCombo_) {
        const int index = diskCombo_->findData(rootPath);
        if (index >= 0 && diskCombo_->currentIndex() != index) {
            const QSignalBlocker blocker(diskCombo_);
            diskCombo_->setCurrentIndex(index);
        }
    }
}

void MainWindow::scanFolderUsage() {
    const QString root = fileRoot_.isEmpty() ? defaultDiskRoot() : fileRoot_;
    fileStatusLabel_->setText(QStringLiteral("正在扫描空间占用: %1").arg(QDir::toNativeSeparators(root)));
    auto* watcher = new QFutureWatcher<FolderUsageScan>(this);
    connect(watcher, &QFutureWatcher<FolderUsageScan>::finished, this, [this, watcher] {
        populateFolderUsage(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([root] {
        return FileManagementEngine().scanFolderUsageDetailed(root, 80, 60000);
    }));
}

void MainWindow::scanManagedFiles() {
    const QString root = fileRoot_.isEmpty() ? defaultDiskRoot() : fileRoot_;
    const ManagedFileType type = static_cast<ManagedFileType>(fileTypeCombo_->currentData().toInt());
    fileStatusLabel_->setText(QStringLiteral("正在扫描%1文件...").arg(FileManagementEngine::typeLabel(type)));
    auto* watcher = new QFutureWatcher<QVector<ManagedFileEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<ManagedFileEntry>>::finished, this, [this, watcher] {
        populateManagedFiles(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([root, type] {
        return FileManagementEngine().listFiles(root, type, 5000);
    }));
}

void MainWindow::populateManagedFiles(const QVector<ManagedFileEntry>& files) {
    managedFiles_ = files;
    managedFileTable_->setSortingEnabled(false);
    managedFileTable_->setRowCount(0);
    for (const ManagedFileEntry& file : files) {
        const int row = managedFileTable_->rowCount();
        managedFileTable_->insertRow(row);
        managedFileTable_->setItem(row, 0, textItem(file.name));
        managedFileTable_->setItem(row, 1, textItem(FileManagementEngine::typeLabel(file.type)));
        auto* size = textItem(CleanupEngine::formatSize(file.sizeBytes));
        size->setData(Qt::UserRole, file.sizeBytes);
        managedFileTable_->setItem(row, 2, size);
        managedFileTable_->setItem(row, 3, textItem(QDir::toNativeSeparators(file.path)));
        setTableRowMetadata(managedFileTable_, row, QStringLiteral("文件筛选结果，可执行复制、移动、重命名、删除、粉碎或迁移。"), QStringLiteral("按操作确定"), file.path);
    }
    managedFileTable_->setSortingEnabled(true);
    fileStatusLabel_->setText(QStringLiteral("文件扫描完成: %1 个").arg(files.size()));
    showOperationLog(QStringLiteral("文件扫描完成: %1 个").arg(files.size()));
}

QStringList MainWindow::selectedManagedPaths() const {
    QStringList paths;
    const QModelIndexList rows = managedFileTable_->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows) {
        if (QTableWidgetItem* item = managedFileTable_->item(index.row(), 3)) {
            const QString path = item->text();
            if (!isWhitelisted(path)) {
                paths.push_back(path);
            }
        }
    }
    return paths;
}

void MainWindow::copySelectedFiles() {
    const QStringList paths = selectedManagedPaths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("跨盘复制"), QStringLiteral("请选择未加入白名单的文件。"));
        return;
    }
    const QString target = QFileDialog::getExistingDirectory(this, QStringLiteral("选择复制目标"), fileRoot_);
    if (target.isEmpty()) {
        return;
    }
    const FileOperationResult result = fileEngine_.copyFiles(paths, target);
    showOperationLog(QStringLiteral("复制文件 %1 个，失败 %2 个").arg(result.affectedPaths.size()).arg(result.errors.size()));
    QMessageBox::information(this, QStringLiteral("复制完成"), QStringLiteral("成功 %1，失败 %2。\n%3").arg(result.affectedPaths.size()).arg(result.errors.size()).arg(result.errors.join(QStringLiteral("\n"))));
}

void MainWindow::moveSelectedFiles() {
    const QStringList paths = selectedManagedPaths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("移动文件"), QStringLiteral("请选择未加入白名单的文件。"));
        return;
    }
    const QString target = QFileDialog::getExistingDirectory(this, QStringLiteral("选择移动目标"), fileRoot_);
    if (target.isEmpty() || !confirmAction(this, QStringLiteral("移动文件"), QStringLiteral("将移动 %1 个文件到 %2，是否继续？").arg(paths.size()).arg(target))) {
        return;
    }
    const FileOperationResult result = fileEngine_.moveFiles(paths, target);
    showOperationLog(QStringLiteral("移动文件 %1 个，失败 %2 个").arg(result.affectedPaths.size()).arg(result.errors.size()));
    scanManagedFiles();
}

void MainWindow::renameSelectedFile() {
    const QStringList paths = selectedManagedPaths();
    if (paths.size() != 1) {
        QMessageBox::information(this, QStringLiteral("批量重命名"), QStringLiteral("当前版本请逐个选择文件重命名。"));
        return;
    }
    bool ok = false;
    const QString newName = QInputDialog::getText(this, QStringLiteral("重命名"), QStringLiteral("新文件名:"), QLineEdit::Normal, QFileInfo(paths.first()).fileName(), &ok);
    if (!ok || newName.trimmed().isEmpty()) {
        return;
    }
    const FileOperationResult result = fileEngine_.renameFile(paths.first(), newName.trimmed());
    showOperationLog(QStringLiteral("重命名文件: %1").arg(paths.first()));
    if (!result.errors.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("重命名"), result.errors.join(QStringLiteral("\n")));
    }
    scanManagedFiles();
}

void MainWindow::deleteSelectedFiles() {
    const QStringList paths = selectedManagedPaths();
    if (paths.isEmpty() || !confirmAction(this, QStringLiteral("批量删除"), QStringLiteral("将永久删除 %1 个文件。是否继续？").arg(paths.size()))) {
        return;
    }
    const FileOperationResult result = fileEngine_.deleteFiles(paths);
    showOperationLog(QStringLiteral("删除文件 %1 个，失败 %2 个").arg(result.affectedPaths.size()).arg(result.errors.size()));
    scanManagedFiles();
}

void MainWindow::shredSelectedFiles() {
    const QStringList paths = selectedManagedPaths();
    if (paths.isEmpty() || !confirmAction(this, QStringLiteral("文件粉碎"), QStringLiteral("将覆写并永久删除 %1 个文件，无法恢复。是否继续？").arg(paths.size()))) {
        return;
    }
    const FileOperationResult result = fileEngine_.shredFiles(paths);
    showOperationLog(QStringLiteral("粉碎文件 %1 个，失败 %2 个").arg(result.affectedPaths.size()).arg(result.errors.size()));
    scanManagedFiles();
}

void MainWindow::repairSelectedFolderPermission() {
    QString path;
    const QStringList paths = selectedManagedPaths();
    if (!paths.isEmpty()) {
        path = QFileInfo(paths.first()).absolutePath();
    } else {
        path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择权限修复目录"), fileRoot_);
    }
    if (path.isEmpty() || !confirmAction(this, QStringLiteral("文件夹权限修复"), QStringLiteral("将重置目录 ACL 权限:\n%1").arg(path))) {
        return;
    }
    const FileOperationResult result = fileEngine_.repairFolderPermission(path);
    showOperationLog(QStringLiteral("修复文件夹权限: %1").arg(path));
    QMessageBox::information(this, QStringLiteral("权限修复"), result.errors.isEmpty() ? result.output : result.errors.join(QStringLiteral("\n")));
}

void MainWindow::migrateSelectedFiles() {
    const QStringList paths = selectedManagedPaths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("普通文件迁移"), QStringLiteral("请先选择文件。"));
        return;
    }
    const QString target = QFileDialog::getExistingDirectory(this, QStringLiteral("选择迁移目标磁盘目录"), fileRoot_);
    if (target.isEmpty() || !confirmAction(this, QStringLiteral("普通文件迁移"), QStringLiteral("将移动 %1 个文件，并在原位置生成快捷方式。是否继续？").arg(paths.size()))) {
        return;
    }
    const FileOperationResult moved = fileEngine_.moveFiles(paths, target);
    QStringList errors = moved.errors;
    for (const QString& source : paths) {
        const QString destination = QDir(target).filePath(QFileInfo(source).fileName());
        if (!QFileInfo::exists(destination)) {
            continue;
        }
        const FileOperationResult shortcut = fileEngine_.createShortcut(destination, source + QStringLiteral(".lnk"));
        errors.append(shortcut.errors);
    }
    showOperationLog(QStringLiteral("普通文件迁移 %1 个，失败 %2 个").arg(moved.affectedPaths.size()).arg(errors.size()));
    QMessageBox::information(this, QStringLiteral("普通文件迁移"), QStringLiteral("迁移 %1 个，失败 %2 个。\n%3").arg(moved.affectedPaths.size()).arg(errors.size()).arg(errors.join(QStringLiteral("\n"))));
    scanManagedFiles();
}

void MainWindow::populateFolderUsage(const FolderUsageScan& scan) {
    folderUsageTree_->clear();
    qint64 total = 0;
    int totalFileCount = 0;
    for (const ExtensionUsageEntry& entry : scan.extensions) {
        total += entry.sizeBytes;
        totalFileCount += entry.fileCount;
    }
    for (const FolderUsageEntry& entry : scan.folders) {
        auto* row = new QTreeWidgetItem(folderUsageTree_);
        row->setText(0, QDir::toNativeSeparators(entry.path));
        row->setText(1, CleanupEngine::formatSize(entry.sizeBytes));
        row->setText(2, QString::number(entry.fileCount));
        row->setText(3, total > 0 ? QStringLiteral("%1%").arg(entry.sizeBytes * 100.0 / total, 0, 'f', 1) : QStringLiteral("--"));
        row->setToolTip(0, QStringLiteral("功能说明: 文件夹占用排行\n适用场景: 定位空间占用\n风险等级: 只读"));
    }
    populateExtensionUsageTable(scan.extensions, total);
    populateFolderUsageTreemap(scan.files, total, totalFileCount);
    fileStatusLabel_->setText(QStringLiteral("空间扫描完成: %1 个目录，%2 个文件").arg(scan.folders.size()).arg(totalFileCount));
    showOperationLog(fileStatusLabel_->text());
}

void MainWindow::populateExtensionUsageTable(const QVector<ExtensionUsageEntry>& entries, qint64 totalBytes) {
    folderExtensionTable_->setRowCount(0);
    for (const ExtensionUsageEntry& entry : entries) {
        const int row = folderExtensionTable_->rowCount();
        folderExtensionTable_->insertRow(row);
        folderExtensionTable_->setItem(row, 0, textItem(entry.extension));
        auto* color = textItem(QStringLiteral(" "));
        color->setBackground(QBrush(extensionColor(entry.extension)));
        folderExtensionTable_->setItem(row, 1, color);
        folderExtensionTable_->setItem(row, 2, textItem(entry.description));
        folderExtensionTable_->setItem(row, 3, textItem(CleanupEngine::formatSize(entry.sizeBytes)));
        folderExtensionTable_->setItem(row, 4, textItem(totalBytes > 0 ? QStringLiteral("%1%").arg(entry.sizeBytes * 100.0 / totalBytes, 0, 'f', 1) : QStringLiteral("--")));
        folderExtensionTable_->setItem(row, 5, textItem(QString::number(entry.fileCount)));
        setTableRowMetadata(folderExtensionTable_, row, QStringLiteral("按扩展名统计磁盘占用。"), QStringLiteral("只读"));
    }
}

void MainWindow::populateFolderUsageTreemap(const QVector<FileUsageEntry>& entries, qint64 totalBytes, int totalFileCount) {
    folderUsageMapScene_->clear();
    folderUsageMapScene_->setSceneRect(0, 0, 1280, 340);
    QVector<FileUsageEntry> visible;
    for (const FileUsageEntry& entry : entries) {
        if (entry.sizeBytes > 0) {
            visible.push_back(entry);
        }
    }
    std::sort(visible.begin(), visible.end(), [](const FileUsageEntry& left, const FileUsageEntry& right) {
        return left.sizeBytes > right.sizeBytes;
    });
    if (visible.size() > 2399) {
        visible.resize(2399);
    }
    qint64 visibleBytes = 0;
    int visibleFiles = 0;
    for (const FileUsageEntry& entry : visible) {
        visibleBytes += entry.sizeBytes;
        visibleFiles += qMax(1, entry.fileCount);
    }
    if (totalBytes > visibleBytes) {
        FileUsageEntry remainder;
        remainder.path = QStringLiteral("其他文件");
        remainder.extension = QStringLiteral("(其他)");
        remainder.sizeBytes = totalBytes - visibleBytes;
        remainder.fileCount = qMax(1, totalFileCount - visibleFiles);
        visible.push_back(remainder);
    }
    if (visible.isEmpty()) {
        auto* text = folderUsageMapScene_->addText(QStringLiteral("没有扫描到可显示的文件。"));
        text->setDefaultTextColor(Qt::white);
    } else {
        drawTreemap(folderUsageMapScene_, visible, 0, visible.size() - 1, folderUsageMapScene_->sceneRect());
    }
    folderUsageMapView_->fitInView(folderUsageMapScene_->sceneRect(), Qt::KeepAspectRatio);
}

void MainWindow::refreshMigrationFolders() {
    if (!migrationTable_ || !migrationWatcher_ || migrationWatcher_->isRunning()) {
        return;
    }
    if (fileStatusLabel_) {
        fileStatusLabel_->setText(QStringLiteral("正在读取系统目录迁移状态..."));
    }
    migrationWatcher_->setFuture(QtConcurrent::run([] {
        return FileManagementEngine().scanMigrationFolders();
    }));
}

void MainWindow::finishMigrationRefresh() {
    populateMigrationFolders(migrationWatcher_->result());
    if (fileStatusLabel_) {
        fileStatusLabel_->setText(QStringLiteral("系统目录迁移状态已刷新。"));
    }
}

void MainWindow::populateMigrationFolders(const QVector<MigrationFolder>& folders) {
    migrationTable_->setRowCount(0);
    for (const MigrationFolder& folder : folders) {
        const int row = migrationTable_->rowCount();
        migrationTable_->insertRow(row);
        auto* check = new QTableWidgetItem();
        const bool personal = QSet<QString>{QStringLiteral("desktop"), QStringLiteral("documents"), QStringLiteral("downloads"), QStringLiteral("pictures"), QStringLiteral("videos")}.contains(folder.key);
        check->setCheckState(personal && folder.exists && !folder.migrated ? Qt::Checked : Qt::Unchecked);
        check->setData(Qt::UserRole, folder.key);
        migrationTable_->setItem(row, 0, check);
        migrationTable_->setItem(row, 1, textItem(folder.name));
        const QString risk = folder.key == QStringLiteral("temp") ? QStringLiteral("低风险") : QStringLiteral("谨慎");
        migrationTable_->setItem(row, 2, textItem(risk));
        migrationTable_->setItem(row, 3, textItem(folder.migrated ? QStringLiteral("已迁移") : (folder.exists ? QStringLiteral("未迁移") : QStringLiteral("不存在"))));
        migrationTable_->setItem(row, 4, textItem(CleanupEngine::formatSize(folder.sizeBytes)));
        migrationTable_->setItem(row, 5, textItem(QDir::toNativeSeparators(folder.path)));
        migrationTable_->setItem(row, 6, textItem(QDir::toNativeSeparators(folder.target)));
        setTableRowMetadata(migrationTable_, row, QStringLiteral("迁移系统目录并更新注册表、环境变量或目录连接；还原时移回原路径。"), risk, folder.path, QStringLiteral("mklink /J + 更新系统目录注册表或 TEMP/TMP 环境变量"));
    }
}

void MainWindow::migrateSelectedFolders() {
    const QString targetRoot = migrationTargetEdit_->text().trimmed();
    if (targetRoot.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("系统目录迁移"), QStringLiteral("请先选择目标目录。"));
        return;
    }
    QStringList keys;
    for (int row = 0; row < migrationTable_->rowCount(); ++row) {
        QTableWidgetItem* check = migrationTable_->item(row, 0);
        if (check && check->checkState() == Qt::Checked) {
            keys.push_back(check->data(Qt::UserRole).toString());
        }
    }
    if (keys.isEmpty() || !confirmAction(this, QStringLiteral("系统目录迁移"), QStringLiteral("请关闭所有软件。将迁移 %1 个目录到:\n%2\n\n是否继续？").arg(keys.size()).arg(targetRoot))) {
        return;
    }
    int success = 0;
    QStringList errors;
    for (const QString& key : keys) {
        const FileOperationResult result = fileEngine_.migratePersonalFolder(key, targetRoot, true);
        if (result.errors.isEmpty()) {
            ++success;
        } else {
            errors.append(result.errors);
        }
    }
    refreshMigrationFolders();
    showOperationLog(QStringLiteral("系统目录迁移 %1 项，失败 %2 项").arg(success).arg(errors.size()));
    QMessageBox::information(this, QStringLiteral("系统目录迁移"), QStringLiteral("成功 %1 项，失败 %2 项。\n%3").arg(success).arg(errors.size()).arg(errors.join(QStringLiteral("\n"))));
}

void MainWindow::restoreSelectedFolders() {
    QStringList keys;
    for (int row = 0; row < migrationTable_->rowCount(); ++row) {
        QTableWidgetItem* check = migrationTable_->item(row, 0);
        if (check && check->checkState() == Qt::Checked) {
            keys.push_back(check->data(Qt::UserRole).toString());
        }
    }
    if (keys.isEmpty() || !confirmAction(this, QStringLiteral("还原目录"), QStringLiteral("将还原 %1 个迁移目录到 C 盘默认路径。是否继续？").arg(keys.size()))) {
        return;
    }
    int success = 0;
    QStringList errors;
    for (const QString& key : keys) {
        const FileOperationResult result = fileEngine_.restorePersonalFolder(key);
        if (result.errors.isEmpty()) {
            ++success;
        } else {
            errors.append(result.errors);
        }
    }
    refreshMigrationFolders();
    showOperationLog(QStringLiteral("还原迁移目录 %1 项，失败 %2 项").arg(success).arg(errors.size()));
    QMessageBox::information(this, QStringLiteral("还原目录"), QStringLiteral("成功 %1 项，失败 %2 项。\n%3").arg(success).arg(errors.size()).arg(errors.join(QStringLiteral("\n"))));
}

void MainWindow::restoreAllFolders() {
    if (!confirmAction(this, QStringLiteral("还原所有迁移目录"), QStringLiteral("将所有已迁移目录还原到 C 盘默认路径。请确保 C 盘空间充足。是否继续？"))) {
        return;
    }
    int success = 0;
    QStringList errors;
    for (const MigrationFolder& folder : fileEngine_.scanMigrationFolders()) {
        if (!folder.migrated) {
            continue;
        }
        const FileOperationResult result = fileEngine_.restorePersonalFolder(folder.key);
        if (result.errors.isEmpty()) {
            ++success;
        } else {
            errors.append(result.errors);
        }
    }
    refreshMigrationFolders();
    showOperationLog(QStringLiteral("还原所有迁移目录 %1 项，失败 %2 项").arg(success).arg(errors.size()));
    QMessageBox::information(this, QStringLiteral("还原所有迁移目录"), QStringLiteral("成功 %1 项，失败 %2 项。\n%3").arg(success).arg(errors.size()).arg(errors.join(QStringLiteral("\n"))));
}

void MainWindow::populateRepairTable(bool deep) {
    if (!repairTable_) {
        return;
    }
    const QVector<RepairItem> items = SystemCatalog::repairActions();
    repairTable_->setRowCount(0);
    for (int i = 0; i < items.size(); ++i) {
        const RepairItem& item = items.at(i);
        const int row = repairTable_->rowCount();
        repairTable_->insertRow(row);
        auto* check = new QTableWidgetItem();
        check->setCheckState((deep || item.recommended) ? Qt::Checked : Qt::Unchecked);
        check->setData(Qt::UserRole, i);
        repairTable_->setItem(row, 0, check);
        repairTable_->setItem(row, 1, textItem(item.title));
        repairTable_->setItem(row, 2, textItem(item.risk));
        repairTable_->setItem(row, 3, textItem(item.description));
        repairTable_->setItem(row, 4, textItem(item.command));
        auto* run = secondaryButton(QStringLiteral("执行"));
        connect(run, &QPushButton::clicked, this, [this, item] { runRepairCommand(item); });
        repairTable_->setCellWidget(row, 5, run);
        setTableRowMetadata(repairTable_, row, item.description, item.risk, {}, item.command);
        if (item.deep) {
            for (int column = 0; column < 5; ++column) {
                repairTable_->item(row, column)->setBackground(QBrush(QColor(QStringLiteral("#fff3cd"))));
            }
        }
    }
}

void MainWindow::updateRepairMode(bool deep) {
    if (deep && deepRepairMode_->isChecked() && !confirmAction(
            this,
            QStringLiteral("深度系统修复风险确认"),
            QStringLiteral("深度修复包含 DISM、CHKDSK /F /R 和系统更新组件重置，耗时较长且可能需要重启。是否继续选择？")
        )) {
        deep = false;
    }
    recommendedRepairMode_->setChecked(!deep);
    deepRepairMode_->setChecked(deep);
    populateRepairTable(deep);
}

void MainWindow::runRepairCommand(const RepairItem& item) {
    if (item.deep && !confirmAction(this, QStringLiteral("执行深度修复"), QStringLiteral("%1\n\n%2\n命令: %3").arg(item.title, item.description, item.command))) {
        return;
    }
    repairLog_->append(QStringLiteral("[%1]").arg(item.title));
    globalStatusLabel_->setText(QStringLiteral("正在执行修复: %1").arg(item.title));
    auto* watcher = new QFutureWatcher<CommandBatchResult>(this);
    connect(watcher, &QFutureWatcher<CommandBatchResult>::finished, this, [this, watcher, item] {
        const CommandBatchResult result = watcher->result();
        const int exitCode = result.exitCodes.value(item.id, -1);
        repairLog_->append(result.output);
        showOperationLog(QStringLiteral("执行修复 %1，退出码 %2").arg(item.title).arg(exitCode));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([item] {
        CommandBatchResult result;
        int exitCode = 0;
        result.output = SystemCatalog::runCommand(item.command, &exitCode);
        result.exitCodes.insert(item.id, exitCode);
        return result;
    }));
}

void MainWindow::runSelectedRepairs() {
    QVector<RepairItem> selected;
    const QVector<RepairItem> items = SystemCatalog::repairActions();
    bool hasDeep = false;
    for (int row = 0; row < repairTable_->rowCount(); ++row) {
        QTableWidgetItem* check = repairTable_->item(row, 0);
        if (check && check->checkState() == Qt::Checked) {
            const int index = check->data(Qt::UserRole).toInt();
            if (index >= 0 && index < items.size()) {
                selected.push_back(items.at(index));
                hasDeep = hasDeep || items.at(index).deep;
            }
        }
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("系统修复"), QStringLiteral("请先勾选修复项。"));
        return;
    }
    if (hasDeep && !confirmAction(this, QStringLiteral("深度修复确认"), QStringLiteral("选中项包含深度修复，可能耗时较长或需要重启。是否执行？"))) {
        return;
    }
    globalStatusLabel_->setText(QStringLiteral("正在执行 %1 个修复项...").arg(selected.size()));
    auto* watcher = new QFutureWatcher<CommandBatchResult>(this);
    connect(watcher, &QFutureWatcher<CommandBatchResult>::finished, this, [this, watcher, selected] {
        const CommandBatchResult result = watcher->result();
        repairLog_->append(result.output);
        for (const RepairItem& item : selected) {
            showOperationLog(QStringLiteral("执行修复 %1，退出码 %2")
                .arg(item.title)
                .arg(result.exitCodes.value(item.id, -1)));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([selected] {
        CommandBatchResult result;
        QStringList outputs;
        for (const RepairItem& item : selected) {
            int exitCode = 0;
            outputs.push_back(QStringLiteral("[%1]\n%2")
                .arg(item.title, SystemCatalog::runCommand(item.command, &exitCode)));
            result.exitCodes.insert(item.id, exitCode);
        }
        result.output = outputs.join(QStringLiteral("\n\n"));
        return result;
    }));
}

void MainWindow::runGlobalRestore() {
    if (!confirmAction(
            this,
            QStringLiteral("全局一键还原"),
            QStringLiteral("将还原本工具可逆的系统优化、显卡设置、系统目录迁移和清理备份。卸载的软件与粉碎文件无法自动恢复。是否继续？")
        )) {
        return;
    }
    QStringList output;
    QSet<QString> restoredActions;
    const QSet<QString> appliedOptimizations = appliedActionIds(OptimizationStateKey);
    const QSet<QString> appliedGpuActions = appliedActionIds(GpuStateKey);
    const QVector<QVector<WindowsOptimizationAction>> groups = {
        SystemCatalog::officeOptimizationActions(),
        SystemCatalog::gamingOptimizationActions(),
        SystemCatalog::advancedControlActions(),
    };
    for (const QVector<WindowsOptimizationAction>& actions : groups) {
        for (const WindowsOptimizationAction& action : actions) {
            if (action.revertCommands.isEmpty()
                || restoredActions.contains(action.id)
                || !appliedOptimizations.contains(action.id)) {
                continue;
            }
            restoredActions.insert(action.id);
            int exitCode = 0;
            output.push_back(QStringLiteral("[%1]\n%2").arg(action.title, SystemCatalog::runActionCommands(action.revertCommands, &exitCode)));
            if (exitCode == 0) {
                setActionApplied(OptimizationStateKey, action.id, false);
            }
        }
    }
    for (const GpuOptimizationAction& action : gpuActions_) {
        if (action.revertCommands.isEmpty() || !appliedGpuActions.contains(action.id)) {
            continue;
        }
        int exitCode = 0;
        output.push_back(QStringLiteral("[%1]\n%2").arg(action.title, gpuEngine_.restoreAction(action, &exitCode)));
        if (exitCode == 0) {
            setActionApplied(GpuStateKey, action.id, false);
        }
    }
    for (const MigrationFolder& folder : fileEngine_.scanMigrationFolders()) {
        if (folder.migrated) {
            const FileOperationResult result = fileEngine_.restorePersonalFolder(folder.key);
            output.push_back(QStringLiteral("[目录还原] %1: %2").arg(folder.name, result.errors.isEmpty() ? QStringLiteral("成功") : result.errors.join(QStringLiteral("; "))));
        }
    }
    int backupRestored = 0;
    QStringList backupErrors;
    for (const BackupRecord& record : CleanupEngine::backupInfo(backupRoot_).backups) {
        QString error;
        if (CleanupEngine::restoreBackupItem(record, &error)) {
            ++backupRestored;
        } else {
            backupErrors.push_back(error);
        }
    }
    output.push_back(QStringLiteral("[清理备份] 恢复 %1 个，失败 %2 个").arg(backupRestored).arg(backupErrors.size()));
    output.append(backupErrors);
    refreshMigrationFolders();
    showOperationLog(QStringLiteral("全局一键还原完成"));
    QMessageBox::information(this, QStringLiteral("全局一键还原"), output.join(QStringLiteral("\n\n")).left(16000));
}
