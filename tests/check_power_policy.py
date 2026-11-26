"""Run the kext's APST and active-PM paths against a simulated controller."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
source = tests.parent / "NVMeFix"
header = (source / "NVMeFixPlugin.hpp").read_text()
pm = (source / "nvme_pm.cpp").read_text()
apst = (source / "nvme_apst.cpp").read_text()
registry = (source / "NVMeFix.cpp").read_text()


def section(text, first, following):
    start = text.index(first)
    return text[start:text.index(following, start)]


with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    (work / "entry.inc").write_text(section(header, "enum class APSTState", "evector<ControllerEntry*>"))
    (work / "pm.inc").write_text(section(header, "struct PM {", "\n};\n"))
    (work / "methods.inc").write_text(
        apst[apst.index("uint64_t NVMeFixPlugin::latencyLimit"):]
        + section(pm, "bool NVMeFixPlugin::PM::init(", "OSDefineMetaClassAndStructors")
        + section(pm, "void NVMePMProxy::free()", "bool NVMeFixPlugin::PM::solveSymbols")
        + pm[pm.index("bool NVMeFixPlugin::PM::activityTickle"):]
        + section(registry, "bool NVMeFixPlugin::terminatedNotificationHandler(", "/**")
        + section(registry, "NVMeFixPlugin::ControllerEntry* NVMeFixPlugin::entryForController", "static const char *")
    )
    for config, flags in (("Release", []), ("Debug", ["-DDEBUG"])):
        binary = work / config
        subprocess.run(["clang++", "-std=c++17", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        *flags, "-I", str(source), "-I", tmp,
                        str(tests / "power_policy.cpp"), "-o", str(binary)], check=True)
        subprocess.run([binary], check=True)
        print(f"{config} power policy checks passed")
