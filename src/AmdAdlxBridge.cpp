#include "AmdAdlxBridge.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QStringList>

#ifdef Q_OS_WIN
#include <SDK/ADLXHelper/Windows/Cpp/ADLXHelper.h>
#include <SDK/Include/I3DSettings.h>

namespace {

const QString BackupGroup = QStringLiteral("gpu/amd_adlx_backup");
QMutex bridgeMutex;

class AdlxContext {
public:
    AdlxContext() {
        const ADLX_RESULT initialized = g_ADLX.Initialize();
        if (ADLX_FAILED(initialized)) {
            error = QStringLiteral("ADLX 初始化失败: %1").arg(static_cast<int>(initialized));
            return;
        }
        system = g_ADLX.GetSystemServices();
        ADLX_RESULT result = system ? system->GetGPUs(&gpus) : ADLX_FAIL;
        if (ADLX_SUCCEEDED(result)) {
            result = gpus->At(0, &gpu);
        }
        if (ADLX_SUCCEEDED(result)) {
            result = system->Get3DSettingsServices(&settings);
        }
        if (ADLX_FAILED(result) || !gpu || !settings) {
            error = QStringLiteral("ADLX 无法打开 AMD 3D 设置服务: %1").arg(static_cast<int>(result));
        }
    }

    ~AdlxContext() {
        if (settings) settings->Release();
        if (gpu) gpu->Release();
        if (gpus) gpus->Release();
        g_ADLX.Terminate();
    }

    bool available() const { return error.isEmpty(); }

    adlx::IADLXSystem* system = nullptr;
    adlx::IADLXGPUList* gpus = nullptr;
    adlx::IADLXGPU* gpu = nullptr;
    adlx::IADLX3DSettingsServices* settings = nullptr;
    QString error;
};

}  // namespace
#endif

bool AmdAdlxBridge::supportsGlobal3DSettings(QString* details) {
#ifdef Q_OS_WIN
    QMutexLocker locker(&bridgeMutex);
    AdlxContext context;
    if (!context.available()) {
        if (details) *details = context.error;
        return false;
    }
    bool any = false;
    QStringList capabilities;
    adlx::IADLX3DWaitForVerticalRefresh* vsync = nullptr;
    if (ADLX_SUCCEEDED(context.settings->GetWaitForVerticalRefresh(context.gpu, &vsync)) && vsync) {
        adlx_bool supported = false;
        if (ADLX_SUCCEEDED(vsync->IsSupported(&supported)) && supported) {
            any = true;
            capabilities.push_back(QStringLiteral("垂直同步"));
        }
        vsync->Release();
    }
    adlx::IADLX3DFrameRateTargetControl* frtc = nullptr;
    if (ADLX_SUCCEEDED(context.settings->GetFrameRateTargetControl(context.gpu, &frtc)) && frtc) {
        adlx_bool supported = false;
        if (ADLX_SUCCEEDED(frtc->IsSupported(&supported)) && supported) {
            any = true;
            capabilities.push_back(QStringLiteral("帧率目标控制"));
        }
        frtc->Release();
    }
    if (details) {
        *details = any ? QStringLiteral("ADLX 支持: %1").arg(capabilities.join(QStringLiteral("、"))) : QStringLiteral("当前 AMD 驱动未提供可修改的 ADLX 3D 设置");
    }
    return any;
#else
    if (details) *details = QStringLiteral("仅 Windows 支持 ADLX");
    return false;
#endif
}

