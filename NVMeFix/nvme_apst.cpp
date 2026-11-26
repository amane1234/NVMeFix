//
// @file nvme_apst.cpp
//
// NVMeFix
//
// Copyright © 2020 acidanthera. All rights reserved.
//
// This program and the accompanying materials
// are licensed and made available under the terms and conditions of the BSD License
// which accompanies this distribution.  The full text of the license may be found at
// http://opensource.org/licenses/bsd-license.php
// THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
// WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver
 * Portions Copyright (c) 2011-2014, Intel Corporation.
 */

#include "Log.hpp"
#include "NVMeFixPlugin.hpp"

/* The latency limit for the current power source (ps-max-latency-us-ac/-battery), else ps-max-latency-us */
uint64_t NVMeFixPlugin::latencyLimit(const ControllerEntry& entry) const {
	auto ac = atomic_load_explicit(&onAC, memory_order_acquire);
	if (ac == 1 && entry.ac_latency_us != UINT64_MAX)
		return entry.ac_latency_us;
	if (ac == 0 && entry.battery_latency_us != UINT64_MAX)
		return entry.battery_latency_us;
	return entry.ps_max_latency_us;
}

bool NVMeFixPlugin::apstAllowed(const ControllerEntry& entry) const {
	return !(entry.quirks & NVMe::nvme_quirks::NVME_QUIRK_NO_APST) && latencyLimit(entry) > 0;
}

/**
 * Returns the APST state the controller reports after the configuration, which is what the power policy
 * must follow. entry.apstConfirmed tells whether that state and the table are the ones requested.
 */
NVMeFixPlugin::APSTState NVMeFixPlugin::applyAPST(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl) {
	entry.apstConfirmed = true;
	if (!(ctrl->apsta & 1)) {
		entry.controller->setProperty("apst", false);
		return APSTState::Disabled;
	}

	auto table = IOBufferMemoryDescriptor::withCapacity(sizeof(NVMe::nvme_feat_auto_pst), kIODirectionOut);
	if (!table) {
		SYSLOG(Log::APST, "Failed to create APST table descriptor");
		entry.apstConfirmed = false;
		entry.controller->removeProperty("apst");
		return APSTState::Unknown;
	}

	bool requested = false, enabled = false, matches = false;
	auto status = configureAPST(entry, ctrl, requested, table);
	// A Set Features right after a wake or reset can fail transiently: try once more at once.
	if (status != kIOReturnSuccess) {
		SYSLOG(Log::APST, "Failed to configure APST with 0x%x, retrying", status);
		status = configureAPST(entry, ctrl, requested, table);
	}
	entry.controller->setProperty("apst-requested", requested);

	IOReturn readback;
	if (status == kIOReturnSuccess) {
		// Read the feature back in every build: a controller can accept the command and keep another policy.
		readback = verifyAPST(entry, ctrl, table, enabled, matches);
	} else {
		SYSLOG(Log::APST, "Failed to configure APST with 0x%x", status);
		readback = APSTenabled(entry, enabled);
	}
	table->release();

	if (readback != kIOReturnSuccess) {
		entry.apstConfirmed = false;
		entry.controller->removeProperty("apst");
		return APSTState::Unknown;
	}
	entry.apstConfirmed = status == kIOReturnSuccess && matches;
	if (status == kIOReturnSuccess && !matches)
		SYSLOG(Log::APST, "APST readback differs from the requested configuration");
#ifdef DEBUG
	if (enabled && dumpAPST(entry, ctrl->npss))
		DBGLOG(Log::APST, "Failed to dump APST table");
#endif
	entry.controller->setProperty("apst", enabled);
	return enabled ? APSTState::Enabled : APSTState::Disabled;
}

