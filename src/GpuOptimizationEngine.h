#pragma once

#include "SystemCatalog.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct GpuDeviceInfo {
    QString name;
    QString vendor;
    QString driverVersion;
    qint64 memoryMB = -1;
    int temperatureC = -1;
    int loadPercent = -1;
    bool nvidiaSmiAvailable = false;
    bool nvapiAvailable = false;
    bool adlxAvailable = false;
    bool amdSoftwareAvailable = false;
    QStringList capabilities;
};

struct GpuOptimizationAction {
    QString id;
    QString vendor;
    QString title;
    QString description;
    QString riskLabel;
    QVector<WindowsOptimizationCommand> commands;
    QVector<WindowsOptimizationCommand> revertCommands;
    bool supported = false;
    bool modifiesSystem = false;
    bool requiresConfirmation = true;
};

class GpuOptimizationEngine {
public:
    QVector<GpuDeviceInfo> detectDevices() const;
    QVector<GpuOptimizationAction> supportedActions(const QVector<GpuDeviceInfo>& devices) const;

    QString runAction(const GpuOptimizationAction& action, int* exitCode = nullptr) const;
    QString restoreAction(const GpuOptimizationAction& action, int* exitCode = nullptr) const;

private:
    QVector<GpuDeviceInfo> queryVideoControllers() const;
    QVector<GpuDeviceInfo> queryNvidiaSmi() const;
    QString nvidiaSmiPath() const;
    bool nvapiAvailable() const;
    bool adlxAvailable() const;
    bool amdSoftwareAvailable() const;
};
