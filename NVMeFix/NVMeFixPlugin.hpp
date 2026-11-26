//
// @file NVMeFixPlugin.hpp
//
// NVMeFix
//
// Copyright © 2019 acidanthera. All rights reserved.
//
// This program and the accompanying materials
// are licensed and made available under the terms and conditions of the BSD License
// which accompanies this distribution.  The full text of the license may be found at
// http://opensource.org/licenses/bsd-license.php
// THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
// WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.


#ifndef NVMeFixPlugin_h
#define NVMeFixPlugin_h

#include <stdatomic.h>
#include <stdint.h>

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <kern/thread_call.h>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>

#include "Log.hpp"
#include "nvme.h"
#include "nvme_quirks.hpp"

class NVMeFixPlugin {
public:
	friend class NVMePMProxy;

	void init();
	void deinit();
	static NVMeFixPlugin& globalPlugin();

	explicit NVMeFixPlugin() : PM(*this) {}
private:
	static void processKext(void*, KernelPatcher&, size_t, mach_vm_address_t, size_t);
	static bool matchingNotificationHandler(void*, void*, IOService*, IONotifier*);
	static bool terminatedNotificationHandler(void*, void*, IOService*, IONotifier*);
	bool solveSymbols(KernelPatcher& kp);
	bool validateLayout(IOService* controller);
	bool validateRequestLayout();

	/* Private IONVMeFamily layout: checked against the live class sizes once a controller exists */
	enum Layout : uint8_t { LayoutUnchecked, LayoutValid, LayoutInvalid };
	uint8_t requestLayout {LayoutUnchecked};
	uint8_t controllerLayout {LayoutUnchecked};

	atomic_bool solvedSymbols = false;

	IONotifier* matchingNotifier {nullptr}, * terminationNotifier {nullptr};

	/* Used for synchronising concurrent access to this class from notification handlers */
	IOLock* lck {nullptr};

	const char* kextPath {
		"/System/Library/Extensions/IONVMeFamily.kext/Contents/MacOS/IONVMeFamily"
	};

	KernelPatcher::KextInfo kextInfo {
		"com.apple.iokit.IONVMeFamily",
		&kextPath,
		1,
		{true},
		{},
		KernelPatcher::KextInfo::Unloaded
	};

	enum {
	    ASPM_L0sAndL1EntryDisabled = 0,
		ASPM_L0sEntryEnabled       = 1,
		ASPM_L1EntryEnabled        = 2,
		ASPM_L0sAndL1EntryEnabled  = 3,
		ASPM_Mask = ASPM_L0sAndL1EntryEnabled
	};

	struct {
		template <typename T, typename... Args>
		struct Func {
			const char* const name;
			mach_vm_address_t fptr {};

			bool solve(KernelPatcher& kp, size_t idx) {
				/* Cache the result */
				if (!fptr) {
					fptr = kp.solveSymbol(idx, name);
					DBGLOG_COND(fptr == 0, Log::Plugin, "Failed to solve %s", name);
				}
				return fptr;
			}

			bool route(KernelPatcher& kp, size_t idx, T(*repl)(Args...)) {
				if (!solve(kp, idx))
					return false;
				fptr = kp.routeFunction(fptr, reinterpret_cast<mach_vm_address_t>(repl), true);
				return fptr;
			}

			bool routeVirtual(KernelPatcher& kp, size_t idx, const char* vtFor, size_t offs, T(*repl)(Args...)) {
				using PtrTy = T(**)(Args...);
				assert(vtFor);
				auto vt = kp.solveSymbol(idx, vtFor);
				return vt && KernelPatcher::routeVirtual(&vt, offs, repl, reinterpret_cast<PtrTy>(&fptr));
			}

			T operator()(Args... args) const {
				assertf(fptr, "%s not solved", name ? name : "(unknown)");
				return (*reinterpret_cast<T(*)(Args...)>(fptr))(static_cast<Args&&>(args)...);
			}
		};

		struct {
			Func<IOReturn,void*,IOMemoryDescriptor*,void*,uint64_t> IssueIdentifyCommand {
				"__ZN16IONVMeController20IssueIdentifyCommandEP18IOMemoryDescriptorP16AppleNVMeRequestj"
			};

