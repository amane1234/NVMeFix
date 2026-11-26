//
// @file NVMeFix.cpp
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
#include <IOKit/IODMACommand.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <kern/assert.h>
#include <libkern/c++/OSMetaClass.h>

#include <Headers/kern_api.hpp>
#include <Headers/kern_disasm.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/hde64.h>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include "Log.hpp"
#include "NVMeFixPlugin.hpp"
#include "pcie_l1ss.hpp"

static NVMeFixPlugin plugin;

NVMeFixPlugin& NVMeFixPlugin::globalPlugin() {
	return plugin;
}

/**
 * This may be invoked before or after we get IOBSD mount notification, so in the both functions we
 * attempt to solve symbols and handle the controllers.
 */
void NVMeFixPlugin::processKext(void* that, KernelPatcher& patcher, size_t index, mach_vm_address_t,
								size_t) {
	auto plugin = static_cast<NVMeFixPlugin*>(that);
	assert(plugin);

	if (index != plugin->kextInfo.loadIndex)
		return;

	DBGLOG(Log::Plugin, "processKext %s", plugin->kextInfo.id);

	if (plugin->solveSymbols(patcher)) {
		atomic_store_explicit(&plugin->solvedSymbols, true, memory_order_release);
		plugin->handleControllers();
	}
}

bool NVMeFixPlugin::solveSymbols(KernelPatcher& kp) {
	auto idx = plugin.kextInfo.loadIndex;
	bool res = true;
	res &= (kextFuncs.IONVMeController.IssueIdentifyCommandNew.solve(kp, idx) ||
			kextFuncs.IONVMeController.IssueIdentifyCommand.solve(kp, idx)) &&
	kextFuncs.IONVMeController.ProcessSyncNVMeRequest.solve(kp, idx) &&
	(kextFuncs.IONVMeController.GetRequestNew.solve(kp, idx) ||
	 kextFuncs.IONVMeController.GetRequest.solve(kp, idx)) &&
	kextFuncs.AppleNVMeRequest.BuildCommandGetFeatures.solve(kp, idx) &&
	kextFuncs.AppleNVMeRequest.BuildCommandSetFeaturesCommon.solve(kp, idx) &&
	kextFuncs.IONVMeController.ReturnRequest.solve(kp, idx) &&
	kextFuncs.AppleNVMeRequest.GetStatus.solve(kp, idx) &&
	kextFuncs.AppleNVMeRequest.GetOpcode.solve(kp, idx) &&
	kextFuncs.AppleNVMeRequest.GenerateIOVMSegments.solve(kp, idx) &&
	kextFuncs.IONVMeController.FilterInterruptRequest.solve(kp, idx);

	/* mov eax, [rdi+0xA8] */
	res &= kextMembers.AppleNVMeRequest.result.fromFunc(kextFuncs.AppleNVMeRequest.GetStatus.fptr,
														 0x8b, 0, 7, 4) &&
	/* movzx eax, byte ptr [rdi+0x10A] */
		kextMembers.AppleNVMeRequest.command.fromFunc(kextFuncs.AppleNVMeRequest.GetOpcode.fptr,
												  0xf, 0, 7) &&
	/* mov [r14+0xC0], r15 (14.0+) or mov [rbx+0xC0], r15 (11.3-13.x) or mov [rbx+0xC0], r12 (<=11.2) */
		(kextFuncs.IONVMeController.IssueIdentifyCommandNew.fptr ?
		kextMembers.AppleNVMeRequest.prpDescriptor.fromFunc(kextFuncs.IONVMeController.IssueIdentifyCommandNew.fptr, 0x89, 7, getKernelVersion() >= KernelVersion::Sonoma ? 14 : 3) :
		 kextMembers.AppleNVMeRequest.prpDescriptor.fromFunc(kextFuncs.IONVMeController.IssueIdentifyCommand.fptr, 0x89, 4, 3));

	/* cmp byte ptr [rdi+269h], 0 */
	kextMembers.IONVMeController.ANS2MSIWorkaround.fromFunc(kextFuncs.IONVMeController.FilterInterruptRequest.fptr,
																0x80, 7, 7, 0, 32);

	/* The controller pointer is the 8-byte field that ends 4 bytes before the result */
	if (res && kextMembers.AppleNVMeRequest.result.offs >= 12 + sizeof(void*))
		kextMembers.AppleNVMeRequest.controller.offs = kextMembers.AppleNVMeRequest.result.offs - 12;
	else
		res = false;

	if (res && !validateRequestLayout()) {
		SYSLOG(Log::Plugin, "Unexpected AppleNVMeRequest layout, NVMeFix disabled");
		requestLayout = LayoutInvalid;
		res = false;
	}

	if (res) {
		// Without the tickle hook only active PM is unavailable; APST still works.
		tickleHooked = PM.solveSymbols(kp);
	}
	if (!res)
		SYSLOG(Log::Plugin, "Failed to solve symbols");
	return res;
}

