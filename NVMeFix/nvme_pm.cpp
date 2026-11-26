//
// @file nvme_pm.cpp
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

#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <kern/assert.h>

#include "Log.hpp"
#include "NVMeFixPlugin.hpp"

/**
* For Apple Controllers, AppleNVMeController toggles self-refresh for low-power states, and
* completely ignores PCI PM. For generic controllers, IONVMeController uses PCI PM and ignores
* NVMe Power Management features.
* We implement active power management by attaching our own IOService to PM root and registering
* the operational power states of the controller. We intercept activityTickle method of the relevant
* IONVMeController to tickle our service, and use NVMe power management feature to set the
* corresponding state.
* Our PM should operate transparently w.r.t PCI link PM and APST. APST is still useful, because typical
* idle intervals for APST transitions are in order of hundreds of milliseconds, while IOPM has seconds
* resolution; PCI link power management is still used by IONVMeController.
* As we never transition to idle states, we need not freeze the command queue, so we don't have to
* touch the internal state of IONVMe.
*/
bool NVMeFixPlugin::PM::init(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl, APSTState state) {
	// Without the root domain the proxy cannot join the power tree: no IOKit power state would ever reach it.
	auto root = IOService::getPMRootDomain();
	if (!root) {
		SYSLOG(Log::PM, "PM root domain is unavailable");
		return false;
	}

	auto proxy = new NVMePMProxy();
	if (!proxy)
		return false;
	if (!proxy->init() || !proxy->initWork()) {
		DBGLOG(Log::PM, "Failed to init IOService");
		proxy->release();
		return false;
	}
	proxy->entry = &entry;
	__atomic_store_n(&entry.pm, proxy, __ATOMIC_RELEASE); // Read without the entry lock on a power source change

	entry.pm->PMinit();
	reinterpret_cast<IOService*>(root)->joinPMtree(entry.pm);

	// A failed Set Features at boot still needs a retry when the controller wakes, and the link
	// power settings of -nvmefl1ss may not survive a power transition.
	if ((ctrl->apsta & 1) || plugin.l1ssRequested)
		entry.controller->registerInterestedDriver(entry.pm);

	return settle(entry, ctrl, state);
}

// The caller holds the entry lock. The count is only a fast negative lookup:
// the registry and entry locks still protect lifetimes on the slow path.
void NVMeFixPlugin::setActivePM(ControllerEntry& entry, bool active) {
	if (entry.activePM == active)
		return;
	entry.activePM = active;
	if (active)
		atomic_fetch_add_explicit(&activePMControllers, 1, memory_order_release);
	else
		atomic_fetch_sub_explicit(&activePMControllers, 1, memory_order_release);
}

// Unlock the entry and deliver an activity tickle that found it locked, so active PM does not
// lower the power state under I/O that arrived meanwhile.
void NVMeFixPlugin::releaseEntry(ControllerEntry& entry) {
	if (atomic_exchange_explicit(&entry.tickleMissed, false, memory_order_acq_rel) &&
		entry.activePM && entry.pm && !entry.removed)
		entry.pm->activityTickle(kIOPMSuperclassPolicy1, entry.nstates - 1);
	IOLockUnlock(entry.lck);
}