QString AmdAdlxBridge::applyGlobalPerformanceSettings(int* exitCode) {
#ifdef Q_OS_WIN
    QMutexLocker locker(&bridgeMutex);
    AdlxContext context;
    if (!context.available()) {
        if (exitCode) *exitCode = -1;
        return context.error;
    }
    QSettings state;
    state.beginGroup(BackupGroup);
    const bool hasBackup = state.value(QStringLiteral("valid"), false).toBool();
    QStringList output;
    bool changed = false;

    adlx::IADLX3DWaitForVerticalRefresh* vsync = nullptr;
    if (ADLX_SUCCEEDED(context.settings->GetWaitForVerticalRefresh(context.gpu, &vsync)) && vsync) {
        adlx_bool supported = false;
        ADLX_WAIT_FOR_VERTICAL_REFRESH_MODE mode = WFVR_ALWAYS_OFF;
        if (ADLX_SUCCEEDED(vsync->IsSupported(&supported)) && supported
            && ADLX_SUCCEEDED(vsync->GetMode(&mode))) {
            if (!hasBackup) state.setValue(QStringLiteral("vsyncMode"), static_cast<int>(mode));
            const ADLX_RESULT result = vsync->SetMode(WFVR_ALWAYS_OFF);
            if (ADLX_FAILED(result)) {
                vsync->Release();
                state.endGroup();
                if (exitCode) *exitCode = static_cast<int>(result);
                return QStringLiteral("ADLX 关闭垂直同步失败: %1").arg(static_cast<int>(result));
            }
            changed = true;
            state.setValue(QStringLiteral("valid"), true);
            state.sync();
            output.push_back(QStringLiteral("已通过 ADLX 关闭垂直同步"));
        }
        vsync->Release();
    }

    adlx::IADLX3DFrameRateTargetControl* frtc = nullptr;
    if (ADLX_SUCCEEDED(context.settings->GetFrameRateTargetControl(context.gpu, &frtc)) && frtc) {
        adlx_bool supported = false;
        adlx_bool enabled = false;
        adlx_int fps = 0;
        if (ADLX_SUCCEEDED(frtc->IsSupported(&supported)) && supported
            && ADLX_SUCCEEDED(frtc->IsEnabled(&enabled))
            && ADLX_SUCCEEDED(frtc->GetFPS(&fps))) {
            if (!hasBackup) {
                state.setValue(QStringLiteral("frtcEnabled"), enabled);
                state.setValue(QStringLiteral("frtcFps"), fps);
            }
            const ADLX_RESULT result = frtc->SetEnabled(false);
            if (ADLX_FAILED(result)) {
                frtc->Release();
                state.endGroup();
                if (exitCode) *exitCode = static_cast<int>(result);
                return QStringLiteral("ADLX 关闭帧率限制失败: %1").arg(static_cast<int>(result));
            }
            changed = true;
            state.setValue(QStringLiteral("valid"), true);
            state.sync();
            output.push_back(QStringLiteral("已通过 ADLX 关闭帧率目标限制"));
        }
        frtc->Release();
    }

    if (!changed) {
        state.endGroup();
        if (exitCode) *exitCode = -1;
        return QStringLiteral("当前 AMD 显卡没有可修改的 ADLX 3D 设置");
    }
    state.setValue(QStringLiteral("valid"), true);
    state.sync();
    state.endGroup();
    if (exitCode) *exitCode = 0;
    return output.join(QStringLiteral("\n"));
#else
    if (exitCode) *exitCode = -1;
    return QStringLiteral("仅 Windows 支持 ADLX");
#endif
}

QString AmdAdlxBridge::restoreGlobalPerformanceSettings(int* exitCode) {
#ifdef Q_OS_WIN
    QMutexLocker locker(&bridgeMutex);
    QSettings state;
    state.beginGroup(BackupGroup);
    if (!state.value(QStringLiteral("valid"), false).toBool()) {
        state.endGroup();
        if (exitCode) *exitCode = 0;
        return QStringLiteral("AMD ADLX 设置已处于还原状态。");
    }
    AdlxContext context;
    if (!context.available()) {
        state.endGroup();
        if (exitCode) *exitCode = -1;
        return context.error;
    }
    QStringList output;
    if (state.contains(QStringLiteral("vsyncMode"))) {
        adlx::IADLX3DWaitForVerticalRefresh* vsync = nullptr;
        if (ADLX_FAILED(context.settings->GetWaitForVerticalRefresh(context.gpu, &vsync)) || !vsync) {
            state.endGroup();
            if (exitCode) *exitCode = -1;
            return QStringLiteral("无法打开 ADLX 垂直同步还原接口");
        }
        const ADLX_RESULT result = vsync->SetMode(static_cast<ADLX_WAIT_FOR_VERTICAL_REFRESH_MODE>(state.value(QStringLiteral("vsyncMode")).toInt()));
        vsync->Release();
        if (ADLX_FAILED(result)) {
            state.endGroup();
            if (exitCode) *exitCode = static_cast<int>(result);
            return QStringLiteral("ADLX 垂直同步还原失败: %1").arg(static_cast<int>(result));
        }
        output.push_back(QStringLiteral("已恢复原垂直同步模式"));
    }
    if (state.contains(QStringLiteral("frtcEnabled"))) {
        adlx::IADLX3DFrameRateTargetControl* frtc = nullptr;
        if (ADLX_FAILED(context.settings->GetFrameRateTargetControl(context.gpu, &frtc)) || !frtc) {
            state.endGroup();
            if (exitCode) *exitCode = -1;
            return QStringLiteral("无法打开 ADLX 帧率限制还原接口");
        }
        const bool enabled = state.value(QStringLiteral("frtcEnabled")).toBool();
        ADLX_RESULT result = frtc->SetEnabled(enabled);
        if (ADLX_SUCCEEDED(result) && enabled) {
            result = frtc->SetFPS(state.value(QStringLiteral("frtcFps")).toInt());
        }
        frtc->Release();
        if (ADLX_FAILED(result)) {
            state.endGroup();
            if (exitCode) *exitCode = static_cast<int>(result);
            return QStringLiteral("ADLX 帧率限制还原失败: %1").arg(static_cast<int>(result));
        }
        output.push_back(QStringLiteral("已恢复原帧率目标设置"));
    }
    state.remove(QString());
    state.endGroup();
    if (exitCode) *exitCode = 0;
    return output.join(QStringLiteral("\n"));
#else
    if (exitCode) *exitCode = -1;
    return QStringLiteral("仅 Windows 支持 ADLX");
#endif
}