/* Offsets found by disassembly must describe distinct, aligned members of one object */
bool NVMeFixPlugin::validateRequestLayout() {
	auto& m = kextMembers.AppleNVMeRequest;
	struct Range { mach_vm_address_t offs; size_t size, align; } ranges[] {
		// The command may sit in a packed part of the request: only pointers must be aligned.
		{m.result.offs, sizeof(uint32_t), 1},
		{m.controller.offs, sizeof(void*), alignof(void*)},
		{m.command.offs, sizeof(NVMe::nvme_command), 1},
		{m.prpDescriptor.offs, sizeof(void*), alignof(void*)},
	};
	for (size_t i = 0; i < arrsize(ranges); i++) {
		if (!ranges[i].offs || ranges[i].offs % ranges[i].align ||
			ranges[i].offs + ranges[i].size > static_cast<mach_vm_address_t>(MaxMemberOffset))
			return false;
		for (size_t j = 0; j < i; j++)
			if (ranges[i].offs < ranges[j].offs + ranges[j].size && ranges[j].offs < ranges[i].offs + ranges[i].size)
				return false;
	}
	return true;
}

/* The class sizes of the loaded kext bound every member offset. Concurrent checks reach the same result. */
bool NVMeFixPlugin::validateLayout(IOService* controller) {
	auto sizeOf = [](const char* name) -> size_t {
		auto sym = OSSymbol::withCStringNoCopy(name);
		auto meta = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
		if (sym)
			sym->release();
		return meta ? meta->getClassSize() : 0;
	};

	if (__atomic_load_n(&requestLayout, __ATOMIC_ACQUIRE) == LayoutUnchecked) {
		auto& m = kextMembers.AppleNVMeRequest;
		size_t size = sizeOf("AppleNVMeRequest"), base = sizeOf("IODMACommand");
		// Without the class sizes only the checks made while solving symbols apply.
		bool known = size && base;
		bool valid = !known || base < size;
		const mach_vm_address_t ends[] {
			m.result.offs + sizeof(uint32_t), m.controller.offs + sizeof(void*),
			m.command.offs + sizeof(NVMe::nvme_command), m.prpDescriptor.offs + sizeof(void*)
		};
		const mach_vm_address_t starts[] {m.result.offs, m.controller.offs, m.command.offs, m.prpDescriptor.offs};
		// Members of AppleNVMeRequest follow its IODMACommand base and end inside the object.
		for (size_t i = 0; known && valid && i < arrsize(ends); i++)
			valid = starts[i] >= base && ends[i] <= size;
		__atomic_store_n(&requestLayout, static_cast<uint8_t>(valid ? LayoutValid : LayoutInvalid), __ATOMIC_RELEASE);
		if (!valid)
			SYSLOG(Log::Plugin, "AppleNVMeRequest members do not fit its class, NVMe commands disabled");
	}
	if (__atomic_load_n(&controllerLayout, __ATOMIC_ACQUIRE) == LayoutUnchecked) {
		// The ANS2 flag is an IONVMeController member: past IOService, inside IONVMeController.
		size_t size = 0, base = sizeOf("IOService");
		for (auto meta = controller->getMetaClass(); meta && !size; meta = meta->getSuperClass())
			if (!strcmp(meta->getClassName(), "IONVMeController"))
				size = meta->getClassSize();
		auto& flag = kextMembers.IONVMeController.ANS2MSIWorkaround;
		bool valid = flag.has() && (!size || !base || (flag.offs >= base && flag.offs < size));
		__atomic_store_n(&controllerLayout, static_cast<uint8_t>(valid ? LayoutValid : LayoutInvalid), __ATOMIC_RELEASE);
		if (flag.has() && !valid)
			SYSLOG(Log::Plugin, "Unexpected ANS2MSIWorkaround offset, workaround disabled");
	}
	return __atomic_load_n(&requestLayout, __ATOMIC_ACQUIRE) == LayoutValid;
}

/**
 * This handler will be invoked when a media (whole disk or a partition) BSD node becomes registered.
 * We need to do two things now:
 * 1. Discover any undetected NVMe controllers.
 * 2. Try and solve symbols. If the relevant partition for symbol solving is not available, the call
 * will fail and we may succeed at next mount.
 * If we have all the symbols ready, we can proceed working with controllers.
 */
