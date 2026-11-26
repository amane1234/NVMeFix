"""Run the ASPM L1.2 register sequence against simulated root port and SSD configuration spaces."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
source = tests.parent / "NVMeFix"

with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / "l1ss"
    subprocess.run(["clang++", "-std=c++14", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                    "-I", str(source), str(tests / "l1ss.cpp"), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True)
    print("ASPM L1.2 checks passed")
