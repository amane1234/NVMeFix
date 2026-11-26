// IOKit and NVMe command doubles. The generated includes contain the production methods.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <stdatomic.h>
#include <uuid/uuid.h>
#include "nvme.h"

#define DBGLOG(...) ((void)0)
#define SYSLOG(...) ((void)0)
using IOReturn = int;
using IOPMPowerFlags = unsigned;
constexpr int kIOReturnSuccess = 0, kIOReturnNoResources = 1, kIODirectionIn = 1, kIODirectionOut = 2;
constexpr int kIOPMAckImplied = 0, IOPMAckImplied = 0, kIOPMDeviceUsable = 4;
constexpr int kIOPMPowerStateVersion1 = 1, kIOPMPowerOn = 2, kIOPMPreventIdleSleep = 8;
constexpr int kIOPMSuperclassPolicy1 = 1;
struct IOPMPowerState {
    unsigned version, capabilityFlags, outputPowerCharacter, inputPowerRequirement, unused[8];
};
struct IONotifier {};
struct KernelPatcher {};

// Thread calls run only when the test drains them, so each deferral can be checked.
using thread_call_param_t = void*;
struct thread_call {
    void (*fn)(void*, void*);
    void* param;
    bool pending = false;
    uint64_t deadline = 0;
};
using thread_call_t = thread_call*;
std::vector<thread_call*> threadCalls;
constexpr uint32_t kMillisecondScale = 1000000;
thread_call_t thread_call_allocate(void (*fn)(void*, void*), void* param) {
    threadCalls.push_back(new thread_call{fn, param});
    return threadCalls.back();
}
bool thread_call_enter(thread_call_t call) {
    bool was = call->pending;
    call->pending = true;
    call->deadline = 0;
    return was;
}
bool thread_call_enter_delayed(thread_call_t call, uint64_t deadline) {
    bool was = call->pending;
    call->pending = true;
    call->deadline = deadline;
    return was;
}
bool thread_call_free(thread_call_t call) {
    assert(!call->pending);
    for (size_t i = 0; i < threadCalls.size(); ++i)
        if (threadCalls[i] == call) threadCalls.erase(threadCalls.begin() + i);
    delete call;
    return true;
}
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t* deadline) {
    *deadline = uint64_t(interval) * scale;
}
// Run the calls pending now once; calls they schedule again wait for the next round.
bool runPending() {
    std::vector<thread_call*> due;
    for (auto call : threadCalls)
        if (call->pending) due.push_back(call);
    for (auto call : due) {
        call->pending = false;
        call->fn(call->param, nullptr); // May free calls later in the list: each is run before its free.
    }
    return !due.empty();
}
void drainPending() { while (runPending()) {} }
bool rootAvailable = true;
namespace NVMe {
enum nvme_quirks { NVME_QUIRK_NONE = 0, NVME_QUIRK_NO_APST = 16, NVME_QUIRK_NO_DEEPEST_PS = 32 };
}

struct IOLock {
    std::mutex mutex;
    unsigned acquisitions = 0;
};
IOLock* IOLockAlloc() { return new IOLock; }
void IOLockFree(IOLock* lock) { delete lock; }
void IOLockLock(IOLock* lock) { lock->mutex.lock(); ++lock->acquisitions; }
bool IOLockTryLock(IOLock* lock) { return lock->mutex.try_lock(); }
void IOLockUnlock(IOLock* lock) { lock->mutex.unlock(); }

struct IOBufferMemoryDescriptor {
    std::vector<uint8_t> bytes;
    static auto withCapacity(size_t size, int) {
        return new IOBufferMemoryDescriptor{std::vector<uint8_t>(size)};
    }
    size_t getLength() const { return bytes.size(); }
    void* getBytesNoCopy() { return bytes.data(); }
    void release() { delete this; }
};

