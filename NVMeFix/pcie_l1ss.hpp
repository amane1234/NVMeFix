//
// @file pcie_l1ss.hpp
//
// NVMeFix
//
// ASPM L1.2 configuration of one PCIe link, following Linux drivers/pci/pcie/aspm.c
// (aspm_calc_l12_info, pcie_config_aspm_l1ss, pcie_config_aspm_link). Kernel independent so the
// register sequence can be tested against a simulated configuration space.
//
// SPDX-License-Identifier: GPL-2.0

#ifndef pcie_l1ss_hpp
#define pcie_l1ss_hpp

#include <stdint.h>

namespace L1SS {

/* PCI Express capability */
static constexpr uint16_t PCIeFlags = 0x02, LinkCtl = 0x10, DevCap2 = 0x24, DevCtl2 = 0x28;
static constexpr uint16_t LinkCtlASPMMask = 0x3, LinkCtlASPML1 = 0x2;
static constexpr uint32_t DevCap2LTR = 1U << 11;
static constexpr uint16_t DevCtl2LTR = 1U << 10;

/* L1 PM Substates extended capability */
static constexpr uint16_t Cap = 0x04, Ctl1 = 0x08, Ctl2 = 0x0C;
static constexpr uint32_t CapASPML12 = 1U << 2, CapL1PMSS = 1U << 4;
static constexpr uint32_t Ctl1PCIPML12 = 1U << 0, Ctl1ASPML12 = 1U << 2, Ctl1L12Mask = Ctl1PCIPML12 | Ctl1ASPML12;
static constexpr uint32_t Ctl1CMRestore = 0x0000FF00, Ctl1ThValue = 0x03FF0000, Ctl1ThScale = 0xE0000000;
static constexpr uint32_t Ctl1Threshold = Ctl1ThValue | Ctl1ThScale;
static constexpr uint32_t Ctl2Mask = 0xFB; /* T_POWER_ON value 7:3, scale 1:0 */

/* Port T_POWER_ON in microseconds; scale 3 is reserved */
inline uint32_t powerOnUs(uint32_t scale, uint32_t value) {
	switch (scale) {
		case 0: return value * 2;
		case 1: return value * 10;
		case 2: return value * 100;
		default: return 0;
	}
}

/* LTR_L1.2_THRESHOLD: a 10-bit value times 1, 32, 1024, 32768, 1048576 or 33554432 ns */
inline void encodeThreshold(uint32_t thresholdUs, uint32_t& scale, uint32_t& value) {
	const uint64_t ns = static_cast<uint64_t>(thresholdUs) * 1000;
	uint64_t unit = 1;
	for (scale = 0; scale < 5 && ns > unit * 0x3FF; scale++)
		unit *= 32;
	value = static_cast<uint32_t>((ns + unit - 1) / unit);
	if (value > 0x3FF)
		value = 0x3FF;
}

struct Timing {
	uint32_t ctl1 {0}, ctl2 {0}, thresholdUs {0}, powerOnUs {0}, commonModeUs {0};
};

/* aspm_calc_l12_info: the larger restore time and T_POWER_ON of both ports, and the L0-L1.2-L0 threshold */
inline bool timing(uint32_t parentCap, uint32_t childCap, Timing& t) {
	t = Timing {};
	uint32_t parentScale = (parentCap >> 16) & 3, parentValue = (parentCap >> 19) & 0x1F;
	uint32_t childScale = (childCap >> 16) & 3, childValue = (childCap >> 19) & 0x1F;
	if (parentScale == 3 || childScale == 3)
		return false;
	uint32_t parentMode = (parentCap >> 8) & 0xFF, childMode = (childCap >> 8) & 0xFF;
	t.commonModeUs = parentMode > childMode ? parentMode : childMode;
	if (powerOnUs(parentScale, parentValue) > powerOnUs(childScale, childValue)) {
		t.ctl2 = parentScale | parentValue << 3;
		t.powerOnUs = powerOnUs(parentScale, parentValue);
	} else {
		t.ctl2 = childScale | childValue << 3;
		t.powerOnUs = powerOnUs(childScale, childValue);
	}
	/* PCIe r3.1 sec 5.5.3.3.1: T(POWER_OFF) is at most 2 us and T(L1.2) at least 4 us */
	t.thresholdUs = 2 + 4 + t.commonModeUs + t.powerOnUs;
	uint32_t scale, value;
	encodeThreshold(t.thresholdUs, scale, value);
	t.ctl1 = t.commonModeUs << 8 | value << 16 | scale << 29;
	return true;
}

enum class Result {
	Enabled,        /* ASPM L1.2 configured and enabled on both ends */
	AlreadyEnabled, /* Nothing to change */
	Unsupported,    /* A port lacks L1 PM Substates or ASPM L1.2 */
	NoLTR,          /* A port lacks LTR, which L1.2 needs for its threshold */
	L1Off,          /* ASPM L1 is not enabled on both ends */
	BadTiming,      /* Reserved T_POWER_ON scale */
	Restored,       /* A readback failed, the previous configuration was restored */
	LinkLost,       /* The device stopped answering, the previous configuration was restored */
};

inline const char* describe(Result r) {
	switch (r) {
		case Result::Enabled: return "ASPM L1.2 enabled";
		case Result::AlreadyEnabled: return "ASPM L1.2 already enabled";
		case Result::Unsupported: return "unsupported: a link partner lacks ASPM L1.2";
		case Result::NoLTR: return "unsupported: a link partner lacks LTR";
		case Result::L1Off: return "not enabled: ASPM L1 is off on the link";
		case Result::BadTiming: return "not enabled: invalid T_POWER_ON scale";
		case Result::Restored: return "failed: readback differs, previous configuration restored";
		case Result::LinkLost: return "failed: device stopped answering, previous configuration restored";
	}
	return "unknown";
}

/**
 * Port needs: pcie and l1ss capability offsets, read32/write32 at any configuration offset, read16/write16
 * below 0x100. parent is the upstream end (root or switch downstream port), child the device.
 *
 * Order (PCIe r6.0 sec 5.5.4, 7.5.3.7): LTR on from the root down; ASPM off downstream first, since
 * the L1 PM Substates enables may only change while ASPM L1 is disabled; L1.2 enables off while the
 * timing is programmed; T_POWER_ON in both ports, Common_Mode_Restore_Time in the upstream port and
 * LTR_L1.2_THRESHOLD in both; ASPM L1.2 on, then ASPM back on, upstream first.
 */
template <typename Port>
Result enableASPML12(Port& parent, Port& child, Timing& t) {
	Port* ports[2] {&parent, &child};
	struct Saved { uint16_t link, dev2; uint32_t ctl1, ctl2; } saved[2];
	uint32_t caps[2];
	for (int i = 0; i < 2; i++) {
		auto& p = *ports[i];
		if (!p.pcie || !p.l1ss || (p.read16(p.pcie + PCIeFlags) & 0xF) < 2)
			return Result::Unsupported;
		caps[i] = p.read32(p.l1ss + Cap);
		if (caps[i] == UINT32_MAX || !(caps[i] & CapL1PMSS) || !(caps[i] & CapASPML12))
			return Result::Unsupported;
		auto devCap2 = p.read32(p.pcie + DevCap2);
		if (devCap2 == UINT32_MAX || !(devCap2 & DevCap2LTR))
			return Result::NoLTR;
		saved[i] = {p.read16(p.pcie + LinkCtl), p.read16(p.pcie + DevCtl2), p.read32(p.l1ss + Ctl1), p.read32(p.l1ss + Ctl2)};
		if (saved[i].link == UINT16_MAX || saved[i].dev2 == UINT16_MAX || saved[i].ctl1 == UINT32_MAX || saved[i].ctl2 == UINT32_MAX)
			return Result::Unsupported;
		if (!(saved[i].link & LinkCtlASPML1))
			return Result::L1Off;
	}
	if (!timing(caps[0], caps[1], t))
		return Result::BadTiming;

	const uint32_t parentFields = Ctl1CMRestore | Ctl1Threshold;
	auto configured = [&](const Saved& p, const Saved& c) {
		return (p.dev2 & DevCtl2LTR) && (c.dev2 & DevCtl2LTR) &&
			(p.ctl1 & Ctl1ASPML12) && (c.ctl1 & Ctl1ASPML12) &&
			(p.ctl1 & parentFields) == (t.ctl1 & parentFields) && (c.ctl1 & Ctl1Threshold) == (t.ctl1 & Ctl1Threshold) &&
			(p.ctl2 & Ctl2Mask) == t.ctl2 && (c.ctl2 & Ctl2Mask) == t.ctl2 &&
			(p.link & LinkCtlASPMMask) == (saved[0].link & LinkCtlASPMMask) &&
			(c.link & LinkCtlASPMMask) == (saved[1].link & LinkCtlASPMMask);
	};
	if (configured(saved[0], saved[1]))
		return Result::AlreadyEnabled;

	auto restore = [&]() {
		child.write16(child.pcie + LinkCtl, saved[1].link & ~LinkCtlASPMMask);
		parent.write16(parent.pcie + LinkCtl, saved[0].link & ~LinkCtlASPMMask);
		child.write32(child.l1ss + Ctl1, saved[1].ctl1 & ~Ctl1L12Mask);
		parent.write32(parent.l1ss + Ctl1, saved[0].ctl1 & ~Ctl1L12Mask);
		parent.write32(parent.l1ss + Ctl2, saved[0].ctl2);
		child.write32(child.l1ss + Ctl2, saved[1].ctl2);
		parent.write32(parent.l1ss + Ctl1, saved[0].ctl1);
		child.write32(child.l1ss + Ctl1, saved[1].ctl1);
		child.write16(child.pcie + DevCtl2, saved[1].dev2);
		parent.write16(parent.pcie + DevCtl2, saved[0].dev2);
		parent.write16(parent.pcie + LinkCtl, saved[0].link);
		child.write16(child.pcie + LinkCtl, saved[1].link);
	};

	for (auto p : ports)
		if (!(saved[p == &child].dev2 & DevCtl2LTR))
			p->write16(p->pcie + DevCtl2, saved[p == &child].dev2 | DevCtl2LTR);
	child.write16(child.pcie + LinkCtl, saved[1].link & ~LinkCtlASPMMask);
	parent.write16(parent.pcie + LinkCtl, saved[0].link & ~LinkCtlASPMMask);
	child.write32(child.l1ss + Ctl1, saved[1].ctl1 & ~Ctl1L12Mask);
	parent.write32(parent.l1ss + Ctl1, saved[0].ctl1 & ~Ctl1L12Mask);
	parent.write32(parent.l1ss + Ctl2, (saved[0].ctl2 & ~Ctl2Mask) | t.ctl2);
	child.write32(child.l1ss + Ctl2, (saved[1].ctl2 & ~Ctl2Mask) | t.ctl2);
	// PCI-PM L1.2 keeps its previous setting; only ASPM L1.2 is turned on.
	const uint32_t parentCtl1 = (saved[0].ctl1 & ~(Ctl1L12Mask | parentFields)) | (t.ctl1 & parentFields);
	const uint32_t childCtl1 = (saved[1].ctl1 & ~(Ctl1L12Mask | Ctl1Threshold)) | (t.ctl1 & Ctl1Threshold);
	parent.write32(parent.l1ss + Ctl1, parentCtl1);
	child.write32(child.l1ss + Ctl1, childCtl1);
	parent.write32(parent.l1ss + Ctl1, parentCtl1 | Ctl1ASPML12 | (saved[0].ctl1 & Ctl1PCIPML12));
	child.write32(child.l1ss + Ctl1, childCtl1 | Ctl1ASPML12 | (saved[1].ctl1 & Ctl1PCIPML12));
	parent.write16(parent.pcie + LinkCtl, saved[0].link);
	child.write16(child.pcie + LinkCtl, saved[1].link);

	Saved now[2];
	for (int i = 0; i < 2; i++) {
		auto& p = *ports[i];
		now[i] = {p.read16(p.pcie + LinkCtl), p.read16(p.pcie + DevCtl2), p.read32(p.l1ss + Ctl1), p.read32(p.l1ss + Ctl2)};
	}
	const bool answering = child.read32(0) != UINT32_MAX;
	if (answering && configured(now[0], now[1]))
		return Result::Enabled;
	restore();
	return answering ? Result::Restored : Result::LinkLost;
}

}

#endif /* pcie_l1ss_hpp */
