// ASPM L1.2 sequence against simulated configuration spaces of a root port and an SSD.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "pcie_l1ss.hpp"

using namespace L1SS;

struct Link;
struct Port {
    uint16_t pcie = 0x40, l1ss = 0x200;
    uint8_t space[0x1000] {};
    Link* link = nullptr;
    bool dead = false, ignoreL12 = false;
    uint32_t read32(uint16_t off) { uint32_t v; memcpy(&v, space + off, 4); return dead ? UINT32_MAX : v; }
    uint16_t read16(uint16_t off) { uint16_t v; memcpy(&v, space + off, 2); return dead ? UINT16_MAX : v; }
    void write32(uint16_t off, uint32_t v);
    void write16(uint16_t off, uint16_t v);
};

struct Link {
    Port parent, child;
    std::vector<int> l12Enables, ltrEnables, aspmOff, aspmOn;
    bool dieOnL12 = false;
    Link() {
        parent.link = child.link = this;
        for (auto p : {&parent, &child}) {
            uint16_t flags = 2;              // PCIe capability version 2
            memcpy(p->space + p->pcie + PCIeFlags, &flags, 2);
            uint32_t devCap2 = DevCap2LTR;
            memcpy(p->space + p->pcie + DevCap2, &devCap2, 4);
            uint16_t linkCtl = LinkCtlASPML1; // ASPM L1 on, as the firmware left it
            memcpy(p->space + p->pcie + LinkCtl, &linkCtl, 2);
            uint32_t id = 0x500615b7;
            memcpy(p->space, &id, 4);
        }
        // Root port: all substates, Common_Mode_Restore_Time 40 us, T_POWER_ON 5 x 10 us
        uint32_t parentCap = 0x1F | 40U << 8 | 1U << 16 | 5U << 19;
        // SSD: ASPM and PCI-PM L1.2 only, restore 10 us, T_POWER_ON 1 x 100 us
        uint32_t childCap = 0x15 | 10U << 8 | 2U << 16 | 1U << 19;
        memcpy(parent.space + parent.l1ss + Cap, &parentCap, 4);
        memcpy(child.space + child.l1ss + Cap, &childCap, 4);
    }
    int index(Port* p) { return p == &child; }
    bool aspmOffBoth() {
        // A device that stopped answering reads all ones and cannot hold the link in L1.
        return !(parent.read16(parent.pcie + LinkCtl) & LinkCtlASPMMask) &&
               (child.dead || !(child.read16(child.pcie + LinkCtl) & LinkCtlASPMMask));
    }
};

void Port::write32(uint16_t off, uint32_t v) {
    if (dead) return;
    if (off == l1ss + Ctl1) {
        uint32_t old = read32(off);
        // PCIe r6.0 sec 5.5.4: the L1 PM Substates enables only change while ASPM L1 is off on both ends.
        if ((old ^ v) & Ctl1ASPML12) {
            assert(link->aspmOffBoth());
            if (v & Ctl1ASPML12) {
                link->l12Enables.push_back(link->index(this));
                // The threshold and T_POWER_ON must be programmed before the enable.
                assert((read32(l1ss + Ctl2) & Ctl2Mask) != 0);
                if (ignoreL12) v &= ~Ctl1ASPML12;
            }
        }
    }
    memcpy(space + off, &v, 4);
}

void Port::write16(uint16_t off, uint16_t v) {
    if (dead) return;
    if (off == pcie + DevCtl2 && (v & DevCtl2LTR) && !(read16(off) & DevCtl2LTR))
        link->ltrEnables.push_back(link->index(this));
    if (off == pcie + LinkCtl) {
        auto old = read16(off) & LinkCtlASPMMask;
        if (old && !(v & LinkCtlASPMMask)) link->aspmOff.push_back(link->index(this));
        if (!old && (v & LinkCtlASPMMask)) {
            link->aspmOn.push_back(link->index(this));
            // An SSD whose CLKREQ# is not routed stops answering once L1.2 is reachable.
            if (link->dieOnL12 && this == &link->child && (read32(l1ss + Ctl1) & Ctl1ASPML12)) {
                memcpy(space + off, &v, 2);
                dead = true;
                return;
            }
        }
    }
    memcpy(space + off, &v, 2);
}