struct IOService {
    unsigned references = 1, registrations = 0, tickles = 0, timer = 0;
    int published = -1, driverRequest = 0, deviceRequest = 0;
    bool subscribed = false, enabled = false, rejectSet = false, rejectGet = false;
    bool rejectPS = false, rejectTimer = false, corruptReadback = false, requested = false;
    unsigned sets = 0, gets = 0, manualPS = 0, manualSets = 0, failSets = 0, acks = 0, resetGeneration = 0;
    alignas(uint64_t) NVMe::nvme_feat_auto_pst table{};
    virtual ~IOService() = default;
    virtual void free() { delete this; }
    void release() { if (--references == 0) free(); }
    void retain() { ++references; }
    void setProperty(const char* key, bool value) {
        if (!strcmp(key, "apst")) published = value;
        else { assert(!strcmp(key, "apst-requested")); requested = value; }
    }
    void setProperty(const char* key, unsigned value, unsigned) {
        assert(!strcmp(key, "reset-generation"));
        resetGeneration = value;
    }
    void removeProperty(const char* key) { assert(!strcmp(key, "apst")); published = -1; }
    void acknowledgeSetPowerState() { ++acks; }
    bool init() { return true; }
    void PMinit() {}
    void PMstop() {}
    void joinPMtree(IOService*) {}
    static IOService* getPMRootDomain() { static IOService root; return rootAvailable ? &root : nullptr; }
    void registerInterestedDriver(IOService*) { subscribed = true; }
    void deRegisterInterestedDriver(IOService*) { subscribed = false; }
    bool metaCast(const char*) { return true; }
    int registerPowerDriver(IOService*, IOPMPowerState*, size_t) {
        ++registrations;
        assert(registrations == 1);
        return 0;
    }
    int makeUsable() { deviceRequest = 3; return 0; }
    int changePowerStateTo(unsigned state) { driverRequest = state; return 0; }
    int changePowerStateToPriv(unsigned state) { deviceRequest = state; return 0; }
    int setIdleTimerPeriod(unsigned period) {
        if (rejectTimer && period) return kIOReturnNoResources;
        timer = period;
        return 0;
    }
    void activityTickle(unsigned long type, unsigned long state) {
        assert(type == kIOPMSuperclassPolicy1 && state == 3);
        ++tickles;
    }
};

struct NVMeFixPlugin {
#include "entry.inc"
    struct Registry : std::vector<ControllerEntry*> {
        void erase(size_t i) { std::vector<ControllerEntry*>::erase(begin() + i); }
    } controllers;
    IOLock registryLock, *lck = &registryLock;
    atomic_uint activePMControllers = 0;
    bool tickleHooked = true;
    atomic_int onAC = -1;
    struct {
        struct {
            unsigned calls = 0;
            bool activityTickle(void* controller, unsigned long type, unsigned long state) {
                assert(controller && type == 7 && state == 2);
                ++calls;
                return false;
            }
        } IONVMeController;
    } kextFuncs;
    static NVMeFixPlugin& globalPlugin() { static NVMeFixPlugin plugin; return plugin; }
    void setActivePM(ControllerEntry&, bool);
    void releaseEntry(ControllerEntry&);
    uint64_t latencyLimit(const ControllerEntry&) const;
    bool apstAllowed(const ControllerEntry&) const;
    APSTState applyAPST(ControllerEntry&, const NVMe::nvme_id_ctrl*);
    IOReturn configureAPST(ControllerEntry&, const NVMe::nvme_id_ctrl*, bool&, IOBufferMemoryDescriptor*);
    IOReturn verifyAPST(ControllerEntry&, const NVMe::nvme_id_ctrl*, const IOBufferMemoryDescriptor*, bool&, bool&);
    IOReturn APSTenabled(ControllerEntry&, bool&);
    IOReturn dumpAPST(ControllerEntry&, int);
    ControllerEntry* entryForController(IOService*) const;
    static bool terminatedNotificationHandler(void*, void*, IOService*, IONotifier*);
#include "pm.inc"
    NVMeFixPlugin() : PM(*this) {}

    IOReturn NVMeFeatures(ControllerEntry& entry, unsigned fid, unsigned* word,
                         IOBufferMemoryDescriptor* buffer, uint32_t* result, bool set) {
        auto& drive = *entry.controller;
        if (fid == NVMe::NVME_FEAT_POWER_MGMT) {
            if (set) {
                ++drive.manualSets;
                if (drive.rejectPS) return kIOReturnNoResources;
                drive.manualPS = *word;
            } else *result = drive.manualPS;
            return 0;
        }
        assert(fid == NVMe::NVME_FEAT_AUTO_PST && buffer->getLength() == sizeof(drive.table));
        if (set) {
            ++drive.sets;
            if (drive.failSets) { --drive.failSets; return kIOReturnNoResources; }
            if (drive.rejectSet) return kIOReturnNoResources;
            drive.enabled = *word != 0;
            memcpy(&drive.table, buffer->getBytesNoCopy(), sizeof(drive.table));
        } else {
            ++drive.gets;
            if (drive.rejectGet) return kIOReturnNoResources;
            if (result) *result = drive.enabled | 0x10; // Only APSTE indicates enabled.
            memcpy(buffer->getBytesNoCopy(), &drive.table, sizeof(drive.table));
            if (drive.corruptReadback) // The controller reports a different table than it accepted.
                static_cast<NVMe::nvme_feat_auto_pst*>(buffer->getBytesNoCopy())->entries[0] ^= 1U << 8;
        }
        return 0;
    }
};