			Func<IOReturn,void*,IOMemoryDescriptor*,unsigned int, bool> IssueIdentifyCommandNew {
				"__ZN16IONVMeController20IssueIdentifyCommandEP18IOMemoryDescriptorjb"
			};

			Func<IOReturn,void*,void*> ProcessSyncNVMeRequest {
				"__ZN16IONVMeController22ProcessSyncNVMeRequestEP16AppleNVMeRequest"
			};

			Func<void*,void*,uint32_t> GetRequest {
				"__ZN16IONVMeController10GetRequestEj"
			};
			Func<void*,void*,uint32_t, uint8_t> GetRequestNew {
				"__ZN16IONVMeController10GetRequestEjh"
			};
			Func<void,void*,void*> ReturnRequest {
				"__ZN16IONVMeController13ReturnRequestEP16AppleNVMeRequest"
			};
			Func<bool,void*,unsigned long, unsigned long> activityTickle {};
			Func<void,void*,void*,int> FilterInterruptRequest {
				"__ZN16IONVMeController22FilterInterruptRequestEP28IOFilterInterruptEventSource"
			};
		} IONVMeController;

		struct {
			Func<void,void*,uint8_t> BuildCommandGetFeatures {
				"__ZN16AppleNVMeRequest23BuildCommandGetFeaturesEh"
			};

			Func<void,void*,uint8_t> BuildCommandSetFeaturesCommon {
				"__ZN16AppleNVMeRequest29BuildCommandSetFeaturesCommonEh"
			};

			Func<uint32_t,void*> GetStatus {
				"__ZN16AppleNVMeRequest9GetStatusEv"
			};

			Func<uint32_t,void*> GetOpcode {
				"__ZN16AppleNVMeRequest9GetOpcodeEv"
			};

			Func<IOReturn,void*,uint64_t,uint64_t> GenerateIOVMSegments {
				"__ZN16AppleNVMeRequest20GenerateIOVMSegmentsEyy"
			};
		} AppleNVMeRequest;
	} kextFuncs;
	
	/* No IONVMeFamily object member is expected beyond this offset */
	static constexpr int64_t MaxMemberOffset {0x4000};

	struct {
		template <typename T>
		struct Member {
#ifdef DEBUG
#define NAMED_MEMBER(n) n {.name = #n}
#else
#define NAMED_MEMBER(n) n
#endif

#ifdef DEBUG
			const char* const name {};
#endif
			mach_vm_address_t offs {};
			T& get(void* obj) {
				assert(offs);
				assert(obj);

				return getMember<T>(obj, offs);
			}

			bool has() {
				return offs != 0;
			}

			bool fromFunc(mach_vm_address_t start, uint32_t opcode, uint32_t reg, uint32_t rm,
						  uint32_t add=0, size_t ninsts_max=128) {
				if (offs)
					return true;

				if (!start) {
					DBGLOG(Log::Disasm, "No start specified for %s", name);
					return false;
				}
				hde64s dis;

				for (size_t i = 0; i < ninsts_max; i++) {
					auto sz = Disassembler::hdeDisasm(start, &dis);

					if (dis.flags & F_ERROR) {
						DBGLOG(Log::Disasm, "Error disassembling %s", name);
						break;
					}

					/* mov reg, [reg+disp] */
					if (dis.opcode == opcode && dis.modrm_reg == reg && dis.modrm_rm == (rm & 7) && dis.rex_b == ((rm & 8) >> 3)) {
						/* Only a register base plus a positive displacement describes a member offset */
						int64_t disp = -1;
						if ((dis.flags & F_MODRM) && dis.modrm_mod == 2 && (dis.flags & F_DISP32))
							disp = static_cast<int32_t>(dis.disp.disp32);
						else if ((dis.flags & F_MODRM) && dis.modrm_mod == 1 && (dis.flags & F_DISP8))
							disp = static_cast<int8_t>(dis.disp.disp8);
						if (disp <= 0 || disp + add > MaxMemberOffset) {
							SYSLOG(Log::Disasm, "Unexpected member operand");
							return false;
						}
						offs = static_cast<mach_vm_address_t>(disp + add);
						DBGLOG(Log::Disasm, "Offset 0x%x for %s", offs, name);
						return true;
					}

					start += sz;
				}
				
				DBGLOG(Log::Disasm, "Failed to find %s", name);
				return false;
			};
		};

