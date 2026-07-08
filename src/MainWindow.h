#pragma once

#include "AccountStore.h"
#include "CleanupEngine.h"
#include "FileManagementEngine.h"
#include "GpuOptimizationEngine.h"
#include "SystemCatalog.h"

#include <QCheckBox>
#include <QFutureWatcher>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTreeWidget>

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    enum class CleanModule {
        CDrive = 0,
        QQ = 1,
        WeChat = 2,
    };

    QWidget* createSidebar();
    QWidget* createCleanPage();
    QWidget* createOptimizePage();
    QWidget* createGpuPage();
    QWidget* createBxPage();
    QWidget* createUninstallPage();
    QWidget* createFilePage();
    QWidget* createRepairPage();
    QWidget* createAccountPage();

    void applyStyle();
    void selectPage(int index);
    void selectCleanModule(CleanModule module);
    QPushButton* sidebarButton(const QString& text, int index);
    QPushButton* cleanSidebarButton(const QString& text, CleanModule module);
    QPushButton* primaryButton(const QString& text) const;
    QPushButton* secondaryButton(const QString& text) const;
    void refreshDiskInfo();
    QWidget* pageHeader(const QString& title, const QString& subtitle) const;
    void showOperationLog(const QString& message);

    void startScan();
    void finishScan();
    void populateCleanupTree();
    void updateCleanModuleHeader();
    void updateReclaimSpaceForCurrentCleanModule();
    CleanupEngine::ScanScope currentScanScope() const;
    QVector<CleanupEntry> entriesForCurrentCleanModule(CleanupEngine::CleanMode mode) const;
    bool cleanupEntryMatchesCurrentModule(const CleanupEntry& entry) const;
    void updateModeSelection(QCheckBox* changed);
    CleanupEngine::CleanMode currentCleanMode() const;
    QVector<CleanupEntry> selectedCleanupEntries() const;
    bool allowScanOnly() const;
    void cleanSelected();
    void cleanAllForCurrentMode();
    void openBackupManager();

    void populateStartupItems();
    void populateMemoryItems();
    void populateSystemOptimizationItems();
    void populatePrivacyItems();
    void populateRegistryItems();
    void populateNvidiaItems();
    void populateAmdItems();
    void populateMaintenanceItems();
    void populateEdgeToolkitItems();
    void populateOptimizerTable(QTreeWidget* table, const QVector<OptimizerItem>& items);
    void runOptimizerAction(const OptimizerItem& item);
    void applyCurrentOptimizationTab();
    void populateWindowsOptimizationActions();
    void runWindowsOptimizationAction(int row, bool revert);
    void runAdBlockAction(bool enable);
    void runGlobalRestore();

    void refreshGpuInfo();
    void populateGpuActions();
    void runGpuAction(int row, bool revert);

    void populateBxItems();
    void applyBxMode(const QString& mode);
    void applyBxOptimization();

    void refreshInstalledApps();
    void populateUninstallTable(QTableWidget* table, const QVector<QJsonObject>& apps, const QString& actionLabel);
    void runUninstallCommand(const QString& command);

    void chooseFileRoot();
    void scanFolderUsage();
    void scanLargeFilesAsync();
    void scanDuplicateFilesAsync();
    void scanEmptyFolders();
    void refreshMigrationFolders();
    void migrateSelectedFolders();
    void restoreSelectedFolders();
    void populateLargeFiles(const QVector<FileEntry>& files);
    void populateDuplicateFiles(const QVector<QVector<FileEntry>>& groups);
    void populateFolderUsage(const FolderUsageScan& scan);
    void populateExtensionUsageTable(const QVector<ExtensionUsageEntry>& entries, qint64 totalBytes);
    void populateFolderUsageTreemap(const QVector<FileUsageEntry>& entries, qint64 totalBytes, int totalFileCount);
    void populateEmptyFolders(const QVector<EmptyFolderEntry>& folders);
    void populateMigrationFolders(const QVector<MigrationFolder>& folders);
    void deleteSelectedFileItems();
    void shredSelectedFileItems();
    void scanFragments();
    void optimizeFragments();

    void populateRepairTable();
    void runRepairCommand(const RepairItem& item);
    void runSelectedRepairs();

    void refreshAccountState();
    void registerAccount();
    void loginAccount();
    void redeemCard();
    void logoutAccount();

    CleanupEngine cleanupEngine_;
    FileManagementEngine fileEngine_;
    GpuOptimizationEngine gpuEngine_;
    AccountStore accountStore_;
    QString backupRoot_;
    CleanModule cleanModule_ = CleanModule::CDrive;
    QStackedWidget* pages_ = nullptr;
    QVector<QPushButton*> navButtons_;

    QLabel* totalSpaceLabel_ = nullptr;
    QLabel* usedSpaceLabel_ = nullptr;
    QLabel* freeSpaceLabel_ = nullptr;
    QLabel* reclaimSpaceLabel_ = nullptr;
    QLabel* cleanTitleLabel_ = nullptr;
    QLabel* cleanSubtitleLabel_ = nullptr;
    QLabel* cleanStatusLabel_ = nullptr;
    QLabel* currentScanPath = nullptr;
    QProgressBar* scanProgress_ = nullptr;
    QTreeWidget* cleanupTree_ = nullptr;
    QCheckBox* recommendedMode = nullptr;
    QCheckBox* professionalMode = nullptr;
    QCheckBox* selectAllMode = nullptr;
    QCheckBox* simulateMode_ = nullptr;
    QCheckBox* backupMode_ = nullptr;
    QVector<CleanupEntry> cleanupEntries_;
    QFutureWatcher<CleanupScanResult>* scanWatcher_ = nullptr;

    QTabWidget* optimizerTabs_ = nullptr;
    QMap<QString, QTreeWidget*> optimizerTables_;
    QTableWidget* windowsOptimizationTable_ = nullptr;
    QTableWidget* gpuInfoTable_ = nullptr;
    QTableWidget* gpuActionTable_ = nullptr;
    QTextEdit* gpuLog_ = nullptr;
    QVector<GpuDeviceInfo> gpuDevices_;
    QVector<GpuOptimizationAction> gpuActions_;
    QTableWidget* bxTable_ = nullptr;
    QLabel* bxStatusLabel_ = nullptr;
    QString bxMode_ = QStringLiteral("basic");

    QTabWidget* uninstallTabs_ = nullptr;
    QTableWidget* uninstallTable_ = nullptr;
    QTableWidget* storeUninstallTable_ = nullptr;
    QTabWidget* fileTabs_ = nullptr;
    QString fileRoot_;
    QLabel* fileRootLabel_ = nullptr;
    QWidget* folderUsagePage_ = nullptr;
    QTreeWidget* folderUsageTree_ = nullptr;
    QTableWidget* folderExtensionTable_ = nullptr;
    QGraphicsScene* folderUsageMapScene_ = nullptr;
    QGraphicsView* folderUsageMapView_ = nullptr;
    QTableWidget* largeFileTable_ = nullptr;
    QTableWidget* duplicateFileTable_ = nullptr;
    QTableWidget* emptyFolderTable_ = nullptr;
    QTableWidget* migrationTable_ = nullptr;
    QLineEdit* migrationTargetEdit_ = nullptr;
    QCheckBox* migrationMoveFiles_ = nullptr;
    QLabel* fileStatusLabel_ = nullptr;
    QTextEdit* repairLog_ = nullptr;
    QTableWidget* repairTable_ = nullptr;
    QTextEdit* operationLog_ = nullptr;

    QLabel* accountStateLabel_ = nullptr;
    QLineEdit* accountEmailEdit_ = nullptr;
    QLineEdit* accountNameEdit_ = nullptr;
    QLineEdit* accountPasswordEdit_ = nullptr;
    QLineEdit* cardCodeEdit_ = nullptr;
};