struct NVMePMProxy : IOService {
    NVMeFixPlugin::ControllerEntry* entry = nullptr;
    IOReturn powerStateDidChangeTo(IOPMPowerFlags, unsigned long, IOService*);
    IOReturn setPowerState(unsigned long, IOService*);
    void free() override;
    bool initWork();
    bool scheduleAck(unsigned long);
    bool scheduleRetry(uint32_t, uint32_t);
    void applyPowerState(unsigned long);
    thread_call_t ackCall = nullptr, retryCall = nullptr;
    unsigned long pendingOrdinal = 0;
    uint32_t retryGeneration = 0;
    static void ackWork(thread_call_param_t, thread_call_param_t);
    static void retryWork(thread_call_param_t, thread_call_param_t);
};
#include "methods.inc"

struct Drive : IOService {
    NVMeFixPlugin& plugin = NVMeFixPlugin::globalPlugin();
    NVMeFixPlugin::ControllerEntry& entry = *new NVMeFixPlugin::ControllerEntry(this);
    NVMe::nvme_id_ctrl* id;
    Drive() {
        entry.identify = IOBufferMemoryDescriptor::withCapacity(sizeof(*id), kIODirectionIn);
        id = static_cast<NVMe::nvme_id_ctrl*>(entry.identify->getBytesNoCopy());
        id->apsta = 1;
        id->npss = 5;
        unsigned exit[] = {2500, 6000, 25000}, enter[] = {1500, 10000, 176000};
        for (int i = 0; i < 3; ++i) {
            auto& ps = id->psd[i+3];
            ps.flags = NVMe::NVME_PS_FLAGS_NON_OP_STATE;
            ps.exit_lat = exit[i];
            ps.entry_lat = enter[i];
        }
        entry.processed = true;
        plugin.controllers.push_back(&entry);
    }
    ~Drive() {
        plugin.terminatedNotificationHandler(&plugin, nullptr, this, nullptr);
        drainPending(); // Work still queued for the removed drive releases the proxy.
        assert(references == 1 && !subscribed);
    }
    bool retryPending() { return proxy()->retryCall->pending; }
    uint64_t retryDelay() { return proxy()->retryCall->deadline / kMillisecondScale; }
    void start() { assert(plugin.PM.init(entry, id, plugin.applyAPST(entry, id))); }
    NVMePMProxy* proxy() { return static_cast<NVMePMProxy*>(entry.pm); }
    void wake() { assert(subscribed); proxy()->powerStateDidChangeTo(kIOPMDeviceUsable, 2, this); }
    void tickle() {
        auto& calls = plugin.kextFuncs.IONVMeController.calls;
        auto before = calls;
        assert(!plugin.PM.activityTickle(this, 7, 2) && calls == before + 1);
    }
};