bool NVMeFixPlugin::matchingNotificationHandler(void* that, void* , IOService* service,
												IONotifier* notifier) {
	auto plugin = static_cast<NVMeFixPlugin*>(that);
	assert(plugin);
	assert(service);

	IOLockLock(plugin->lck);

	DBGLOG("nvmef", "matchingNotificationHandler for %s", service->getName());

	auto parent = service->getProvider();

	/* Typical depth is 9 on real setups */
	for (int i = 0; parent && i < controllerSearchDepth; i++) {
//		DBGLOG("nvmef", "Parent %s", parent->getName());

		if (parent->metaCast("IONVMeController")) {
			auto existing = plugin->entryForController(parent);
			if (existing && !parent->isInactive() && IOLockTryLock(existing->lck)) {
				if (existing->retryIdentify) {
					existing->retryIdentify = false;
					existing->processed = false;
				}
				IOLockUnlock(existing->lck);
			}
			if (!existing && !parent->isInactive()) {
				auto entry = new ControllerEntry(parent);
				if (!entry) {
					SYSLOG(Log::Plugin, "Failed to allocate ControllerEntry memory");
					break;
				}
				if (!entry->lck || !plugin->controllers.push_back(entry)) {
					SYSLOG(Log::Plugin, "Failed to insert ControllerEntry memory");
					delete entry;
					break;
				}
				break;
			}
		}

		parent = parent->getProvider();
	}

//	DBGLOG("nvmef", "Discovered %u controllers", plugin->controllers.size());

	IOLockUnlock(plugin->lck);

	if (atomic_load_explicit(&plugin->solvedSymbols, memory_order_acquire))
		plugin->handleControllers();

	return true;
}

void NVMeFixPlugin::handleControllers() {
	for (;;) {
		ControllerEntry* entry = nullptr;
		IOLockLock(lck);
		for (size_t i = 0; i < controllers.size(); ++i) {
			if (!controllers[i]->processed) {
				entry = controllers[i];
				// An unclaimed entry has no PM proxy and is skipped by activityTickle.
				IOLockLock(entry->lck);
				entry->processed = true;
				break;
			}
		}
		IOLockUnlock(lck);
		if (!entry)
			return;

		handleController(*entry);
		releaseEntry(*entry);
	}
}

// Use virtual PCI accessors so OpenCore can link without direct IOPCIFamily imports.
static bool linkPartners(IOService* controller, IOPCIDevice*& ssd, IOPCIDevice*& bridge) {
	auto pciParent = [](IORegistryEntry *entry, const IORegistryPlane *plane) {
		auto parent = entry->getParentEntry(plane);
		return parent ? static_cast<IOPCIDevice *>(parent->metaCast("IOPCIDevice")) : nullptr;
	};
	ssd = pciParent(controller, gIOServicePlane);
	bridge = ssd ? pciParent(ssd, gIODTPlane) : nullptr;
	return bridge != nullptr;
}

static IOPCIAddressSpace configSpace(IOPCIDevice* pci, IOByteCount offset) {
	auto space = pci->space;
	space.es.registerNumExtended = (offset >> 8) & 0xF;
	return space;
}

void NVMeFixPlugin::forceEnableASPM(IOService *controller) {
	IOPCIDevice *ssd, *bridge;
	if (!linkPartners(controller, ssd, bridge))
		return;

	// Both partners must support L1. Enable the upstream end first.
	IOPCIDevice *partners[] {bridge, ssd};
	UInt8 offsets[2] {};
	UInt16 original[2] {};
	for (size_t i = 0; i < arrsize(partners); ++i) {
		auto pci = partners[i];
		if (!pci->findPCICapability(kIOPCICapabilityIDPCIExpress, &offsets[i]))
			return;
		if (offsets[i] > 0xEC)
			return;
		auto capabilities = pci->configRead32(configSpace(pci, 0), offsets[i] + 0x0C);
		if (capabilities == UINT32_MAX || !(capabilities & (1U << 11)))
			return;
		original[i] = pci->configRead16(configSpace(pci, 0), offsets[i] + 0x10);
		if (original[i] == UINT16_MAX)
			return;
	}

	size_t changed = 0;
	bool ok = true;
	for (size_t i = 0; ok && i < arrsize(partners); ++i, ++changed) {
		auto pci = partners[i];
		UInt8 offset = offsets[i] + 0x10;
		if (!(original[i] & ASPM_L1EntryEnabled))
			pci->configWrite16(configSpace(pci, 0), offset, original[i] | ASPM_L1EntryEnabled);
		auto control = pci->configRead16(configSpace(pci, 0), offset);
		ok = control != UINT16_MAX && (control & ASPM_L1EntryEnabled);
	}

	if (!ok) {
		// L1 on only one end of the link: restore what was there, downstream end first.
		SYSLOG(Log::Plugin, "Failed to enable ASPM L1 on both link partners, restoring");
		while (changed-- > 0) {
			auto pci = partners[changed];
			pci->configWrite16(configSpace(pci, 0), offsets[changed] + 0x10, original[changed]);
		}
		return;
	}
	for (size_t i = 0; i < arrsize(partners); ++i) {
		auto control = partners[i]->configRead16(configSpace(partners[i], 0), offsets[i] + 0x10);
		partners[i]->setProperty("pci-aspm-custom", control, 32);
	}
}

