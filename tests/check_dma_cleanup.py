"""Exercise the production NVMeFeatures cleanup paths without a kernel or SSD."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "NVMeFix/NVMeFix.cpp").read_text()
start = source.index("IOReturn NVMeFixPlugin::NVMeFeatures(")
end = source.index("\n}", start) + 2

mock = r"""
#include <cassert>
#include <cstdint>
#include <string>
#define DBGLOG(...) ((void)0)
#define SYSLOG(...) ((void)0)
using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNoResources = 1, kIOReturnInternalError = 2;
enum Stage { Success, Buffer, Acquire, WrongType, Attach, Prepare, Segments, Submit };
struct IOBufferMemoryDescriptor {
    Stage failure;
    int active = 0;
    int prepare() { if (failure == Buffer) return 1; ++active; return 0; }
    int getLength() { return 256; }
    void complete() { assert(active-- == 1); }
};
struct OSMetaClassBase {
    bool wrongType = false;
    const void* metaCast(const char* name) const {
        assert(std::string(name) == "AppleNVMeRequest");
        return wrongType ? nullptr : this;
    }
};
struct IODMACommand : OSMetaClassBase {
    Stage failure;
    int active = 0;
    int setMemoryDescriptor(IOBufferMemoryDescriptor*, bool autoPrepare) {
        assert(!autoPrepare); return failure == Attach;
    }
    int prepare(int, int) { if (failure == Prepare) return 1; ++active; return 0; }
    void complete() { assert(active-- == 1); }
};
struct ControllerEntry { void* controller = nullptr; };
template<class T> struct Member {
    T value{};
    T& get(void*) { return value; }
};
struct Command { struct { unsigned dword11; } features; };
struct NVMeFixPlugin {
    IODMACommand dma{};
    int acquired = 0, returned = 0;
    struct {
        struct {
            struct {
                bool fptr = true;
                void* (*call)(void*, int, int);
                void* operator()(void* c,int n,int f) { return call(c,n,f); }
            } GetRequestNew;
            void* GetRequest(void*, int) { assert(false); return nullptr; }
            int ProcessSyncNVMeRequest(void*,void* r) { return static_cast<IODMACommand*>(r)->failure == Submit; }
            void ReturnRequest(void* c,void*) { ++static_cast<NVMeFixPlugin*>(c)->returned; }
        } IONVMeController;
        struct {
            void BuildCommandSetFeaturesCommon(void*,unsigned) {}
            void BuildCommandGetFeatures(void*,unsigned) {}
            int GenerateIOVMSegments(void* r,int,int) { return static_cast<IODMACommand*>(r)->failure == Segments; }
        } AppleNVMeRequest;
    } kextFuncs;
    struct { struct {
        Member<Command> command;
        Member<IOBufferMemoryDescriptor*> prpDescriptor;
        Member<void*> controller;
        Member<uint32_t> result;
    } AppleNVMeRequest; } kextMembers;
    IOReturn NVMeFeatures(ControllerEntry&,unsigned,unsigned*,IOBufferMemoryDescriptor*,uint32_t*,bool);
};
"""
checks = r"""
int main() {
    for (int f = Success; f <= Submit; ++f) {
        NVMeFixPlugin plugin{};
        plugin.dma.failure = static_cast<Stage>(f);
        IOBufferMemoryDescriptor buffer{static_cast<Stage>(f)};
        ControllerEntry entry{&plugin};
        plugin.kextFuncs.IONVMeController.GetRequestNew.call = [](void* controller,int,int)->void* {
            auto p = static_cast<NVMeFixPlugin*>(controller);
            if (p->dma.failure == Acquire) return nullptr;
            p->dma.wrongType = p->dma.failure == WrongType;
            ++p->acquired;
            return &p->dma;
        };
        unsigned word = 1;
        auto status = plugin.NVMeFeatures(entry,12,&word,&buffer,nullptr,true);
        assert((status == 0) == (f == Success));
        assert(plugin.acquired == (f != Acquire && f != Buffer));
        assert(plugin.acquired == plugin.returned && !plugin.dma.active && !buffer.active);
    }
}
"""
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "check"
    subprocess.run(["c++", "-std=c++17", "-x", "c++", "-", "-o", str(binary)],
                   input=mock + source[start:end] + checks, text=True, check=True)
    subprocess.run([binary], check=True)