bool NVMeFixPlugin::PM::updatePolicy(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl, APSTState state) {
	bool ok = true;
	// Active PM lowers the power state on idle, so it needs the activity tickles to see I/O.
	if (state == APSTState::Disabled && !plugin.tickleHooked) {
		DBGLOG(Log::PM, "Active PM needs the activityTickle hook");
		state = APSTState::Unknown;
	}
	if (state == APSTState::Disabled) {
		if (entry.activePM)
			return true;
		if (!entry.powerStates && !registerPowerStates(entry, ctrl))
			return false;
		if (entry.pm->makeUsable() == kIOReturnSuccess &&
			entry.pm->changePowerStateTo(1) == kIOReturnSuccess &&
			entry.pm->setIdleTimerPeriod(idlePeriod) == kIOReturnSuccess) {
			plugin.setActivePM(entry, true);
			return true;
		}
		ok = false; // Release any IOKit requests accepted before the failure.
	}

	plugin.setActivePM(entry, false);
	if (entry.powerStates) {
		// Drop the timer and both IOKit power requests left by the manual policy.
		ok &= entry.pm->setIdleTimerPeriod(0) == kIOReturnSuccess;
		ok &= entry.pm->changePowerStateTo(0) == kIOReturnSuccess;
		ok &= entry.pm->changePowerStateToPriv(0) == kIOReturnSuccess;
	}
	if (state == APSTState::Enabled && entry.restorePS0) {
		unsigned ps = 0;
		if (plugin.NVMeFeatures(entry, NVMe::NVME_FEAT_POWER_MGMT, &ps, nullptr, nullptr, true) != kIOReturnSuccess)
			return false;
		entry.restorePS0 = false;
		entry.lastPS = 0;
	}
	return ok;
}

// Apply the policy for the reported APST state and retry while the configuration is not confirmed.
// The caller holds the entry lock.
bool NVMeFixPlugin::PM::settle(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl, APSTState state) {
	bool ok = updatePolicy(entry, ctrl, state);
	if (ok && entry.apstConfirmed && state != APSTState::Unknown) {
		entry.apstRetries = 0;
		return true;
	}
	auto proxy = static_cast<NVMePMProxy*>(entry.pm);
	if (proxy && entry.apstRetries < maxRetries) {
		auto delay = retryDelayMs(entry.apstRetries++);
		DBGLOG(Log::APST, "Retrying the power policy in %u ms", delay);
		proxy->scheduleRetry(delay, entry.generation);
	} else if (proxy) {
		SYSLOG(Log::APST, "Power policy not confirmed after %u retries", maxRetries);
	}
	return ok;
}

bool NVMeFixPlugin::PM::refresh(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl) {
	return settle(entry, ctrl, plugin.applyAPST(entry, ctrl));
}

// The controller lost its power settings without a power transition we saw, e.g. a controller reset.
// The caller holds the entry lock.
void NVMeFixPlugin::PM::resetDetected(ControllerEntry& entry) {
	__atomic_add_fetch(&entry.generation, 1, __ATOMIC_ACQ_REL);
	entry.apstRetries = 0;
	entry.restorePS0 = false;
	entry.controller->setProperty("reset-generation", entry.generation, 32);
	SYSLOG(Log::PM, "Controller lost its power settings, generation %u", entry.generation);
	// The reset may also have restored the firmware's APST policy: configure it again from the worker.
	if (entry.pm)
		static_cast<NVMePMProxy*>(entry.pm)->scheduleRetry(0, entry.generation);
}