namespace {
/* Configuration access for L1SS::enableASPML12 through the virtual accessors */
struct PCIPort {
	IOPCIDevice* pci {nullptr};
	uint16_t pcie {0}, l1ss {0};
	uint32_t read32(uint16_t off) { return pci->configRead32(configSpace(pci, off), off & 0xFF); }
	void write32(uint16_t off, uint32_t v) { pci->configWrite32(configSpace(pci, off), off & 0xFF, v); }
	uint16_t read16(uint16_t off) { return pci->configRead16(configSpace(pci, off), off & 0xFF); }
	void write16(uint16_t off, uint16_t v) { pci->configWrite16(configSpace(pci, off), off & 0xFF, v); }

	bool find(IOPCIDevice* device) {
		pci = device;
		UInt8 offset = 0;
		IOByteCount extended = 0;
		// DevCap2/DevCtl2 must be inside the standard configuration header.
		if (!pci->findPCICapability(kIOPCICapabilityIDPCIExpress, &offset) || offset + L1SS::DevCtl2 + 2 > 0x100)
			return false;
		pcie = offset;
		if (!pci->extendedFindPCICapability(static_cast<UInt32>(kIOPCIExpressCapabilityIDL1PMSubstates), &extended) ||
			extended < 0x100 || extended + L1SS::Ctl2 + 4 > 0x1000)
			return false;
		l1ss = static_cast<uint16_t>(extended);
		return true;
	}
};
}

/**
 * -nvmefl1ss (testing only): enable ASPM L1.2 on the SSD link with Linux' timing. The board must route
 * CLKREQ#; a link that cannot leave L1.2 makes the SSD stop answering. Every step is read back and the
 * previous configuration restored on a mismatch. The entry lock is held.
 */
void NVMeFixPlugin::configureL12(IOService* controller) {
	IOPCIDevice *ssd, *bridge;
	if (!linkPartners(controller, ssd, bridge))
		return;
	PCIPort parent, child;
	L1SS::Timing timing;
	auto result = parent.find(bridge) && child.find(ssd) ?
		L1SS::enableASPML12(parent, child, timing) : L1SS::Result::Unsupported;
	controller->setProperty("nvmef-l1ss-status", L1SS::describe(result));
	if (timing.thresholdUs) {
		controller->setProperty("nvmef-l12-threshold-us", timing.thresholdUs, 32);
		controller->setProperty("nvmef-l12-power-on-us", timing.powerOnUs, 32);
	}
	if (result == L1SS::Result::Enabled)
		SYSLOG(Log::Plugin, "ASPM L1.2 enabled, threshold %u us, T_POWER_ON %u us", timing.thresholdUs, timing.powerOnUs);
	else if (result != L1SS::Result::AlreadyEnabled)
		SYSLOG(Log::Plugin, "ASPM L1.2 %s", L1SS::describe(result));
}

/* A power transition can drop the L1 PM Substates configuration. The entry lock is held. */
void NVMeFixPlugin::reapplyLinkPM(ControllerEntry& entry) {
	if (!l1ssRequested)
		return;
	configureL12(entry.controller);
	reportLinkPM(entry.controller);
}

/**
 * Publish the link power management both link partners support and have enabled, read once at setup:
 * nvmef-aspm-supported / nvmef-aspm-enabled (bit 0 L0s, bit 1 L1) and, with the L1 PM Substates
 * capability, nvmef-l1ss-supported / nvmef-l1ss-enabled (bit 0 PCI-PM L1.2, bit 1 PCI-PM L1.1,
 * bit 2 ASPM L1.2, bit 3 ASPM L1.1).
 */