		struct {
			Member<uint8_t> NAMED_MEMBER(ANS2MSIWorkaround);
		} IONVMeController;

		struct {
			Member<uint32_t> NAMED_MEMBER(result);
			Member<void*> NAMED_MEMBER(controller);
			Member<NVMe::nvme_command> NAMED_MEMBER(command);
			Member<IOBufferMemoryDescriptor*> NAMED_MEMBER(prpDescriptor);
		} AppleNVMeRequest;
#undef NAMED_MEMBER
	} kextMembers;

	/*
	 * How far should we traverse ioreg searching for parent NVMe controller
	 * Typical depth is 9 on real setups
	 */
	static constexpr int controllerSearchDepth {20};


	enum class APSTState { Unknown, Disabled, Enabled };

	/* Minimum APST idle time before a transition (Linux' default primary APST timeout) */
	static constexpr uint64_t minIdleTimeMs {100};

	struct ControllerEntry {
		IOService* controller {nullptr};
		/* Optional ps-max-latency-us-ac / ps-max-latency-us-battery overrides, UINT64_MAX when unset */
		uint64_t ac_latency_us {UINT64_MAX};
		uint64_t battery_latency_us {UINT64_MAX};
		/*
		 * Incremented whenever the controller may have lost its NVMe power settings: a power
		 * state change of the controller, or a power state that changed without the host.
		 * Deferred work started for an older generation is dropped. Protected by the entry lock.
		 */
		uint32_t generation {0};
		/* The last APST configuration was written and read back as requested */
		bool apstConfirmed {false};
		unsigned apstRetries {0};
		/* Last power state set or observed with active PM, 0xFF when unknown */
		uint8_t lastPS {0xFF};
		/* An activity tickle found the entry locked; replayed when the lock is released */
		atomic_bool tickleMissed;
		bool processed {false}; // Protected by the registry lock.
		bool retryIdentify {false}; // Retry on the next matching media notification.
		bool removed {false}; // Protected by the entry lock.
		NVMe::nvme_quirks quirks {NVMe::NVME_QUIRK_NONE};
		uint64_t ps_max_latency_us {100000};
		IOPMPowerState* powerStates {nullptr};
		size_t nstates {0};
		uint8_t operationalPS[32] {}; // Indexed by nonzero IOKit ordinal minus one.
		IOLock* lck {nullptr};
		IOService* pm {nullptr};
		IOBufferMemoryDescriptor* identify {nullptr};
		bool activePM {false}; // Protected by the entry lock, as is the last manual power setting.
		bool restorePS0 {false};

		explicit ControllerEntry(IOService* c) : controller(c), lck(IOLockAlloc()) {
			atomic_init(&tickleMissed, false);
			controller->retain();
		}

		~ControllerEntry() {
			if (powerStates)
				delete[] powerStates;
			if (identify)
				identify->release();
			if (lck)
				IOLockFree(lck);
			controller->release();
		}
	};