bool NVMeFixPlugin::PM::registerPowerStates(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl) {
	unsigned op {};
	
	for (int state = ctrl->npss; state >= 0; state--)
		if (!(ctrl->psd[state].flags & NVMe::NVME_PS_FLAGS_NON_OP_STATE))
			op++;

	entry.nstates = op + 1; // IOPM state zero precedes the operational states.
	entry.powerStates = new IOPMPowerState[entry.nstates];
	if (!entry.powerStates) {
		SYSLOG(Log::PM, "Failed to allocate power state buffer");
		return false;
	}

	if (op <= 1)
		SYSLOG(Log::PM, "Controller declares too few operational power states");
	DBGLOG(Log::PM, "npss 0x%x", ctrl->npss);
	
	entry.powerStates[0] = {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	/**
	 * Linux has a different model: they save state upon suspend and switch to
	 * npss state; upon resume, they restore last state. They also reset ps
	 * when they fail to set or get it, but it's unclear how that would ever
	 * occur given that NVMe 1.4 spec 5.21.1.2 only mentions error when trying
	 * to set unsupported state.
	 */
	size_t idx {1};
	for (int state = ctrl->npss; state >= 0; state--) {
		if (ctrl->psd[state].flags & NVMe::NVME_PS_FLAGS_NON_OP_STATE)
			continue;

		entry.operationalPS[idx - 1] = static_cast<uint8_t>(state);
		auto& ps = entry.powerStates[idx];
		ps = {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		/**
		 * We shouldn't have any IOPM clients that require power, so don't
		 * set outputPowerCharacter.
		 */
//		ps.outputPowerCharacter = kIOPMPowerOn;
		ps.inputPowerRequirement = kIOPMPowerOn;

		/* In non-operational state device is not usable and we may sleep */
		ps.capabilityFlags |= kIOPMPreventIdleSleep;
		ps.capabilityFlags |= kIOPMDeviceUsable;
		DBGLOG(Log::PM, "Setting ps %u capabilityFlags 0x%x", idx, ps.capabilityFlags);
		idx++;
	}

	assert(idx == entry.nstates);
	DBGLOG(Log::PM, "Publishing %u states", entry.nstates);
	
	auto status = entry.pm->registerPowerDriver(entry.pm, entry.powerStates, entry.nstates);
	if (status != kIOReturnSuccess) {
		SYSLOG(Log::PM, "registerPowerDriver failed with 0x%x", status);
		delete[] entry.powerStates;
		entry.powerStates = nullptr;
		return false;
	}
	return true;
}

OSDefineMetaClassAndStructors(NVMePMProxy, IOService);

void NVMePMProxy::free() {
	assert(!entry || entry->removed);
	// A pending call holds a reference, so none is pending here.
	if (ackCall)
		thread_call_free(ackCall);
	if (retryCall)
		thread_call_free(retryCall);
	delete entry;
	IOService::free();
}

bool NVMePMProxy::initWork() {
	ackCall = thread_call_allocate(ackWork, this);
	retryCall = thread_call_allocate(retryWork, this);
	return ackCall && retryCall;
}

bool NVMePMProxy::scheduleAck(unsigned long ordinal) {
	__atomic_store_n(&pendingOrdinal, ordinal, __ATOMIC_RELEASE);
	retain();
	if (thread_call_enter(ackCall))
		release(); // Already pending: that call holds the reference
	return true;
}

bool NVMePMProxy::scheduleRetry(uint32_t delayMs, uint32_t generation) {
	__atomic_store_n(&retryGeneration, generation, __ATOMIC_RELEASE);
	retain();
	bool pending;
	if (delayMs) {
		uint64_t deadline;
		clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
		pending = thread_call_enter_delayed(retryCall, deadline);
	} else {
		pending = thread_call_enter(retryCall);
	}
	if (pending)
		release();
	return true;
}

// The entry lock is held.
void NVMePMProxy::applyPowerState(unsigned long powerStateOrdinal) {
	auto& plugin = NVMeFixPlugin::globalPlugin();
	if (entry->removed || !entry->activePM || powerStateOrdinal >= entry->nstates)
		return;
	unsigned dword11 = entry->operationalPS[powerStateOrdinal - 1];
	auto ctrl = static_cast<const NVMe::nvme_id_ctrl*>(entry->identify->getBytesNoCopy());
	uint32_t res {};
	auto ret = plugin.NVMeFeatures(*entry, NVMe::NVME_FEAT_POWER_MGMT, nullptr, nullptr, &res,
		false);
	res &= 0x1F;

	DBGLOG(Log::PM, "Current ps 0x%x, proposed 0x%x", res, dword11);

	if (ret != kIOReturnSuccess) {
		SYSLOG(Log::PM, "Failed to get power state");
		entry->lastPS = 0xFF;
		return;
	}
	// With APST off only the host changes the power state: a different one means the controller was reset.
	if (entry->lastPS != 0xFF && res != entry->lastPS)
		plugin.PM.resetDetected(*entry);
	entry->lastPS = static_cast<uint8_t>(res);
	if (res != dword11 && ctrl && res <= ctrl->npss && !(ctrl->psd[res].flags & NVMe::NVME_PS_FLAGS_NON_OP_STATE)) {
		DBGLOG(Log::PM, "Setting power state 0x%x", dword11);

		ret = plugin.NVMeFeatures(*entry, NVMe::NVME_FEAT_POWER_MGMT, &dword11, nullptr, nullptr,
								   true);
		if (ret != kIOReturnSuccess) {
			SYSLOG(Log::PM, "Failed to set power state");
			entry->lastPS = 0xFF;
		} else {
			entry->restorePS0 = dword11 != 0;
			entry->lastPS = static_cast<uint8_t>(dword11);
		}
	}
}

IOReturn NVMePMProxy::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) {
	DBGLOG(Log::PM, "setPowerState %lu", powerStateOrdinal);

	if (powerStateOrdinal == 0)
		return kIOPMAckImplied;

	auto& plugin = NVMeFixPlugin::globalPlugin();

	if (IOLockTryLock(entry->lck)) {
		applyPowerState(powerStateOrdinal);
		plugin.releaseEntry(*entry);
		/**
		 * FIXME: We should return entry + exit + switching overhead latency here, but at least in my tests
		 * it is always 0.
		 */
		return kIOPMAckImplied;
	}

	/**
	 * The entry is busy (setup or a policy update). The power management thread must not block on it,
	 * but acknowledging now would tell IOKit the state changed while the controller stayed where it
	 * was. Finish on the worker and acknowledge from there.
	 */
	DBGLOG(Log::PM, "Entry busy, deferring power state %lu", powerStateOrdinal);
	scheduleAck(powerStateOrdinal);
	return NVMeFixPlugin::PM::deferredAckUs;
}

void NVMePMProxy::ackWork(thread_call_param_t param0, thread_call_param_t) {
	auto proxy = static_cast<NVMePMProxy*>(param0);
	auto& plugin = NVMeFixPlugin::globalPlugin();
	auto entry = proxy->entry;
	IOLockLock(entry->lck);
	proxy->applyPowerState(__atomic_load_n(&proxy->pendingOrdinal, __ATOMIC_ACQUIRE));
	bool removed = entry->removed;
	plugin.releaseEntry(*entry);
	// After removal PMstop has dropped the pending change, there is nothing to acknowledge.
	if (!removed)
		proxy->acknowledgeSetPowerState();
	proxy->release();
}

void NVMePMProxy::retryWork(thread_call_param_t param0, thread_call_param_t) {
	auto proxy = static_cast<NVMePMProxy*>(param0);
	auto& plugin = NVMeFixPlugin::globalPlugin();
	auto entry = proxy->entry;
	IOLockLock(entry->lck);
	auto identify = entry->identify ?
		static_cast<const NVMe::nvme_id_ctrl*>(entry->identify->getBytesNoCopy()) : nullptr;
	// Work scheduled for an earlier generation is superseded by the policy applied for the new one.
	if (!entry->removed && identify &&
		__atomic_load_n(&proxy->retryGeneration, __ATOMIC_ACQUIRE) == entry->generation &&
		!plugin.PM.refresh(*entry, identify))
		SYSLOG(Log::PM, "Failed to restore power policy");
	plugin.releaseEntry(*entry);
	proxy->release();
}

IOReturn NVMePMProxy::powerStateDidChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber,
											IOService *whatDevice) {
	DBGLOG(Log::PM, "powerStateDidChangeTo 0x%x", stateNumber);

	/* FIXME: Should we ignore PAUSE->ACTIVE transition? */
	if (!(capabilities & kIOPMDeviceUsable)) {
		DBGLOG(Log::PM, "Ignoring transition to non-usable state 0x%x", stateNumber);
		return kIOPMAckImplied;
	}

	auto& plugin = NVMeFixPlugin::globalPlugin();

	/* We only get once chance after wake, so we insist on getting to critical section */
	IOLockLock(entry->lck);
	NVMe::nvme_id_ctrl* identify {};

	if (entry->removed || entry->controller != whatDevice) {
		DBGLOG(Log::PM, "Power state change for irrelevant device %s",
			   whatDevice->getMetaClass()->getClassName());
		goto done;
	}

	// A power transition may have reset every NVMe power setting: start a new generation.
	__atomic_add_fetch(&entry->generation, 1, __ATOMIC_ACQ_REL);
	entry->apstRetries = 0;
	entry->lastPS = 0xFF;
	plugin.reapplyLinkPM(*entry);

	assert(entry->identify);
	identify = static_cast<decltype(identify)>(entry->identify->getBytesNoCopy());
	if (!identify) {
		DBGLOG(Log::PM, "Failed to get identify bytes");
		goto done;
	} else if (!plugin.PM.refresh(*entry, identify)) {
		SYSLOG(Log::PM, "Failed to restore power policy");
	}

done:
	plugin.releaseEntry(*entry);

	return IOPMAckImplied;
}