void NVMeFixPlugin::reportLinkPM(IOService* controller) {
	IOPCIDevice *ssd, *bridge;
	if (!linkPartners(controller, ssd, bridge))
		return;
	IOPCIDevice *partners[] {bridge, ssd};
	for (auto pci : partners) {
		UInt8 offset = 0;
		if (pci->findPCICapability(kIOPCICapabilityIDPCIExpress, &offset) && offset <= 0xEC) {
			auto capabilities = pci->configRead32(configSpace(pci, 0), offset + 0x0C);
			auto control = pci->configRead16(configSpace(pci, 0), offset + 0x10);
			if (capabilities != UINT32_MAX && control != UINT16_MAX) {
				pci->setProperty("nvmef-aspm-supported", (capabilities >> 10) & 3, 32);
				pci->setProperty("nvmef-aspm-enabled", control & ASPM_Mask, 32);
			}
			// L1.2 is only entered when LTR is enabled and the device may report a latency.
			if (offset + L1SS::DevCtl2 + 2 <= 0x100) {
				auto devCap2 = pci->configRead32(configSpace(pci, 0), offset + L1SS::DevCap2);
				auto devCtl2 = pci->configRead16(configSpace(pci, 0), offset + L1SS::DevCtl2);
				if (devCap2 != UINT32_MAX && devCtl2 != UINT16_MAX)
					pci->setProperty("nvmef-ltr", (devCap2 & L1SS::DevCap2LTR ? 1U : 0U) | (devCtl2 & L1SS::DevCtl2LTR ? 2U : 0U), 32);
			}
		}
		IOByteCount ltr = 0;
		if (pci->extendedFindPCICapability(static_cast<UInt32>(kIOPCIExpressCapabilityIDLatencyTolerenceReporting), &ltr) &&
			ltr >= 0x100 && ltr <= 0xFF8) {
			// Max Snoop (bits 15:0) and Max No-Snoop (bits 31:16) latency the device may report
			auto maxLatency = pci->configRead32(configSpace(pci, ltr + 4), (ltr + 4) & 0xFF);
			if (maxLatency != UINT32_MAX)
				pci->setProperty("nvmef-ltr-max-latency", maxLatency, 32);
		}
		IOByteCount l1ss = 0;
		if (pci->extendedFindPCICapability(static_cast<UInt32>(kIOPCIExpressCapabilityIDL1PMSubstates), &l1ss) &&
			l1ss >= 0x100 && l1ss <= 0xFF4) {
			auto capabilities = pci->configRead32(configSpace(pci, l1ss + 4), (l1ss + 4) & 0xFF);
			auto control = pci->configRead32(configSpace(pci, l1ss + 8), (l1ss + 8) & 0xFF);
			if (capabilities != UINT32_MAX && control != UINT32_MAX) {
				// Bit 4 of the capabilities says the substates are supported at all.
				pci->setProperty("nvmef-l1ss-supported", (capabilities & (1U << 4)) ? capabilities & 0xF : 0, 32);
				pci->setProperty("nvmef-l1ss-enabled", control & 0xF, 32);
			}
		}
	}
}