	evector<ControllerEntry*> controllers;
	atomic_uint activePMControllers = 0;
	/* IONVMeController::activityTickle is routed; active PM needs it to see I/O */
	bool tickleHooked {false};
	/* Power source for the AC/battery latency overrides: -1 unknown, 0 battery, 1 AC */
	atomic_int onAC = -1;
	atomic_bool powerSourceWatch = false;
	IONotifier* powerSourceNotifier {nullptr};
	IONotifier* powerSourceInterest {nullptr};
	static bool powerSourcePublished(void*, void*, IOService*, IONotifier*);
	static IOReturn powerSourceChanged(void*, void*, UInt32, IOService*, void*, vm_size_t);
	void watchPowerSource();
	void powerSourceUpdated();
	void setActivePM(ControllerEntry&, bool);
	void releaseEntry(ControllerEntry&);
	void handleControllers();
	void forceEnableASPM(IOService*);
	void reportLinkPM(IOService*);
	void handleController(ControllerEntry&);
	IOReturn identify(ControllerEntry&,IOBufferMemoryDescriptor*&);
	uint64_t latencyLimit(const ControllerEntry&) const;
	bool apstAllowed(const ControllerEntry&) const;
	APSTState applyAPST(ControllerEntry&, const NVMe::nvme_id_ctrl*);
	IOReturn configureAPST(ControllerEntry&, const NVMe::nvme_id_ctrl*, bool& enabled, IOBufferMemoryDescriptor*);
	IOReturn verifyAPST(ControllerEntry&, const NVMe::nvme_id_ctrl*, const IOBufferMemoryDescriptor*, bool& enabled, bool& matches);
	IOReturn APSTenabled(ControllerEntry&, bool&);
	IOReturn dumpAPST(ControllerEntry&, int npss);
	IOReturn NVMeFeatures(ControllerEntry&, unsigned fid, unsigned* dword11, IOBufferMemoryDescriptor* desc,
							 uint32_t* res, bool set);
	ControllerEntry* entryForController(IOService*) const;
	struct PM {
		bool init(ControllerEntry&, const NVMe::nvme_id_ctrl*, APSTState);
		bool updatePolicy(ControllerEntry&, const NVMe::nvme_id_ctrl*, APSTState);
		bool settle(ControllerEntry&, const NVMe::nvme_id_ctrl*, APSTState);
		bool refresh(ControllerEntry&, const NVMe::nvme_id_ctrl*);
		void resetDetected(ControllerEntry&);
		bool solveSymbols(KernelPatcher&);

		/* APST retries after a failed or unconfirmed configuration, per generation: 1 s, 5 s, 30 s */
		static constexpr unsigned maxRetries {3};
		static uint32_t retryDelayMs(unsigned attempt) {
			return attempt == 0 ? 1000 : attempt == 1 ? 5000 : 30000;
		}
		/* How long IOKit waits for a setPowerState finished by the worker */
		static constexpr IOReturn deferredAckUs {2000000};

		static constexpr unsigned idlePeriod {2}; /* seconds */
		/* activityTickle slot (counting the two vtable header words) of macOS 10.14 to 26 */
		static constexpr size_t legacyTickleSlot {249};
		static constexpr size_t maxVtableSlots {1024};

		static bool activityTickle(void*,unsigned long,unsigned long);

		explicit PM(NVMeFixPlugin& plugin) : plugin(plugin) {}
	private:
		NVMeFixPlugin& plugin;
		
		bool registerPowerStates(ControllerEntry&, const NVMe::nvme_id_ctrl*);
	} PM;
};

class NVMePMProxy : public IOService {
	OSDeclareDefaultStructors(NVMePMProxy)
public:
	// NVMe power management
	virtual IOReturn setPowerState(
		unsigned long powerStateOrdinal,
		IOService *   whatDevice ) override;
	
	// Monitoring IONVMeController power state to re-enable APST
	virtual IOReturn powerStateDidChangeTo(
		IOPMPowerFlags  capabilities,
		unsigned long   stateNumber,
		IOService *     whatDevice ) override;

	// A successfully initialized proxy owns the entry until its final IOKit callback returns.
	NVMeFixPlugin::ControllerEntry* entry {nullptr};
	void free() override;

	// Deferred work. Each pending call holds a reference to the proxy, and so to the entry.
	bool initWork();
	bool scheduleAck(unsigned long ordinal);
	bool scheduleRetry(uint32_t delayMs, uint32_t generation);
	void applyPowerState(unsigned long ordinal);
private:
	thread_call_t ackCall {nullptr}, retryCall {nullptr};
	unsigned long pendingOrdinal {0};
	uint32_t retryGeneration {0};
	static void ackWork(thread_call_param_t, thread_call_param_t);
	static void retryWork(thread_call_param_t, thread_call_param_t);
};

#endif /* NVMeFixPlugin_h */
