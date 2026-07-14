#pragma once

#include <QString>

class AmdAdlxBridge {
public:
    static bool supportsGlobal3DSettings(QString* details = nullptr);
    static QString applyGlobalPerformanceSettings(int* exitCode = nullptr);
    static QString restoreGlobalPerformanceSettings(int* exitCode = nullptr);
};