int main() {
    auto& plugin = NVMeFixPlugin::globalPlugin();
    {
        Drive drive;
        drive.start();
        assert(drive.subscribed && drive.published == 1 && !drive.entry.activePM);
        assert(drive.table.entries[0] == (200U << 8 | 3U << 3));
        assert(drive.table.entries[4] == (10050U << 8 | 5U << 3));
        auto acquisitions = plugin.lck->acquisitions;
        drive.tickle();
        assert(acquisitions == plugin.lck->acquisitions && !drive.proxy()->registrations);

        drive.entry.ps_max_latency_us = 2499;
        drive.wake();
        assert(drive.sets == 2 && drive.published == 0 && !drive.enabled && drive.entry.activePM);
        for (auto word : drive.table.entries) assert(word == 0);
        drive.tickle();
        assert(drive.proxy()->tickles == 1);
        drive.proxy()->setPowerState(1, drive.proxy()); // IOKit ordinal 1 maps to NVMe PS2.
        assert(drive.manualPS == 2);
        drive.entry.ps_max_latency_us = 10000;
        drive.rejectPS = true;
        drive.wake();
        assert(drive.enabled && drive.entry.restorePS0 && !drive.entry.activePM);
        assert(!drive.proxy()->timer && !drive.proxy()->deviceRequest && !drive.proxy()->driverRequest);
        drive.rejectPS = false;
        drive.wake();
        assert(drive.manualPS == 0 && !drive.entry.restorePS0 && drive.table.entries[4] == 0);
        drive.proxy()->setPowerState(1, drive.proxy());
        assert(drive.manualPS == 0);

        drive.rejectSet = true;
        drive.wake(); // Rejected Set can leave the previous APST configuration active.
        assert(drive.published == 1 && !drive.entry.activePM);
        drive.rejectGet = true;
        drive.wake();
        assert(drive.published == -1 && !drive.entry.activePM);
        drive.enabled = drive.rejectGet = false;
        drive.proxy()->rejectTimer = true;
        drive.wake();
        assert(drive.published == 0 && !drive.entry.activePM && !drive.proxy()->driverRequest);
        drive.proxy()->rejectTimer = false;
        drive.wake();
        drive.wake();
        assert(drive.entry.activePM && drive.proxy()->registrations == 1 && plugin.activePMControllers == 1);
        {
            Drive second;
            second.rejectSet = true; // Transient failure at boot, firmware APST is off.
            second.start();
            assert(second.subscribed && second.entry.activePM && plugin.activePMControllers == 2);
            second.rejectSet = false;
            second.wake();
            assert(second.enabled && !second.entry.activePM && plugin.activePMControllers == 1);
            second.tickle();
            assert(second.proxy()->tickles == 0);
            second.entry.ps_max_latency_us = 0;
            second.wake();
            assert(second.entry.activePM && plugin.activePMControllers == 2);
        }
        assert(plugin.activePMControllers == 1);
        drive.rejectSet = false;
        drive.wake();
        assert(plugin.activePMControllers == 0);
        acquisitions = plugin.lck->acquisitions;
        drive.tickle();
        assert(acquisitions == plugin.lck->acquisitions);
    }
    {
        Drive drive;
        drive.rejectGet = drive.rejectSet = true;
        drive.start();
        assert(drive.subscribed && drive.published == -1 && !drive.entry.activePM);
        drive.rejectGet = drive.rejectSet = false;
        drive.wake();
        assert(drive.published == 1 && !drive.entry.activePM);
    }
    {
        Drive drive;
        drive.id->apsta = 0;
        drive.start();
        assert(!drive.subscribed && !drive.sets && !drive.gets && drive.entry.activePM);
        IOLockLock(drive.entry.lck);
        std::thread contended([&drive]{ drive.tickle(); });
        contended.join();
        IOLockUnlock(drive.entry.lck);
        assert(drive.proxy()->tickles == 0);
    }
    {
        Drive drive;
        drive.start();
        drive.rejectSet = true;
        drive.enabled = false; // The reset lost APST and reprogramming fails.
        drive.wake();
        assert(drive.published == 0 && drive.entry.activePM && drive.proxy()->registrations == 1);
        drive.rejectGet = true;
        drive.wake();
        assert(drive.published == -1 && !drive.entry.activePM && !drive.proxy()->timer);
        drive.entry.quirks = NVMe::NVME_QUIRK_NO_APST;
        drive.rejectSet = drive.rejectGet = false;
        drive.wake();
        assert(drive.published == 0 && drive.entry.activePM && drive.proxy()->registrations == 1);

        // A tickle already in flight may finish as the registry removes the drive.
        atomic_bool running = false, stop = false;
        std::thread io([&] {
            do {
                drive.tickle();
                atomic_store(&running, true);
            } while (!atomic_load(&stop));
        });
        while (!atomic_load(&running)) std::this_thread::yield();
        plugin.terminatedNotificationHandler(&plugin, nullptr, &drive, nullptr);
        atomic_store(&stop, true);
        io.join();
        assert(plugin.activePMControllers == 0);
        drainPending(); // A queued policy retry holds the proxy until it sees the removal.
        assert(drive.references == 1);
    }
    {
        // A transient Set failure is retried at once; a readback that differs schedules a retry.
        Drive drive;
        drive.failSets = 1;
        drive.start();
        assert(drive.sets == 2 && drive.enabled && drive.requested && drive.entry.apstConfirmed && !drive.retryPending());
        drive.corruptReadback = true;
        drive.wake();
        assert(drive.published == 1 && !drive.entry.apstConfirmed && drive.retryPending() && drive.retryDelay() == 1000);
        drive.corruptReadback = false;
        runPending();
        assert(drive.entry.apstConfirmed && !drive.retryPending() && drive.entry.apstRetries == 0);

        // Retries back off and stop after three attempts for one generation.
        drive.rejectSet = true;
        drive.wake();
        for (uint64_t delay : {1000u, 5000u, 30000u}) {
            assert(drive.retryPending() && drive.retryDelay() == delay);
            runPending();
        }
        assert(!drive.retryPending() && !drive.entry.apstConfirmed);
        drive.rejectSet = false;
        auto sets = drive.sets;
        drive.wake(); // A new generation retries again.
        assert(drive.entry.apstConfirmed && drive.sets == sets + 1 && drive.enabled);

        // Work scheduled for an older generation is dropped.
        drive.proxy()->scheduleRetry(0, drive.entry.generation - 1);
        sets = drive.sets;
        runPending();
        assert(drive.sets == sets);
    }
    {
        // A non-operational state without latencies still waits the 100 ms minimum.
        Drive drive;
        drive.id->psd[3].exit_lat = drive.id->psd[3].entry_lat = 0;
        drive.start();
        assert(drive.table.entries[0] == (100U << 8 | 3U << 3));
        assert(drive.table.entries[3] == (800U << 8 | 4U << 3));
    }
    {
        Drive drive;
        drive.id->apsta = 0;
        drive.start();
        assert(drive.entry.activePM);
        // A contended setPowerState is finished and acknowledged by the worker.
        IOLockLock(drive.entry.lck);
        assert(drive.proxy()->setPowerState(1, drive.proxy()) == NVMeFixPlugin::PM::deferredAckUs);
        // A tickle that found the entry locked is replayed on release.
        std::thread io([&drive] { drive.tickle(); });
        io.join();
        assert(drive.proxy()->tickles == 0 && atomic_load(&drive.entry.tickleMissed));
        plugin.releaseEntry(drive.entry);
        assert(drive.proxy()->tickles == 1 && !atomic_load(&drive.entry.tickleMissed));
        assert(drive.manualPS == 0 && !drive.proxy()->acks);
        runPending();
        assert(drive.manualPS == 2 && drive.proxy()->acks == 1 && drive.entry.lastPS == 2);

        // The power state fell back to PS0 behind the host: a controller reset.
        drive.manualPS = 0;
        auto generation = drive.entry.generation;
        assert(drive.proxy()->setPowerState(2, drive.proxy()) == kIOPMAckImplied);
        assert(drive.entry.generation == generation + 1 && drive.resetGeneration == generation + 1);
        assert(drive.manualPS == 1 && drive.retryPending() && drive.retryDelay() == 0);
        runPending();
        assert(drive.entry.activePM && drive.entry.apstConfirmed);
    }
    {
        // The latency limit follows the power source when an override exists.
        Drive drive;
        drive.entry.ac_latency_us = 10000;
        drive.entry.battery_latency_us = 2499;
        atomic_store(&plugin.onAC, 0);
        drive.start();
        assert(!drive.enabled && drive.entry.activePM);
        atomic_store(&plugin.onAC, 1);
        drive.wake();
        assert(drive.enabled && !drive.entry.activePM && drive.table.entries[4] == 0);
        atomic_store(&plugin.onAC, -1);
        drive.wake();
        assert(drive.enabled && drive.table.entries[4] == (10050U << 8 | 5U << 3));
    }
    {
        // Without the root domain no proxy is created.
        Drive drive;
        rootAvailable = false;
        assert(!plugin.PM.init(drive.entry, drive.id, plugin.applyAPST(drive.entry, drive.id)));
        assert(!drive.entry.pm && !drive.subscribed);
        rootAvailable = true;
    }
    {
        // Without the activityTickle hook active PM would lower the power state under I/O.
        Drive drive;
        drive.id->apsta = 0;
        plugin.tickleHooked = false;
        drive.start();
        assert(!drive.entry.activePM && !drive.proxy()->registrations);
        plugin.tickleHooked = true;
    }
    assert(plugin.activePMControllers == 0 && plugin.controllers.empty() && threadCalls.empty());
}
