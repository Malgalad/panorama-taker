#include <reshade.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifndef PANORAMA_ADDON_TEST_KEY
#define PANORAMA_ADDON_TEST_KEY 0x79 // F10
#endif

namespace
{
struct BridgeRequest
{
    std::string session;
    std::string pose;
    std::string token;
    bool issued = false;
};

struct BridgeCompletion
{
    BridgeRequest request;
    std::string path;
};

std::atomic<reshade::api::effect_runtime *> g_runtime {nullptr};
std::atomic<std::uint32_t> g_sequence {0};
std::mutex g_mutex;
std::condition_variable g_worker_wakeup;
std::optional<BridgeRequest> g_bridge_request;
std::optional<BridgeCompletion> g_completion;
std::chrono::steady_clock::time_point g_pending_since;
std::string g_token;
bool g_pending = false;
std::atomic<bool> g_worker_running {false};
std::thread g_worker;
std::filesystem::path g_bridge_request_path;
std::filesystem::path g_bridge_ack_path;

void log_info(const char *message)
{
    reshade::log::message(reshade::log::level::info, message);
}

void log_error(const char *message)
{
    reshade::log::message(reshade::log::level::error, message);
}

bool parse_request(const std::string &line, BridgeRequest &request)
{
    const auto first = line.find('\t');
    const auto second = line.find('\t', first + 1);
    const auto third = line.find('\t', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos ||
        line.substr(0, first) != "1")
        return false;
    request.session = line.substr(first + 1, second - first - 1);
    request.pose = line.substr(second + 1, third - second - 1);
    request.token = line.substr(third + 1);
    return !request.session.empty() && !request.pose.empty() && !request.token.empty();
}

void write_ack(const BridgeCompletion &completion)
{
    const auto temporary = g_bridge_ack_path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        log_error("PanoramaCaptureReShade: cannot open bridge acknowledgement file");
        return;
    }
    output << "1\t" << completion.request.session << '\t' << completion.request.pose << '\t'
           << completion.request.token << '\t' << completion.path << '\n';
    output.close();
    std::error_code error;
    std::filesystem::remove(g_bridge_ack_path, error);
    std::filesystem::rename(temporary, g_bridge_ack_path, error);
    if (error)
        log_error("PanoramaCaptureReShade: cannot publish bridge acknowledgement file");
}

void bridge_worker_loop()
{
    while (true)
    {
        std::optional<BridgeCompletion> completion;
        {
            std::unique_lock lock(g_mutex);
            g_worker_wakeup.wait_for(lock, std::chrono::milliseconds(25), [] {
                return !g_worker_running.load(std::memory_order_acquire) || g_completion.has_value();
            });
            if (!g_worker_running.load(std::memory_order_acquire))
                break;
            completion = std::move(g_completion);
            g_completion.reset();
        }
        if (completion.has_value())
            write_ack(*completion);

        if (std::filesystem::exists(g_bridge_request_path))
        {
            std::ifstream input(g_bridge_request_path, std::ios::binary);
            std::string line;
            std::getline(input, line);
            input.close();
            std::error_code error;
            std::filesystem::remove(g_bridge_request_path, error);
            BridgeRequest request;
            if (parse_request(line, request))
            {
                std::lock_guard lock(g_mutex);
                if (!g_bridge_request.has_value() && !g_pending)
                    g_bridge_request = std::move(request);
            }
            else
                log_error("PanoramaCaptureReShade: invalid bridge request");
        }
    }
}

void resolve_bridge_paths()
{
    char base[4096] = {};
    size_t base_size = sizeof(base);
    reshade::get_reshade_base_path(base, &base_size);
    const std::filesystem::path directory(base[0] != '\0' ? base : ".");
    const auto bridge_directory = directory / "plugins" / "cyber_engine_tweaks" / "mods" /
        "PanoramaCaptureProbe";
    g_bridge_request_path = bridge_directory / "PanoramaCaptureBridge.request";
    g_bridge_ack_path = bridge_directory / "PanoramaCaptureBridge.ack";
}

void on_init_runtime(reshade::api::effect_runtime *runtime)
{
    g_runtime.store(runtime, std::memory_order_release);
    log_info("PanoramaCaptureReShade: effect runtime ready");
}

void on_destroy_runtime(reshade::api::effect_runtime *runtime)
{
    auto *expected = runtime;
    g_runtime.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    std::lock_guard lock(g_mutex);
    g_bridge_request.reset();
    g_pending = false;
    g_token.clear();
}

