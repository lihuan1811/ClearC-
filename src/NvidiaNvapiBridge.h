#pragma once

#include <QString>

class NvidiaNvapiBridge {
public:
    static bool supportsGlobalProfileSettings(QString* details = nullptr);
    static QString applyGlobalPerformanceSettings(int* exitCode = nullptr);
    static QString restoreGlobalPerformanceSettings(int* exitCode = nullptr);
};
