#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct RepairItem {
    QString id;
    QString title;
    QString risk;
    QString description;
    QString command;
    bool recommended = true;
    bool deep = false;
};

struct WindowsOptimizationCommand {
    QString executable;
    QStringList arguments;
    QString description;
};

struct WindowsOptimizationAction {
    QString id;
    QString title;
    QString description;
    QString category;
    QString riskLabel;
    QVector<WindowsOptimizationCommand> commands;
    QVector<WindowsOptimizationCommand> revertCommands;
    bool requiresAdmin = false;
    bool recommended = true;
};

class SystemCatalog {
public:
    static QVector<WindowsOptimizationAction> officeOptimizationActions();
    static QVector<WindowsOptimizationAction> gamingOptimizationActions();
    static QVector<WindowsOptimizationAction> advancedControlActions();
    static QVector<RepairItem> repairActions();

    static QString runCommand(const QString& command, int* exitCode = nullptr);
    static QString runActionCommands(
        const QVector<WindowsOptimizationCommand>& commands,
        int* exitCode = nullptr
    );
    static QString runOptimizationAction(
        const WindowsOptimizationAction& action,
        bool revert,
        int* exitCode = nullptr
    );
};