bool NVMeFixPlugin::PM::solveSymbols(KernelPatcher& kp) {
	auto idx = plugin.kextInfo.loadIndex;
	/**
	 * Find activityTickle in IOService's own vtable instead of trusting a fixed slot, and only
	 * replace IONVMeController's slot when it still holds IOService's implementation or
	 * IONVMeController's own override. Anything else means the layout changed.
	 */
	auto serviceVt = reinterpret_cast<const mach_vm_address_t*>(kp.solveSymbol(KernelPatcher::KernelID, "__ZTV9IOService"));
	auto tickle = kp.solveSymbol(KernelPatcher::KernelID, "__ZN9IOService14activityTickleEmm");
	auto controllerVt = reinterpret_cast<const mach_vm_address_t*>(kp.solveSymbol(idx, "__ZTV16IONVMeController"));
	auto override = kp.solveSymbol(idx, "__ZN16IONVMeController14activityTickleEmm");
	kp.clearError();
	if (!controllerVt || (!tickle && !override)) {
		SYSLOG(Log::PM, "Failed to solve activityTickle, active PM disabled");
		return false;
	}

	auto expected = [&](size_t slot) {
		auto fn = controllerVt[slot];
		return fn && (fn == tickle || fn == override);
	};
	size_t slot = 0;
	// Two header words (offset to top, type info) precede the virtual functions.
	if (serviceVt && tickle)
		for (size_t i = 2; i < maxVtableSlots && !slot; i++)
			if (serviceVt[i] == tickle)
				slot = i;
	if (!slot && expected(legacyTickleSlot))
		slot = legacyTickleSlot;
	if (!slot || !expected(slot)) {
		SYSLOG(Log::PM, "Unexpected IONVMeController vtable, active PM disabled");
		return false;
	}
	DBGLOG(Log::PM, "activityTickle at vtable slot %u", static_cast<unsigned>(slot));

	return plugin.kextFuncs.IONVMeController.activityTickle.routeVirtual(kp, idx,
													"__ZTV16IONVMeController", slot, activityTickle);
}

bool NVMeFixPlugin::PM::activityTickle(void* controller, unsigned long type, unsigned long stateNumber) {
	auto& plugin = NVMeFixPlugin::globalPlugin();

	if (atomic_load_explicit(&plugin.activePMControllers, memory_order_acquire) != 0) {
		IOLockLock(plugin.lck);
		auto entry = plugin.entryForController(static_cast<IOService*>(controller));
		if (entry && entry->processed && !IOLockTryLock(entry->lck)) {
			// The lock holder replays it when releasing the entry.
			atomic_store_explicit(&entry->tickleMissed, true, memory_order_release);
			entry = nullptr;
		} else if (entry && !entry->processed) {
			entry = nullptr;
		}
		IOLockUnlock(plugin.lck);

		if (entry) {
			if (entry->activePM)
				entry->pm->activityTickle(kIOPMSuperclassPolicy1, entry->nstates - 1);
			IOLockUnlock(entry->lck);
		}
	}

	return plugin.kextFuncs.IONVMeController.activityTickle(controller, type, stateNumber);
}
