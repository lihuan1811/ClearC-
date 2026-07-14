#include "NvidiaNvapiBridge.h"

#include <QSettings>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <NvApiDriverSettings.h>
#include <nvapi.h>

#include <array>
#include <cstring>

namespace {

constexpr NvU32 InitializeId = 0x0150e828;
constexpr NvU32 UnloadId = 0xd22bdd7e;
constexpr NvU32 CreateSessionId = 0x0694d52e;
constexpr NvU32 DestroySessionId = 0xdad9cff8;
constexpr NvU32 LoadSettingsId = 0x375dbd6b;
constexpr NvU32 SaveSettingsId = 0xfcbc7e14;
constexpr NvU32 SetSettingId = 0x577dd202;
constexpr NvU32 GetSettingId = 0x73bf8338;
constexpr NvU32 EnumSettingIdsId = 0xf020614a;
constexpr NvU32 RestoreDefaultSettingId = 0x53f0381e;
constexpr NvU32 GetBaseProfileId = 0xda8466a0;
const QString BackupGroup = QStringLiteral("gpu/nvidia_nvapi_backup");

class NvapiRuntime {
public:
    NvapiRuntime() {
#if defined(_WIN64)
        module_ = LoadLibraryW(L"nvapi64.dll");
#else
        module_ = LoadLibraryW(L"nvapi.dll");
#endif
        if (!module_) {
            error_ = QStringLiteral("未找到 NVIDIA NVAPI 运行库");
            return;
        }
        query_ = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(module_, "nvapi_QueryInterface"));
        if (!query_) {
            error_ = QStringLiteral("NVAPI 查询接口不可用");
            return;
        }
        initialize = resolve<decltype(&NvAPI_Initialize)>(InitializeId);
        unload = resolve<decltype(&NvAPI_Unload)>(UnloadId);
        createSession = resolve<decltype(&NvAPI_DRS_CreateSession)>(CreateSessionId);
        destroySession = resolve<decltype(&NvAPI_DRS_DestroySession)>(DestroySessionId);
        loadSettings = resolve<decltype(&NvAPI_DRS_LoadSettings)>(LoadSettingsId);
        saveSettings = resolve<decltype(&NvAPI_DRS_SaveSettings)>(SaveSettingsId);
        setSetting = resolve<decltype(&NvAPI_DRS_SetSetting)>(SetSettingId);
        getSetting = resolve<decltype(&NvAPI_DRS_GetSetting)>(GetSettingId);
        enumSettingIds = resolve<decltype(&NvAPI_DRS_EnumAvailableSettingIds)>(EnumSettingIdsId);
        restoreDefaultSetting = resolve<decltype(&NvAPI_DRS_RestoreProfileDefaultSetting)>(RestoreDefaultSettingId);
        getBaseProfile = resolve<decltype(&NvAPI_DRS_GetBaseProfile)>(GetBaseProfileId);
        if (!initialize || !unload || !createSession || !destroySession || !loadSettings
            || !saveSettings || !setSetting || !getSetting || !enumSettingIds
            || !restoreDefaultSetting || !getBaseProfile) {
            error_ = QStringLiteral("当前 NVIDIA 驱动缺少所需 NVAPI DRS 接口");
            return;
        }
        const NvAPI_Status status = initialize();
        if (status != NVAPI_OK) {
            error_ = QStringLiteral("NVAPI 初始化失败: %1").arg(static_cast<int>(status));
            return;
        }
        initialized_ = true;
    }

    ~NvapiRuntime() {
        if (initialized_ && unload) {
            unload();
        }
        if (module_) {
            FreeLibrary(module_);
        }
    }

    bool available() const { return initialized_; }
    QString error() const { return error_; }

    decltype(&NvAPI_DRS_CreateSession) createSession = nullptr;
    decltype(&NvAPI_DRS_DestroySession) destroySession = nullptr;
    decltype(&NvAPI_DRS_LoadSettings) loadSettings = nullptr;
    decltype(&NvAPI_DRS_SaveSettings) saveSettings = nullptr;
    decltype(&NvAPI_DRS_SetSetting) setSetting = nullptr;
    decltype(&NvAPI_DRS_GetSetting) getSetting = nullptr;
    decltype(&NvAPI_DRS_EnumAvailableSettingIds) enumSettingIds = nullptr;
    decltype(&NvAPI_DRS_RestoreProfileDefaultSetting) restoreDefaultSetting = nullptr;
    decltype(&NvAPI_DRS_GetBaseProfile) getBaseProfile = nullptr;

private:
    using QueryInterfaceFn = void* (__cdecl*)(NvU32);