/* linux/drivers/nvme/host/core.c:nvme_configure_apst */
IOReturn NVMeFixPlugin::configureAPST(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl,
									 bool& enabled, IOBufferMemoryDescriptor* apstDesc) {
	assert(ctrl);
	assert(entry.controller);
	assert(apstDesc);

	auto apstTable = reinterpret_cast<NVMe::nvme_feat_auto_pst*>(apstDesc->getBytesNoCopy());
	if (!apstTable)
		return kIOReturnNoResources;

	memset(apstTable, 0, sizeof(*apstTable));
	int max_ps {-1};
	const uint64_t latencyLimitUs = latencyLimit(entry);
	if (apstAllowed(entry)) {
		uint64_t target {0};
		uint64_t max_lat_us {0};

		/*
		* Walk through all states from lowest- to highest-power.
		* According to the spec, lower-numbered states use more
		* power.  NPSS, despite the name, is the index of the
		* lowest-power state, not the number of states.
		*/
		for (int state = ctrl->npss; state >= 0; state--) {
			if (target) {
				apstTable->entries[state] = target;
				DBGLOG(Log::APST, "Set entry %d to 0x%llx", state, target);
			}

			/*
			 * Don't allow transitions to the deepest state
			 * if it's quirked off.
			 */
			if (state == ctrl->npss && (entry.quirks & NVMe::nvme_quirks::NVME_QUIRK_NO_DEEPEST_PS))
				continue;

			/*
			 * Is this state a useful non-operational state for
			 * higher-power states to autonomously transition to?
			 */
			if (!(ctrl->psd[state].flags & NVMe::NVME_PS_FLAGS_NON_OP_STATE))
				continue;

			uint64_t exit_latency_us = ctrl->psd[state].exit_lat;
			if (exit_latency_us > latencyLimitUs)
				continue;

			uint64_t total_latency_us = exit_latency_us + ctrl->psd[state].entry_lat;
			/*
			 * This state is good.  Use it as the APST idle
			 * target for higher power states.
			 */
			uint64_t transition_ms = total_latency_us + 19;
			transition_ms /= 20;
			/*
			 * Firmware that reports no entry or exit latency would get an idle time of 0, which the
			 * spec leaves without a useful transition. Use Linux' primary APST timeout (100 ms) as
			 * the minimum idle time instead, so such states neither thrash nor go unused.
			 */
			if (transition_ms < minIdleTimeMs) {
				if (!total_latency_us)
					DBGLOG(Log::APST, "State %d reports no transition latency", state);
				transition_ms = minIdleTimeMs;
			}
			if (transition_ms > (1ull << 24) - 1)
				transition_ms = (1ull << 24) - 1;

			target = (state << 3ull) | (transition_ms << 8ull);

			if (max_ps == -1)
				max_ps = state;

			if (total_latency_us > max_lat_us)
				max_lat_us = total_latency_us;
		}

		if (max_ps == -1)
			DBGLOG(Log::APST, "No non-operational states are available");
		else
			DBGLOG(Log::APST, "APST enabled: max PS = %d, max round-trip latency = %lluus\n",
				max_ps, max_lat_us);
	}

	// A zero table must replace any policy left in the controller, even when
	// the latency limit is positive but excludes every non-operational state.
	enabled = max_ps != -1;
	uint32_t dword11 = enabled ? 1 : 0;
	return NVMeFeatures(entry, NVMe::NVME_FEAT_AUTO_PST, &dword11, apstDesc, nullptr, true);
}

/* Read APST back and compare the enable bit and the table entries for the reported states */
IOReturn NVMeFixPlugin::verifyAPST(ControllerEntry& entry, const NVMe::nvme_id_ctrl* ctrl,
								   const IOBufferMemoryDescriptor* written, bool& enabled, bool& matches) {
	matches = false;
	auto readDesc = IOBufferMemoryDescriptor::withCapacity(sizeof(NVMe::nvme_feat_auto_pst), kIODirectionIn);
	if (!readDesc)
		return kIOReturnNoResources;
	auto read = static_cast<NVMe::nvme_feat_auto_pst*>(readDesc->getBytesNoCopy());
	auto expected = static_cast<const NVMe::nvme_feat_auto_pst*>(
		const_cast<IOBufferMemoryDescriptor*>(written)->getBytesNoCopy());
	if (!read || !expected) {
		readDesc->release();
		return kIOReturnNoResources;
	}
	memset(read, 0, sizeof(*read));

	uint32_t res {};
	auto ret = NVMeFeatures(entry, NVMe::NVME_FEAT_AUTO_PST, nullptr, readDesc, &res, false);
	if (ret == kIOReturnSuccess) {
		enabled = (res & 1) != 0;
		bool wantEnabled = false;
		for (int state = 0; state <= ctrl->npss; state++)
			wantEnabled |= expected->entries[state] != 0;
		matches = enabled == wantEnabled;
		// Entries beyond NPSS are reserved: only compare the states the controller reports.
		for (int state = 0; matches && state <= ctrl->npss; state++)
			matches = read->entries[state] == expected->entries[state];
	} else {
		DBGLOG(Log::APST, "Failed to read back APST");
	}
	readDesc->release();
	return ret;
}

IOReturn NVMeFixPlugin::APSTenabled(ControllerEntry& entry, bool& enabled) {
	uint32_t res {};
	auto table = IOBufferMemoryDescriptor::withCapacity(sizeof(NVMe::nvme_feat_auto_pst), kIODirectionIn);
	if (!table)
		return kIOReturnNoResources;
	auto ret = NVMeFeatures(entry, NVMe::NVME_FEAT_AUTO_PST, nullptr, table, &res, false);
	table->release();

	if (ret != kIOReturnSuccess)
		DBGLOG(Log::APST, "Failed to get features");
	else
		enabled = (res & 1) != 0;
	return ret;
}

IOReturn NVMeFixPlugin::dumpAPST(ControllerEntry& entry, int npss) {
	assert(entry.controller);

	IOReturn ret = kIOReturnSuccess;
	auto apstDesc = IOBufferMemoryDescriptor::withCapacity(sizeof(NVMe::nvme_feat_auto_pst),
														   kIODirectionIn);
	if (!apstDesc) {
		SYSLOG(Log::APST, "Failed to create APST table descriptor");
		return kIOReturnNoResources;
	}

	auto apstTable = static_cast<NVMe::nvme_feat_auto_pst*>(apstDesc->getBytesNoCopy());
	if (!apstTable) {
		apstDesc->release();
		return kIOReturnNoResources;
	}
	memset(apstTable, '\0', apstDesc->getLength());

	ret = NVMeFeatures(entry, NVMe::NVME_FEAT_AUTO_PST, nullptr, apstDesc, nullptr, false);
	if (ret != kIOReturnSuccess) {
		DBGLOG(Log::APST, "Failed to get features");
		goto fail;
	}

	for (int state = npss; state >= 0; state--)
		DBGLOG(Log::APST, "entry %d : 0x%llx", state, apstTable->entries[state]);

fail:
	if (apstDesc)
		apstDesc->release();
	return ret;
}
