#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>

#include <optional>

#ifndef PANORAMA_PROBE_STAGE
#define PANORAMA_PROBE_STAGE 0
#endif

namespace {
RED4ext::v1::PluginHandle g_pluginHandle;
RED4ext::v1::Logger *g_logger;
RED4ext::CClass *g_gameInstanceClass;
RED4ext::CBaseFunction *g_getCameraSystem;
std::optional<RED4ext::ScriptGameInstance> g_gameInstance;
uint32_t g_probeTicks;

void LogInfo(const char *aMessage) {
    if (g_logger != nullptr) {
        g_logger->Info(g_pluginHandle, aMessage);
    }
}

bool OnUpdate(RED4ext::CGameApplication *);

bool OnRunning(RED4ext::CGameApplication *) {
    g_logger->InfoF(g_pluginHandle, "Camera probe: running stage %d.", PANORAMA_PROBE_STAGE);

#if PANORAMA_PROBE_STAGE >= 1
    auto *rtti = RED4ext::CRTTISystem::Get();
    g_gameInstanceClass = rtti != nullptr ? rtti->GetClass("ScriptGameInstance") : nullptr;
    g_getCameraSystem = g_gameInstanceClass != nullptr
                            ? g_gameInstanceClass->GetFunction("GetCameraSystem")
                            : nullptr;
    if (g_getCameraSystem == nullptr) {
        LogInfo("Camera probe: GetCameraSystem was not found.");
        return true;
    }
    LogInfo("Camera probe: RTTI lookup succeeded.");
#endif

#if PANORAMA_PROBE_STAGE >= 2
    g_gameInstance.emplace();
    LogInfo("Camera probe: ScriptGameInstance construction succeeded.");
#endif

#if PANORAMA_PROBE_STAGE >= 3
    LogInfo("Camera probe: deferring GetCameraSystem until the update loop.");
#endif

    return true;
}

bool OnUpdate(RED4ext::CGameApplication *) {
#if PANORAMA_PROBE_STAGE >= 3
    if (++g_probeTicks < 600) {
        return false;
    }

    RED4ext::Handle<RED4ext::IScriptable> cameraSystem;
    LogInfo("Camera probe: about to invoke GetCameraSystem.");
    if (!RED4ext::ExecuteFunction(g_gameInstanceClass, g_getCameraSystem, &cameraSystem,
                                  &*g_gameInstance) ||
        !cameraSystem) {
        LogInfo("Camera probe: GetCameraSystem returned no instance.");
        return true;
    }
    LogInfo("Camera probe: GetCameraSystem invocation succeeded.");
#endif

#if PANORAMA_PROBE_STAGE >= 4
    auto *cameraClass = cameraSystem.instance->GetType();
    if (cameraClass == nullptr) {
        LogInfo("Camera probe: camera system has no RTTI class.");
        return true;
    }
#endif

#if PANORAMA_PROBE_STAGE >= 5
    const auto *className = cameraClass->GetName().ToString();
#endif

#if PANORAMA_PROBE_STAGE >= 6
    const auto hasForward = cameraClass->GetFunction("GetActiveCameraForward") != nullptr;
    const auto hasRight = cameraClass->GetFunction("GetActiveCameraRight") != nullptr;
    g_logger->InfoF(g_pluginHandle, "Camera probe: class=%s forward=%s right=%s",
                    className != nullptr ? className : "<unnamed>", hasForward ? "yes" : "no",
                    hasRight ? "yes" : "no");
#endif

#if PANORAMA_PROBE_STAGE == 4
    LogInfo("Camera probe: GetType succeeded.");
#elif PANORAMA_PROBE_STAGE == 5
    g_logger->InfoF(g_pluginHandle, "Camera probe: class=%s.",
                    className != nullptr ? className : "<unnamed>");
#endif
    return true;
}
} // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle,
                                        RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk *aSdk) {
    if (aReason == RED4ext::v1::EMainReason::Load) {
        g_pluginHandle = aHandle;
        g_logger = aSdk->logger;
        g_logger->InfoF(g_pluginHandle, "PanoramaCaptureProbe loaded for Cyberpunk 2077 %u.%u.%u.",
                        aSdk->runtime->major, aSdk->runtime->minor, aSdk->runtime->patch);

        RED4ext::v1::GameState state{
            .OnEnter = OnRunning,
            .OnUpdate = OnUpdate,
            .OnExit = nullptr,
        };
        aSdk->gameStates->Add(aHandle, RED4ext::EGameStateType::Running, &state);
    }

    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo *aInfo) {
    aInfo->name = L"PanoramaCaptureProbe";
    aInfo->author = L"Panorama Capture";
    aInfo->version = RED4EXT_V1_SEMVER(1, 1, 4);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_2_31;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return RED4EXT_API_VERSION_1; }