void NVMeFixPlugin::handleController(ControllerEntry& entry) {
	assert(entry.controller);

	uint32_t vendor {};
	propertyFromParent(entry.controller, "vendor-id", vendor);
	if (vendor == 0x106b || entry.controller->metaCast("AppleNVMeController")) {
		SYSLOG(Log::Plugin, "Ignoring Apple controller");
		return;
	}

	/**
	 * Force enable ASPM when the user cannot provide device properties.
	 */
	if (checkKernelArgument("-nvmefaspm"))
		forceEnableASPM(entry.controller);
	if (checkKernelArgument("-nvmefl1ss")) {
		l1ssRequested = true;
		configureL12(entry.controller);
	}
	reportLinkPM(entry.controller);

	/* Disassembled offsets are only used once they fit the loaded classes */
	bool layoutValid = validateLayout(entry.controller);

	/**
	 * Force enable ANS2MSIWorkaround.
	 *
	 * We would often get a panic with I/O Read command timeout on VMware and Samsung PM981.
	 * Some investigation showed that CQ head entry phase and CQ phase would mismatch.
	 * This implies there is a race such that CQ head gets updated to point to an entry with inverted
	 * phase.
	 * Since FilterIRQ detects phase mismatch, it does not schedule HandleIRQ at the workloop of NVMe
	 * controller, so a request is never handled and we get a timeout. If Filter and Handle IRQ calls can
	 * possibly race, this may happen because HandleIRQ does not manage to update CQ phase in time before
	 * FilterIRQ phase check is scheduled, observing the old phase. This is the case in situation where
	 * two FilterIRQ calls happen with no HandleIRQ in between where the controller was too slow to
	 * notice INTMS being set.
	 * Normally, IRQ is masked just before HandleIRQ is scheduled in FilterIRQ, and unmasked when
	 * HandleIRQ is done. IONVMeController::ANS2MSIWorkaround forces IRQ to be masked at the very
	 * start of FilterIRQ instead so that FilterIRQ does not race with itself. This seems to eliminate
	 * the timeouts.
	 **/
	if (__atomic_load_n(&controllerLayout, __ATOMIC_ACQUIRE) == LayoutValid) {
		kextMembers.IONVMeController.ANS2MSIWorkaround.get(entry.controller) = 1;
	} else {
		DBGLOG(Log::Plugin, "Ignoring ANS2 workaround patch on newer system");
	}

	/* First get quirks based on PCI device */
	entry.quirks = NVMe::quirksForController(entry.controller);
	propertyFromParent(entry.controller, "ps-max-latency-us", entry.ps_max_latency_us);
	propertyFromParent(entry.controller, "ps-max-latency-us-ac", entry.ac_latency_us);
	propertyFromParent(entry.controller, "ps-max-latency-us-battery", entry.battery_latency_us);
	if (entry.ac_latency_us != UINT64_MAX || entry.battery_latency_us != UINT64_MAX)
		watchPowerSource();

	if (!layoutValid)
		return;

	IOBufferMemoryDescriptor* identifyDesc {nullptr};

	if (identify(entry, identifyDesc) != kIOReturnSuccess || !identifyDesc) {
		entry.retryIdentify = true;
		SYSLOG(Log::Plugin, "Failed to identify controller");
		return;
	}

	auto ctrl = reinterpret_cast<NVMe::nvme_id_ctrl*>(identifyDesc->getBytesNoCopy());
	if (!ctrl) {
		DBGLOG(Log::Plugin, "Failed to get identify buffer bytes");
		if (identifyDesc)
			identifyDesc->release();
		return;
	}

	entry.identify = identifyDesc;
	if (ctrl->npss >= arrsize(ctrl->psd)) {
		SYSLOG(Log::Plugin, "Invalid NPSS");
		return;
	}

	/* Get additional quirks based on identify data */
	entry.quirks |= NVMe::quirksForController(ctrl->vid, ctrl->mn, ctrl->fr);

	entry.controller->setProperty("quirks", entry.quirks, 8 * sizeof(entry.quirks));

#ifdef DEBUG
	char mn[40];
	lilu_os_memcpy(mn, ctrl->mn, sizeof(mn));
	mn[sizeof(mn) - 1] = '\0';

	DBGLOG(Log::Plugin, "Identified model %s (vid 0x%x)", mn, ctrl->vid);
#endif

	auto state = applyAPST(entry, ctrl);
	if (!PM.init(entry, ctrl, state))
		SYSLOG(Log::PM, "Failed to initialise power management");
}

IOReturn NVMeFixPlugin::identify(ControllerEntry& entry, IOBufferMemoryDescriptor*& desc) {
	IOReturn ret = kIOReturnSuccess;

	uint8_t* data {nullptr};
	bool prepared {false};

	desc = IOBufferMemoryDescriptor::withCapacity(sizeof(NVMe::nvme_id_ctrl), kIODirectionIn);

	if (!desc) {
		SYSLOG(Log::Plugin, "Failed to init descriptor");
		ret = kIOReturnNoResources;
		goto fail;
	}
	data = static_cast<uint8_t*>(desc->getBytesNoCopy());
	if (!data) {
		ret = kIOReturnNoResources;
		goto fail;
	}
	memset(data, '\0', desc->getLength());

	ret = desc->prepare();
	if (ret != kIOReturnSuccess) {
		SYSLOG(Log::Plugin, "Failed to prepare descriptor");
		goto fail;
	}
	prepared = true;

	if (kextFuncs.IONVMeController.IssueIdentifyCommandNew.fptr)
		ret = kextFuncs.IONVMeController.IssueIdentifyCommandNew(entry.controller, desc, 0, false);
	else
		ret = kextFuncs.IONVMeController.IssueIdentifyCommand(entry.controller, desc, nullptr, 0);
	if (ret != kIOReturnSuccess) {
		SYSLOG(Log::Plugin, "issueIdentifyCommand failed");
		goto fail;
	}

fail:
	if (prepared)
		desc->complete();
	if (ret != kIOReturnSuccess && desc) {
		desc->release();
		desc = nullptr;
	}
	return ret;
}

