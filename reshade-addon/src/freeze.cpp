#include <windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <cstddef>
#include <mutex>

namespace freeze {
using Timing = std::array<unsigned char, 24>;
struct LayerType {
    void *base[2];
    LayerType *parent;
    uint64_t name;
};
struct Job { void *layer; const Timing *timing; };
using JobFn = void (*)(Job *, void *);
using CopyFn = void *(*)(void *, void *);
static JobFn originalActive, originalPassive;
static CopyFn originalCopy;
static std::mutex guard;
static std::atomic<bool> requested, latched;
static std::atomic<unsigned long> jobs;
// JobQueue(group) attaches child queues to the completion handle at group+0x20.
struct Completion {
    unsigned char header[0x18];
    volatile LONG outstanding;
    volatile LONG references;
};
struct JobGroup {
    unsigned char header[0x20];
    Completion **completion;
};
static_assert(offsetof(Completion, references) == 0x1c);
static_assert(offsetof(JobGroup, completion) == 0x20);
using FreeHandle = void (*)(Completion *);
static FreeHandle freeHandle;
struct GroupRecords {
    unsigned parentsRunning{};
    std::set<Timing> timings;
};
static std::map<Completion *, GroupRecords> retained;
static unsigned sweepCountdown;

static void collectCompletedGroups() {
    for (auto it = retained.begin(); it != retained.end();) {
        auto *group = it->first;
        if (it->second.parentsRunning || InterlockedCompareExchange(&group->outstanding, 0, 0) != 0) {
            ++it;
            continue;
        }
        it = retained.erase(it);
        if (InterlockedDecrement(&group->references) == 0) freeHandle(group);
    }
}
static bool installed;
static bool attempted;

constexpr uint64_t nameHash(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) h = (h ^ static_cast<unsigned char>(*s++)) * 1099511628211ULL;
    return h;
}
struct Clock { double correction{}; float phase{}; bool held{}; };
static std::array<Clock, 3> clocks;
static void *copy(void *destination, void *source) {
    auto result = originalCopy(destination, source);
    std::lock_guard lock(guard);
    if (!retained.empty() && sweepCountdown-- == 0) {
        collectCompletedGroups();
        sweepCountdown = 15;
    }
    const bool hold = requested.load();
    for (size_t i = 0; i < clocks.size(); ++i) {
        auto &clock = clocks[i];
        auto *field = static_cast<unsigned char *>(destination) + 0x1ac + i * sizeof(float);
        float input;
        std::memcpy(&input, field, sizeof(input));
        if (hold && !clock.held) clock.phase = static_cast<float>(input - clock.correction);
        if (!hold && clock.held) clock.correction = static_cast<double>(input) - clock.phase;
        clock.held = hold;
        float output = hold ? clock.phase : static_cast<float>(input - clock.correction);
        std::memcpy(field, &output, sizeof(output));
    }
    latched.store(hold);
    return result;
}
static bool selected(void *layer) {
    using GetType = LayerType *(*)(void *);
    auto getType = (*static_cast<GetType **>(layer))[0];
    for (auto *t = getType(layer); t; t = t->parent) {
        if (t->name == nameHash("inkStreetSignsLayer") || t->name == nameHash("inkAdvertisementsLayer") ||
            t->name == nameHash("inkWorldLayer") || t->name == nameHash("inkWorldFluffLayer")) return true;
    }
    return false;
}
static void tick(JobFn original, Job *job, void *context) {
    const Timing *previous = nullptr;
    GroupRecords *storage = nullptr;
    auto *group = static_cast<JobGroup *>(context);
    if (requested.load() && job && job->layer && job->timing && selected(job->layer) &&
        group && group->completion && *group->completion) {
        auto timing = *job->timing;
        std::memset(timing.data(), 0, sizeof(float));
        std::lock_guard lock(guard);
        auto *handle = *group->completion;
        auto entry = retained.try_emplace(handle);
        if (entry.second) InterlockedIncrement(&handle->references);
        previous = job->timing;
        storage = &entry.first->second;
        ++storage->parentsRunning;
        job->timing = &*storage->timings.insert(timing).first;
        ++jobs;
    }
    original(job, context);
    if (storage) {
        job->timing = previous;
        std::lock_guard lock(guard);
        --storage->parentsRunning;
    }
}
static void active(Job *j, void *c) { tick(originalActive, j, c); }
static void passive(Job *j, void *c) { tick(originalPassive, j, c); }

bool install() {
    if (attempted) return installed;
    attempted = true;
    auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *pe = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (pe->FileHeader.TimeDateStamp != 0x6ad55daf || pe->OptionalHeader.SizeOfImage != 0x581b000) return false;
    const unsigned char framePrefix[] = {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20};
    const unsigned char activePrefix[] = {0x48,0x89,0x5c,0x24,8,0x57,0x48,0x83,0xec,0x60};
    const unsigned char passivePrefix[] = {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x70};
    void *sites[] = {base+0x499e1c,base+0x4c0af0,base+0x4bfed0};
    if (std::memcmp(sites[0],framePrefix,sizeof(framePrefix)) ||
        std::memcmp(sites[1],activePrefix,sizeof(activePrefix)) ||
        std::memcmp(sites[2],passivePrefix,sizeof(passivePrefix))) return false;
    // This build's final-reference recycler takes the internal handle directly.
    const unsigned char recyclerPrefix[] = {0x48,0x83,0xec,0x38,0x48,0x85,0xc9};
    if (std::memcmp(base+0x275c34,recyclerPrefix,sizeof(recyclerPrefix))) return false;
    freeHandle = reinterpret_cast<FreeHandle>(base+0x275c34);
    HMODULE pinned;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&installed), &pinned) || MH_Initialize() != MH_OK) return false;
    void *callbacks[] = {reinterpret_cast<void *>(copy),reinterpret_cast<void *>(active),reinterpret_cast<void *>(passive)};
    void **originals[] = {reinterpret_cast<void **>(&originalCopy),reinterpret_cast<void **>(&originalActive),reinterpret_cast<void **>(&originalPassive)};
    for (size_t i = 0; i < 3; ++i) {
        if (MH_CreateHook(sites[i], callbacks[i], originals[i]) != MH_OK || MH_QueueEnableHook(sites[i]) != MH_OK) {
            MH_Uninitialize(); return false;
        }
    }
    installed = MH_ApplyQueued() == MH_OK;
    if (!installed) MH_Uninitialize();
    return installed;
}
void set(bool value) {
    std::lock_guard lock(guard);
    if (value) jobs.store(0);
    latched.store(false);
    requested.store(value);
}
bool ready() { return latched.load(); }
unsigned long matches() { return jobs.load(); }
}
