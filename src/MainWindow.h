#pragma once

#include "CleanupEngine.h"
#include "FileManagementEngine.h"
#include "GpuOptimizationEngine.h"
#include "SoftwareUninstallEngine.h"
#include "SystemCatalog.h"

#include <QCheckBox>
#include <QFutureWatcher>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <functional>

class QComboBox;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    QWidget* createTopNavigation();
    QWidget* createBottomBar();
    QWidget* createCleanPage();
    QWidget* createUninstallPage();
    QWidget* createOptimizePage();
    QWidget* createFilePage();
    QWidget* createRepairPage();
    QWidget* createGpuPanel();
    QWidget* scrollablePage(QWidget* page);

    void applyStyle();
    void selectPage(int index);
    QPushButton* navigationButton(const QString& text, int index);
    QPushButton* primaryButton(const QString& text) const;
    QPushButton* secondaryButton(const QString& text) const;
    QWidget* pageHeader(const QString& title, const QString& subtitle) const;
    void showOperationLog(const QString& message);
    void showOperationLogDialog();
    void runGlobalRestore();
    bool isWhitelisted(const QString& path) const;
    void addToWhitelist(const QString& path);
    void installTableContextMenu(QTableWidget* table, int pathColumn, int commandColumn = -1);
    void installTreeContextMenu(QTreeWidget* tree, int pathColumn, int commandColumn = -1);
    void setTableRowMetadata(QTableWidget* table, int row, const QString& description, const QString& risk, const QString& path = {}, const QString& command = {}, const QString& actionId = {});
    void runContextAction(const QString& actionId);
    void runShellCommandAsync(const QString& title, const QString& command);

    void refreshDiskInfo();
    void startScan();
    void finishScan();
    void populateCleanupTree();
    void updateCleanMode(bool deep);
    CleanupEngine::CleanMode currentCleanMode() const;
    QVector<CleanupEntry> selectedCleanupEntries() const;
    void cleanEntries(const QVector<CleanupEntry>& entries);
    void cleanSelected();
    void openBackupManager();

    void refreshInstalledApps();
    void populateUninstallTable();
    void filterInstalledApps(const QString& text);
    void uninstallApplication(int appIndex, bool strong);
    void uninstallSelectedApplications();
    void cleanApplicationResiduals(const InstalledApplication& app);
    void cleanApplicationResidualsBatch(const QVector<InstalledApplication>& applications);

    QTableWidget* createOptimizationTable(QWidget* parent);
    void populateOptimizationTable(QTableWidget* table, const QVector<WindowsOptimizationAction>& actions);
    void runOptimizationAction(const WindowsOptimizationAction& action, bool revert);
    void applyOptimizationPreset(QTableWidget* table, const QVector<WindowsOptimizationAction>& actions, const QString& title, bool deep);
    void refreshGpuInfo();
    void finishGpuRefresh();
    void populateGpuActions();
    void runGpuAction(int actionIndex, bool revert);

    void refreshDisks();
    void selectDiskRoot(const QString& rootPath);
    void scanFolderUsage();
    void scanManagedFiles();
    void populateManagedFiles(const QVector<ManagedFileEntry>& files);
    void runFileOperationAsync(
        const QString& title,
        const std::function<FileOperationResult()>& operation,
        const std::function<void()>& completed = {}
    );
    QStringList selectedManagedPaths() const;
    void copySelectedFiles();
    void moveSelectedFiles();
    void renameSelectedFile();
    void deleteSelectedFiles();
    void shredSelectedFiles();
    void repairSelectedFolderPermission();
    void migrateSelectedFiles();
    void populateFolderUsage(const FolderUsageScan& scan);
    void populateExtensionUsageTable(const QVector<ExtensionUsageEntry>& entries, qint64 totalBytes);
    void populateFolderUsageTreemap(const QVector<FileUsageEntry>& entries, qint64 totalBytes, int totalFileCount);
    void refreshMigrationFolders();
    void finishMigrationRefresh();
    void populateMigrationFolders(const QVector<MigrationFolder>& folders);
    void migrateSelectedFolders();
    void restoreSelectedFolders();
    void restoreAllFolders();

    void populateRepairTable(bool deep);
    void updateRepairMode(bool deep);
    void runRepairCommand(const RepairItem& item);
    void runSelectedRepairs();

    CleanupEngine cleanupEngine_;
    FileManagementEngine fileEngine_;
    GpuOptimizationEngine gpuEngine_;
    SoftwareUninstallEngine uninstallEngine_;

    QStackedWidget* pages_ = nullptr;
    QVector<QPushButton*> navButtons_;
    QLabel* globalStatusLabel_ = nullptr;
    QLabel* privilegeStatusLabel_ = nullptr;

    QLabel* totalSpaceLabel_ = nullptr;
    QLabel* usedSpaceLabel_ = nullptr;
    QLabel* freeSpaceLabel_ = nullptr;
    QLabel* reclaimSpaceLabel_ = nullptr;
    QLabel* cleanStatusLabel_ = nullptr;
    QLabel* currentScanPath_ = nullptr;
    QProgressBar* scanProgress_ = nullptr;
    QTreeWidget* cleanupTree_ = nullptr;
    QCheckBox* recommendedMode_ = nullptr;
    QCheckBox* deepMode_ = nullptr;
    QVector<CleanupEntry> cleanupEntries_;
    QString backupRoot_;
    qint64 cleanFreeBytesBefore_ = -1;
    QFutureWatcher<CleanupScanResult>* scanWatcher_ = nullptr;
    QFutureWatcher<CleanResult>* cleanWatcher_ = nullptr;

    QLineEdit* uninstallSearchEdit_ = nullptr;
    QTableWidget* uninstallTable_ = nullptr;
    QVector<InstalledApplication> installedApps_;
    QFutureWatcher<QVector<InstalledApplication>>* uninstallWatcher_ = nullptr;

    QTabWidget* optimizationTabs_ = nullptr;
    QTableWidget* officeOptimizationTable_ = nullptr;
    QTableWidget* gamingOptimizationTable_ = nullptr;
    QTableWidget* advancedControlTable_ = nullptr;
    QTableWidget* gpuInfoTable_ = nullptr;
    QTableWidget* gpuActionTable_ = nullptr;
    QTextEdit* gpuLog_ = nullptr;
    int gpuTabIndex_ = -1;
    QVector<GpuDeviceInfo> gpuDevices_;
    QVector<GpuOptimizationAction> gpuActions_;
    QFutureWatcher<QVector<GpuDeviceInfo>>* gpuWatcher_ = nullptr;

    QComboBox* diskCombo_ = nullptr;
    QComboBox* fileTypeCombo_ = nullptr;
    QString fileRoot_;
    QLabel* fileDiskInfoLabel_ = nullptr;
    QLabel* fileStatusLabel_ = nullptr;
    QTabWidget* fileTabs_ = nullptr;
    QWidget* folderUsagePage_ = nullptr;
    QTreeWidget* folderUsageTree_ = nullptr;
    QTableWidget* folderExtensionTable_ = nullptr;
    QGraphicsScene* folderUsageMapScene_ = nullptr;
    QGraphicsView* folderUsageMapView_ = nullptr;
    QTableWidget* managedFileTable_ = nullptr;
    QTableWidget* migrationTable_ = nullptr;
    QLineEdit* migrationTargetEdit_ = nullptr;
    QVector<ManagedFileEntry> managedFiles_;
    QFutureWatcher<FolderUsageScan>* folderUsageWatcher_ = nullptr;
    QFutureWatcher<QVector<ManagedFileEntry>>* managedFileWatcher_ = nullptr;
    QFutureWatcher<QVector<MigrationFolder>>* migrationWatcher_ = nullptr;
    bool fileOperationRunning_ = false;

    QCheckBox* recommendedRepairMode_ = nullptr;
    QCheckBox* deepRepairMode_ = nullptr;
    QTableWidget* repairTable_ = nullptr;
    QTextEdit* repairLog_ = nullptr;
};