IOReturn NVMeFixPlugin::NVMeFeatures(ControllerEntry& entry, unsigned fid, unsigned* dword11,
										IOBufferMemoryDescriptor* desc, uint32_t* res, bool set) {
	auto ret = kIOReturnSuccess;

	bool prepared {false};

	if (desc) {
		ret = desc->prepare();
		prepared = ret == kIOReturnSuccess;
	}

	if (!desc || prepared) {
		void *req;
		if (kextFuncs.IONVMeController.GetRequestNew.fptr)
			req = kextFuncs.IONVMeController.GetRequestNew(entry.controller, 1, 0); /* Set 0b10 to tickle */
		else
			req = kextFuncs.IONVMeController.GetRequest(entry.controller, 1); /* Set 0b10 to tickle */

		if (!req) {
			DBGLOG(Log::Feature, "IONVMeController::GetRequest failed");
			ret = kIOReturnNoResources;
		} else if (!reinterpret_cast<OSMetaClassBase*>(req)->metaCast("AppleNVMeRequest")) {
			SYSLOG(Log::Feature, "GetRequest returned an unexpected object");
			ret = kIOReturnInternalError;
		} else if (desc)
			ret = reinterpret_cast<IODMACommand*>(req)->setMemoryDescriptor(desc, false);

		if (ret == kIOReturnSuccess) {
			bool dmaPrepared = false;
			if (set)
				kextFuncs.AppleNVMeRequest.BuildCommandSetFeaturesCommon(req, fid);
			else
				kextFuncs.AppleNVMeRequest.BuildCommandGetFeatures(req, fid);

			if (dword11)
				kextMembers.AppleNVMeRequest.command.get(req).features.dword11 = *dword11;
			if (desc) {
				kextMembers.AppleNVMeRequest.prpDescriptor.get(req) = desc;
				ret = reinterpret_cast<IODMACommand*>(req)->prepare(0, desc->getLength());
				dmaPrepared = ret == kIOReturnSuccess;
			}

			if (ret != kIOReturnSuccess)
				DBGLOG(Log::Feature, "Failed to prepare DMA command");
			else {
				if (desc)
					ret = kextFuncs.AppleNVMeRequest.GenerateIOVMSegments(req, 0,
																		  desc->getLength());

				if (ret != kIOReturnSuccess)
					DBGLOG(Log::Feature, "Failed to generate IO VM segments");
				else {
					kextMembers.AppleNVMeRequest.controller.get(req) = entry.controller;

					ret = kextFuncs.IONVMeController.ProcessSyncNVMeRequest(entry.controller,
																			req);
					if (ret != kIOReturnSuccess)
						DBGLOG(Log::Feature, "ProcessSyncNVMeRequest failed");
					else if (res)
						*res = kextMembers.AppleNVMeRequest.result.get(req);
				}
			}
			if (dmaPrepared)
				reinterpret_cast<IODMACommand*>(req)->complete();
		}
		if (req)
			kextFuncs.IONVMeController.ReturnRequest(entry.controller, req);
	} else
		SYSLOG(Log::Feature, "Failed to prepare buffer");

	if (desc && prepared)
		desc->complete();

	return ret;
}

/**
 * The AC/battery latency overrides follow the first IOPMPowerSource. Installed once, when a controller
 * has one of the overrides; the notification may call back synchronously, so no lock is held here.
 */
void NVMeFixPlugin::watchPowerSource() {
	bool expected = false;
	if (!atomic_compare_exchange_strong(&powerSourceWatch, &expected, true))
		return;
	powerSourceNotifier = IOService::addMatchingNotification(gIOFirstPublishNotification,
		IOService::serviceMatching("IOPMPowerSource"), powerSourcePublished, this);
	if (!powerSourceNotifier)
		SYSLOG(Log::Plugin, "Failed to watch the power source, AC/battery latencies unused");
}