    template<typename Function>
    Function resolve(NvU32 id) const {
        return reinterpret_cast<Function>(query_(id));
    }

    HMODULE module_ = nullptr;
    QueryInterfaceFn query_ = nullptr;
    decltype(&NvAPI_Initialize) initialize = nullptr;
    decltype(&NvAPI_Unload) unload = nullptr;
    bool initialized_ = false;
    QString error_;
};

bool containsSetting(NvapiRuntime& runtime, NvU32 settingId) {
    std::array<NvU32, 2048> ids{};
    NvU32 count = static_cast<NvU32>(ids.size());
    if (runtime.enumSettingIds(ids.data(), &count) != NVAPI_OK) {
        return false;
    }
    for (NvU32 index = 0; index < count && index < ids.size(); ++index) {
        if (ids[index] == settingId) {
            return true;
        }
    }
    return false;
}

bool openBaseProfile(
    NvapiRuntime& runtime,
    NvDRSSessionHandle* session,
    NvDRSProfileHandle* profile,
    QString* error
) {
    NvAPI_Status status = runtime.createSession(session);
    if (status == NVAPI_OK) {
        status = runtime.loadSettings(*session);
    }
    if (status == NVAPI_OK) {
        status = runtime.getBaseProfile(*session, profile);
    }
    if (status != NVAPI_OK) {
        if (*session) {
            runtime.destroySession(*session);
            *session = nullptr;
        }
        if (error) {
            *error = QStringLiteral("无法打开 NVIDIA 全局配置: %1").arg(static_cast<int>(status));
        }
        return false;
    }
    return true;
}

bool readSetting(
    NvapiRuntime& runtime,
    NvDRSSessionHandle session,
    NvDRSProfileHandle profile,
    NvU32 settingId,
    NVDRS_SETTING* setting
) {
    std::memset(setting, 0, sizeof(*setting));
    setting->version = NVDRS_SETTING_VER;
    return runtime.getSetting(session, profile, settingId, setting) == NVAPI_OK;
}

bool writeSetting(
    NvapiRuntime& runtime,
    NvDRSSessionHandle session,
    NvDRSProfileHandle profile,
    NvU32 settingId,
    NvU32 value,
    QString* error
) {
    NVDRS_SETTING setting{};
    setting.version = NVDRS_SETTING_VER;
    setting.settingId = settingId;
    setting.settingType = NVDRS_DWORD_TYPE;
    setting.u32CurrentValue = value;
    const NvAPI_Status status = runtime.setSetting(session, profile, &setting);
    if (status != NVAPI_OK && error) {
        *error = QStringLiteral("NVAPI 设置写入失败 (%1): %2").arg(settingId, 0, 16).arg(static_cast<int>(status));
    }
    return status == NVAPI_OK;
}

}  // namespace
#endif

bool NvidiaNvapiBridge::supportsGlobalProfileSettings(QString* details) {
#ifdef Q_OS_WIN
    NvapiRuntime runtime;
    const bool supported = runtime.available()
        && containsSetting(runtime, PREFERRED_PSTATE_ID)
        && containsSetting(runtime, VSYNCMODE_ID);
    if (details) {
        *details = supported
            ? QStringLiteral("NVAPI 支持全局电源管理和垂直同步设置")
            : (runtime.error().isEmpty() ? QStringLiteral("当前驱动不支持所需 NVAPI 设置") : runtime.error());
    }
    return supported;
#else
    if (details) {
        *details = QStringLiteral("仅 Windows 支持 NVAPI");
    }
    return false;
#endif
}