void on_present(reshade::api::effect_runtime *runtime)
{
    if (runtime != g_runtime.load(std::memory_order_acquire))
        return;

    std::string request_token;
    bool bridge = false;
    bool timed_out = false;
    std::optional<BridgeCompletion> timeout_completion;
    {
        std::lock_guard lock(g_mutex);
        if (g_pending && std::chrono::steady_clock::now() - g_pending_since >= std::chrono::seconds(10))
        {
            if (g_bridge_request.has_value() && g_bridge_request->issued)
                timeout_completion = BridgeCompletion {*g_bridge_request, "ERROR:timeout"};
            g_pending = false;
            g_token.clear();
            g_bridge_request.reset();
            timed_out = true;
        }
        if (!g_pending && g_bridge_request.has_value() && !g_bridge_request->issued)
        {
            g_bridge_request->issued = true;
            request_token = g_bridge_request->token;
            g_token = request_token;
            g_pending_since = std::chrono::steady_clock::now();
            g_pending = true;
            bridge = true;
        }
    }
    if (timed_out)
    {
        log_error("PanoramaCaptureReShade: screenshot timed out after 10 seconds");
        if (timeout_completion.has_value())
        {
            std::lock_guard lock(g_mutex);
            g_completion = std::move(timeout_completion);
            g_worker_wakeup.notify_one();
        }
    }
    if (bridge)
    {
        runtime->save_screenshot(request_token.c_str());
        char message[256] = {};
        std::snprintf(message, sizeof(message),
            "PanoramaCaptureReShade: bridge screenshot requested token=%s", request_token.c_str());
        log_info(message);
        return;
    }

    {
        std::lock_guard lock(g_mutex);
        if (g_pending)
            return;
    }
    if (!runtime->is_key_pressed(PANORAMA_ADDON_TEST_KEY))
        return;
    const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    char token[48] = {};
    std::snprintf(token, sizeof(token), "pano-test-%06u", sequence);
    request_token = token;
    {
        std::lock_guard lock(g_mutex);
        g_token = request_token;
        g_pending_since = std::chrono::steady_clock::now();
        g_pending = true;
    }
    runtime->save_screenshot(request_token.c_str());
    char message[128] = {};
    std::snprintf(message, sizeof(message),
        "PanoramaCaptureReShade: screenshot requested token=%s", request_token.c_str());
    log_info(message);
}

void on_screenshot(reshade::api::effect_runtime *runtime, const char *path)
{
    if (runtime != g_runtime.load(std::memory_order_acquire) || path == nullptr)
        return;
    const std::string saved_path(path);
    std::lock_guard lock(g_mutex);
    if (!g_pending || saved_path.find(g_token) == std::string::npos)
        return;
    if (g_bridge_request.has_value() && g_bridge_request->issued)
    {
        g_completion = BridgeCompletion {*g_bridge_request, saved_path};
        g_bridge_request.reset();
        g_pending = false;
        g_token.clear();
        g_worker_wakeup.notify_one();
        return;
    }
    char message[1024] = {};
    std::snprintf(message, sizeof(message),
        "PanoramaCaptureReShade: screenshot saved token=%s path=%s", g_token.c_str(), saved_path.c_str());
    log_info(message);
    g_pending = false;
    g_token.clear();
}
} // namespace

extern "C" __declspec(dllexport) const char *NAME = "Panorama Capture ReShade";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Bridges CET pose requests to ReShade HDR screenshots for PanoramaCapture.";

void stop_bridge_worker(bool join)
{
    g_worker_running.store(false, std::memory_order_release);
    g_worker_wakeup.notify_all();
    if (!g_worker.joinable())
        return;
    if (join)
        g_worker.join();
    else
        g_worker.detach();
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE, HMODULE)
{
    resolve_bridge_paths();
    g_worker_running.store(true, std::memory_order_release);
    g_worker = std::thread(bridge_worker_loop);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE, HMODULE)
{
    stop_bridge_worker(true);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_runtime);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_screenshot>(on_screenshot);
        break;
    }
    case DLL_PROCESS_DETACH:
        if (reserved != nullptr)
            stop_bridge_worker(false);
        reshade::unregister_addon(module);
        break;
    default:
        break;
    }
    return TRUE;
}