bool NVMeFixPlugin::powerSourcePublished(void* that, void*, IOService* service, IONotifier*) {
	auto plugin = static_cast<NVMeFixPlugin*>(that);
	IONotifier* none = nullptr;
	auto notifier = service->registerInterest(gIOGeneralInterest, powerSourceChanged, plugin);
	if (!notifier)
		return true;
	// One power source is enough; a second one keeps no interest.
	if (!__atomic_compare_exchange_n(&plugin->powerSourceInterest, &none, notifier, false,
									 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		notifier->remove();
		return true;
	}
	powerSourceChanged(plugin, nullptr, kIOPMMessageBatteryStatusHasChanged, service, nullptr, 0);
	return true;
}

IOReturn NVMeFixPlugin::powerSourceChanged(void* target, void*, UInt32 type, IOService* provider, void*, vm_size_t) {
	if (type != kIOPMMessageBatteryStatusHasChanged)
		return kIOReturnSuccess;
	auto plugin = static_cast<NVMeFixPlugin*>(target);
	auto property = provider->copyProperty(kIOPMPSExternalConnectedKey);
	auto connected = OSDynamicCast(OSBoolean, property);
	int ac = connected ? connected->isTrue() : -1;
	if (property)
		property->release();
	if (atomic_exchange_explicit(&plugin->onAC, ac, memory_order_acq_rel) != ac)
		plugin->powerSourceUpdated();
	return kIOReturnSuccess;
}

/* Reconfigure APST with the latency limit of the new power source on each controller's worker */
void NVMeFixPlugin::powerSourceUpdated() {
	IOLockLock(lck);
	for (size_t i = 0; i < controllers.size(); i++) {
		auto entry = controllers[i];
		auto pm = static_cast<NVMePMProxy*>(__atomic_load_n(&entry->pm, __ATOMIC_ACQUIRE));
		if (pm && (entry->ac_latency_us != UINT64_MAX || entry->battery_latency_us != UINT64_MAX))
			pm->scheduleRetry(0, __atomic_load_n(&entry->generation, __ATOMIC_ACQUIRE));
	}
	IOLockUnlock(lck);
}

bool NVMeFixPlugin::terminatedNotificationHandler(void* that, void* , IOService* service,
												IONotifier* notifier) {
	auto plugin = static_cast<NVMeFixPlugin*>(that);
	assert(plugin);
	assert(service && service->metaCast("IONVMeController"));

	ControllerEntry* entry = nullptr;
	IOLockLock(plugin->lck);
	for (size_t i = 0; i < plugin->controllers.size(); ++i) {
		if (plugin->controllers[i]->controller == service) {
			entry = plugin->controllers[i];
			plugin->controllers.erase(i);
			break;
		}
	}
	IOLockUnlock(plugin->lck);

	if (entry) {
		// Drain setup or a tickle that acquired the entry before removal.
		IOLockLock(entry->lck);
		entry->removed = true;
		plugin->setActivePM(*entry, false);
		auto pm = entry->pm;
		IOLockUnlock(entry->lck);
		if (pm) {
			service->deRegisterInterestedDriver(pm);
			pm->PMstop();
			pm->release();
		} else {
			delete entry;
		}
	}

	return false;
}

/**
 * NOTE: We are in kmod context, not IOService. This works fine as long as we publish our personality
 * in Info.plist to match something in ioreg, but specify a non-existing IOClass so that IOKit attempts
 * to load us anyway. It is otherwise unsafe to use matching notifications from kmod when we have a
 * living IOService.
 */
void NVMeFixPlugin::init() {
	LiluAPI::Error err;

	if (!(lck = IOLockAlloc())) {
		SYSLOG(Log::Plugin, "Failed to alloc lock");
		goto fail;
	}

	atomic_store_explicit(&solvedSymbols, false, memory_order_relaxed);

	matchingNotifier = IOService::addMatchingNotification(gIOPublishNotification,
							IOService::serviceMatching("IOMediaBSDClient"),
							matchingNotificationHandler,
						    this);
	if (!matchingNotifier) {
		SYSLOG(Log::Plugin, "Failed to register for matching notification");
		goto fail;
	}

	terminationNotifier = IOService::addMatchingNotification(gIOTerminatedNotification,
							IOService::serviceMatching("IONVMeController"),
							terminatedNotificationHandler,
						    this);
	if (!terminationNotifier) {
		SYSLOG(Log::Plugin, "Failed to register for termination notification");
		goto fail;
	}

	DBGLOG(Log::Plugin, "Registered for matching notifications");

	err = lilu.onKextLoad(&kextInfo, 1, NVMeFixPlugin::processKext, this);
	if (err != LiluAPI::Error::NoError) {
		SYSLOG(Log::Plugin, "Failed to register kext load cb");
		goto fail;
	}

	return;
fail:
	if (matchingNotifier)
		matchingNotifier->remove();
	if (terminationNotifier)
		terminationNotifier->remove();
	for (size_t i = 0; i < controllers.size(); ++i)
		delete controllers[i];
	controllers.deinit();
	if (lck)
		IOLockFree(lck);
}

void NVMeFixPlugin::deinit() {
	/* This kext is not unloadable */
	panic("nvmef: deinit called");
}

NVMeFixPlugin::ControllerEntry* NVMeFixPlugin::entryForController(IOService* controller) const {
	for (size_t i = 0; i < controllers.size(); i++)
		if (controllers[i]->controller == controller)
			return controllers[i];
	return nullptr;
}

static const char *bootargOff[] {
	"-nvmefoff"
};

static const char *bootargDebug[] {
	"-nvmefdbg"
};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	nullptr,
	0,
	KernelVersion::Mojave,
	KernelVersion::Tahoe,
	[]() {
		NVMeFixPlugin::globalPlugin().init();
	}
};
