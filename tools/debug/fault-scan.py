#!/usr/bin/env python3
"""Post-mortem for a BSOD or black screen: fault registers, stack frames, symbols.

    ./tools/debug/fault-scan.py            # uses build/gw_retro_go.elf

What it does, in one run:
  1. Reads CFSR/HFSR/BFAR/MMFAR plus the LTDC layer state over SWD.
  2. Dumps the top of the launcher stack and scans it for exception frames
     (r0-r3, r12, LR, PC, xPSR), resolving PC and LR via addr2line.
  3. Dumps 30K of stack and lists every return-address candidate - a healthy
     chain has many; ONE giant call-free gap means a huge stack local.

Reading CFSR: low byte MMFSR (0x82 = DACCVIOL+MMARVALID: MPU hit, see MMFAR -
if MMFAR is the redzone base, the stack overflowed). 0x8200 = precise bus
fault, BFAR valid. BFAR is only trustworthy if CFSR was clear before the
fault: the BSOD never clears it, so a stale latch shows the PREVIOUS address.

Hard-won context for interpreting results is in AMIGA-CORE.md and the git log
of the amiga branch.
"""
import re, struct, subprocess, sys, tempfile, os

ELF = "build/gw_retro_go.elf"
OPENOCD = "/opt/homebrew/bin/openocd"

script = """
source [find interface/cmsis-dap.cfg]
transport select swd
source [find target/stm32h7x.cfg]
adapter speed 1000
init
halt
echo "CFSR  [format 0x%08x [mrw 0xE000ED28]]"
echo "HFSR  [format 0x%08x [mrw 0xE000ED2C]]"
echo "BFAR  [format 0x%08x [mrw 0xE000ED38]]"
echo "MMFAR [format 0x%08x [mrw 0xE000ED34]]"
echo "PFCR  [format 0x%08x [mrw 0x50001094]]"
echo "CFBAR [format 0x%08x [mrw 0x500010AC]]"
dump_image {STACKBIN} 0x20018800 30720
resume
shutdown
"""

def addr2line(a):
    out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-e", ELF, hex(a)],
                         capture_output=True, text=True).stdout.strip()
    return out.replace("\n", " @ ")

def read_panic_mark():
    """If the Rust half panicked, name the exact file and line."""
    nm = subprocess.run(["arm-none-eabi-nm", ELF], capture_output=True, text=True).stdout
    m = re.search(r"^([0-9a-f]+) . AMIGA_PANIC_MARK", nm, re.M)
    if not m:
        return
    addr = int(m.group(1), 16)
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as f:
        binp = f.name + ".mark"
        f.write(f"""source [find interface/cmsis-dap.cfg]\ntransport select swd
source [find target/stm32h7x.cfg]\nadapter speed 1000\ninit\nhalt
dump_image {{{binp}}} 0x{addr:08x} 16\nresume\nshutdown\n""")
        cfg = f.name
    try:
        subprocess.run([OPENOCD, "-f", cfg], capture_output=True)
        magic, fptr, flen, line = struct.unpack("<4I", open(binp, "rb").read())
    finally:
        os.unlink(cfg)
        if os.path.exists(binp):
            os.unlink(binp)
    if magic != 0x50414E43:
        return
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as f:
        binp = f.name + ".str"
        f.write(f"""source [find interface/cmsis-dap.cfg]\ntransport select swd
source [find target/stm32h7x.cfg]\nadapter speed 1000\ninit\nhalt
dump_image {{{binp}}} 0x{fptr:08x} {min(flen, 256)}\nresume\nshutdown\n""")
        cfg = f.name
    try:
        subprocess.run([OPENOCD, "-f", cfg], capture_output=True)
        fname = open(binp, "rb").read().decode(errors="replace")
    finally:
        os.unlink(cfg)
        if os.path.exists(binp):
            os.unlink(binp)
    print(f"\nRUST PANIC at {fname}:{line}")


def main():
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as f:
        stackbin = f.name + ".stack"
        f.write(script.replace("STACKBIN", stackbin))
        cfg = f.name
    try:
        out = subprocess.run([OPENOCD, "-f", cfg], capture_output=True, text=True)
        for line in (out.stdout + out.stderr).splitlines():
            if re.match(r"(CFSR|HFSR|BFAR|MMFAR|PFCR|CFBAR)", line):
                print(line)
        d = open(stackbin, "rb").read()
    finally:
        os.unlink(cfg)
        if os.path.exists(stackbin):
            os.unlink(stackbin)

    words = struct.unpack("<%dI" % (len(d) // 4), d)
    base = 0x20018800
    print("\nexception-frame candidates:")
    for i in range(len(words) - 8):
        pc, xpsr, lr = words[i + 6], words[i + 7], words[i + 5]
        if (0x08000000 <= pc < 0x08040000 or 0x90000000 <= pc < 0x90100000) \
           and (xpsr & 0x01000000):
            print(f"  @0x{base + i*4:08X} PC=0x{pc:08X} {addr2line(pc)[:70]}")
            print(f"             LR=0x{lr:08X} {addr2line(lr)[:70]}")

    addrs = [w for w in words
             if (0x08000000 <= w < 0x08040000 or 0x90000000 <= w < 0x90100000)
             and (w & 1)]
    print(f"\nreturn-address candidates in 30K of stack: {len(addrs)}")
    for a in addrs[:20]:
        print(f"  0x{a:08X} {addr2line(a)[:80]}")

if __name__ == "__main__":
    main()
    read_panic_mark()