QString NvidiaNvapiBridge::applyGlobalPerformanceSettings(int* exitCode) {
#ifdef Q_OS_WIN
    NvapiRuntime runtime;
    if (!runtime.available()) {
        if (exitCode) *exitCode = -1;
        return runtime.error();
    }
    NvDRSSessionHandle session = nullptr;
    NvDRSProfileHandle profile = nullptr;
    QString error;
    if (!openBaseProfile(runtime, &session, &profile, &error)) {
        if (exitCode) *exitCode = -1;
        return error;
    }

    QSettings settings;
    settings.beginGroup(BackupGroup);
    if (!settings.value(QStringLiteral("valid"), false).toBool()) {
        NVDRS_SETTING power{};
        NVDRS_SETTING vsync{};
        if (!readSetting(runtime, session, profile, PREFERRED_PSTATE_ID, &power)
            || !readSetting(runtime, session, profile, VSYNCMODE_ID, &vsync)) {
            runtime.destroySession(session);
            settings.endGroup();
            if (exitCode) *exitCode = -1;
            return QStringLiteral("无法读取 NVIDIA 原始设置，未进行修改");
        }
        settings.setValue(QStringLiteral("powerOverride"), power.isCurrentPredefined == 0);
        settings.setValue(QStringLiteral("powerValue"), power.u32CurrentValue);
        settings.setValue(QStringLiteral("vsyncOverride"), vsync.isCurrentPredefined == 0);
        settings.setValue(QStringLiteral("vsyncValue"), vsync.u32CurrentValue);
    }

    const bool wrotePower = writeSetting(runtime, session, profile, PREFERRED_PSTATE_ID, PREFERRED_PSTATE_PREFER_MAX, &error);
    const bool wroteVsync = wrotePower && writeSetting(runtime, session, profile, VSYNCMODE_ID, VSYNCMODE_FORCEOFF, &error);
    const NvAPI_Status saveStatus = wroteVsync ? runtime.saveSettings(session) : NVAPI_ERROR;
    runtime.destroySession(session);
    if (!wrotePower || !wroteVsync || saveStatus != NVAPI_OK) {
        settings.endGroup();
        if (exitCode) *exitCode = -1;
        return error.isEmpty() ? QStringLiteral("NVIDIA 配置保存失败: %1").arg(static_cast<int>(saveStatus)) : error;
    }
    settings.setValue(QStringLiteral("valid"), true);
    settings.sync();
    settings.endGroup();
    if (exitCode) *exitCode = 0;
    return QStringLiteral("NVAPI 已设置最高性能电源管理，并强制关闭垂直同步。低延迟模式需在 NVIDIA 控制面板中手动设置。");
#else
    if (exitCode) *exitCode = -1;
    return QStringLiteral("仅 Windows 支持 NVAPI");
#endif
}

QString NvidiaNvapiBridge::restoreGlobalPerformanceSettings(int* exitCode) {
#ifdef Q_OS_WIN
    QSettings settings;
    settings.beginGroup(BackupGroup);
    if (!settings.value(QStringLiteral("valid"), false).toBool()) {
        settings.endGroup();
        if (exitCode) *exitCode = 0;
        return QStringLiteral("NVIDIA NVAPI 设置已处于还原状态。");
    }
    const bool powerOverride = settings.value(QStringLiteral("powerOverride")).toBool();
    const NvU32 powerValue = settings.value(QStringLiteral("powerValue")).toUInt();
    const bool vsyncOverride = settings.value(QStringLiteral("vsyncOverride")).toBool();
    const NvU32 vsyncValue = settings.value(QStringLiteral("vsyncValue")).toUInt();

    NvapiRuntime runtime;
    NvDRSSessionHandle session = nullptr;
    NvDRSProfileHandle profile = nullptr;
    QString error;
    if (!runtime.available() || !openBaseProfile(runtime, &session, &profile, &error)) {
        settings.endGroup();
        if (exitCode) *exitCode = -1;
        return error.isEmpty() ? runtime.error() : error;
    }
    auto restoreSetting = [&](NvU32 id, bool hadOverride, NvU32 value) {
        if (hadOverride) {
            return writeSetting(runtime, session, profile, id, value, &error);
        }
        const NvAPI_Status status = runtime.restoreDefaultSetting(session, profile, id);
        if (status != NVAPI_OK && error.isEmpty()) {
            error = QStringLiteral("NVAPI 默认值还原失败 (%1): %2").arg(id, 0, 16).arg(static_cast<int>(status));
        }
        return status == NVAPI_OK;
    };
    const bool powerOk = restoreSetting(PREFERRED_PSTATE_ID, powerOverride, powerValue);
    const bool vsyncOk = powerOk && restoreSetting(VSYNCMODE_ID, vsyncOverride, vsyncValue);
    const NvAPI_Status saveStatus = vsyncOk ? runtime.saveSettings(session) : NVAPI_ERROR;
    runtime.destroySession(session);
    if (!powerOk || !vsyncOk || saveStatus != NVAPI_OK) {
        settings.endGroup();
        if (exitCode) *exitCode = -1;
        return error.isEmpty() ? QStringLiteral("NVIDIA 配置还原保存失败: %1").arg(static_cast<int>(saveStatus)) : error;
    }
    settings.remove(QString());
    settings.endGroup();
    if (exitCode) *exitCode = 0;
    return QStringLiteral("已恢复 NVIDIA 原始电源管理和垂直同步设置。");
#else
    if (exitCode) *exitCode = -1;
    return QStringLiteral("仅 Windows 支持 NVAPI");
#endif
}
