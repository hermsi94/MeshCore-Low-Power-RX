"""Compile the actual C++ policy for Cortex-M4 and run it under Unicorn.

Requires pyelftools and unicorn==2.1.4. No hardware peripherals are simulated.
"""
import argparse
import os
from pathlib import Path
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC, UC_CPU_ARM_CORTEX_M4

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPILER = ROOT.parent / ".tools/platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-g++.exe"
parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default=os.environ.get("ARM_CXX", str(DEFAULT_COMPILER)))
args = parser.parse_args()
output = ROOT / "out/power-tests"
output.mkdir(parents=True, exist_ok=True)
elf_path = output / "power_tests.elf"
subprocess.run([
    args.compiler, "-std=c++11", "-Os", "-mcpu=cortex-m4", "-mthumb", "-fno-exceptions",
    "-fno-rtti", "-nostdlib", "-Wl,-Ttext=0x10000", "-Wl,-e,test_boot_and_trust",
    "-I" + str(ROOT / "tests/radio_mocks"),
    str(ROOT / "tests/power_manager_tests.cpp"), str(ROOT / "tests/radio_power_tests.cpp"),
    "-lgcc", "-o", str(elf_path),
], check=True)

with elf_path.open("rb") as stream:
    elf = ELFFile(stream)
    segments = [(s["p_vaddr"], s["p_memsz"], s.data()) for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
    tests = sorted((s.name, s["st_value"]) for s in elf.get_section_by_name(".symtab").iter_symbols()
                   if s.name.startswith("test_") and s["st_info"]["type"] == "STT_FUNC")

for name, entry in tests:
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
    lower = min(addr for addr, _, _ in segments) & ~4095
    upper = (max(addr + length for addr, length, _ in segments) + 4095) & ~4095
    cpu.mem_map(lower, upper - lower)
    for addr, _, data in segments:
        cpu.mem_write(addr, data)
    cpu.mem_map(0x20000000, 0x40000)
    cpu.mem_map(0x800000, 4096)
    cpu.reg_write(UC_ARM_REG_SP, 0x2003FFF0)
    cpu.reg_write(UC_ARM_REG_LR, 0x800001)
    cpu.emu_start(entry | 1, 0x800000, timeout=10_000_000, count=10_000_000)
    if cpu.reg_read(UC_ARM_REG_PC) != 0x800000:
        raise SystemExit(f"FAIL {name}: test did not return before the execution limit")
    result = cpu.reg_read(UC_ARM_REG_R0)
    if result:
        raise SystemExit(f"FAIL {name}: C++ source line {result}")
    print(f"PASS {name}")
print(f"{len(tests)} C++ tests passed on Cortex-M4 emulation.")