int main() {
    // Linux' threshold encoding: 1, 32, 1024, ... ns units with a 10-bit value, rounded up.
    uint32_t scale, value;
    encodeThreshold(1, scale, value);
    assert(scale == 0 && value == 1000);
    encodeThreshold(146, scale, value);
    assert(scale == 2 && value == 143);
    encodeThreshold(UINT32_MAX / 1000, scale, value);
    assert(scale == 5 && value <= 0x3FF);
    assert(powerOnUs(0, 7) == 14 && powerOnUs(1, 5) == 50 && powerOnUs(2, 1) == 100 && powerOnUs(3, 1) == 0);

    {
        Link link;
        Timing t;
        assert(enableASPML12(link.parent, link.child, t) == Result::Enabled);
        // Larger restore time (40) and T_POWER_ON (100 us, the SSD's), threshold 2 + 4 + 40 + 100 us.
        assert(t.commonModeUs == 40 && t.powerOnUs == 100 && t.thresholdUs == 146);
        assert(t.ctl2 == (2U | 1U << 3) && t.ctl1 == (40U << 8 | 143U << 16 | 2U << 29));
        assert(link.parent.read32(link.parent.l1ss + Ctl1) == (t.ctl1 | Ctl1ASPML12));
        // The device keeps its own restore time field; only the threshold is programmed there.
        assert(link.child.read32(link.child.l1ss + Ctl1) == ((t.ctl1 & Ctl1Threshold) | Ctl1ASPML12));
        assert(link.parent.read32(link.parent.l1ss + Ctl2) == t.ctl2 && link.child.read32(link.child.l1ss + Ctl2) == t.ctl2);
        // LTR from the root down, ASPM off downstream first, L1.2 and ASPM on upstream first.
        assert((link.ltrEnables == std::vector<int>{0, 1}));
        assert((link.aspmOff == std::vector<int>{1, 0}));
        assert((link.l12Enables == std::vector<int>{0, 1}));
        assert((link.aspmOn == std::vector<int>{0, 1}));
        assert((link.parent.read16(link.parent.pcie + LinkCtl) & LinkCtlASPMMask) == LinkCtlASPML1);
        // A second run (after wake) finds nothing to change.
        auto writes = link.l12Enables.size();
        assert(enableASPML12(link.parent, link.child, t) == Result::AlreadyEnabled && link.l12Enables.size() == writes);
        // Settings lost in a power transition are programmed again.
        uint32_t zero = 0;
        memcpy(link.child.space + link.child.l1ss + Ctl1, &zero, 4);
        assert(enableASPML12(link.parent, link.child, t) == Result::Enabled);
    }
    {
        // A port that ignores the enable: everything is put back as it was.
        Link link;
        link.child.ignoreL12 = true;
        Link reference;
        Timing t;
        assert(enableASPML12(link.parent, link.child, t) == Result::Restored);
        assert(!memcmp(link.parent.space, reference.parent.space, sizeof(link.parent.space)));
        assert(!memcmp(link.child.space, reference.child.space, sizeof(link.child.space)));
    }
    {
        // The SSD drops off the bus: the root port is restored and the result says so.
        Link link;
        link.dieOnL12 = true;
        Link reference;
        Timing t;
        assert(enableASPML12(link.parent, link.child, t) == Result::LinkLost);
        assert(!memcmp(link.parent.space, reference.parent.space, sizeof(link.parent.space)));
    }
    {
        // Nothing is written when a precondition is missing.
        Link link;
        uint32_t noLTR = 0;
        memcpy(link.child.space + link.child.pcie + DevCap2, &noLTR, 4);
        Link reference;
        memcpy(reference.child.space + reference.child.pcie + DevCap2, &noLTR, 4);
        Timing t;
        assert(enableASPML12(link.parent, link.child, t) == Result::NoLTR);
        assert(!memcmp(link.child.space, reference.child.space, sizeof(link.child.space)));

        Link l1off;
        uint16_t off = 0;
        memcpy(l1off.child.space + l1off.child.pcie + LinkCtl, &off, 2);
        assert(enableASPML12(l1off.parent, l1off.child, t) == Result::L1Off && l1off.l12Enables.empty());

        Link noL12;
        uint32_t cap = 0x11; // L1 PM Substates with PCI-PM L1.2 only
        memcpy(noL12.child.space + noL12.child.l1ss + Cap, &cap, 4);
        assert(enableASPML12(noL12.parent, noL12.child, t) == Result::Unsupported);

        Link reserved;
        cap = 0x15 | 3U << 16;
        memcpy(reserved.child.space + reserved.child.l1ss + Cap, &cap, 4);
        assert(enableASPML12(reserved.parent, reserved.child, t) == Result::BadTiming && reserved.ltrEnables.empty());
    }
}
