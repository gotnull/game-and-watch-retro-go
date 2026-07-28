#!/usr/bin/env bash
# Full flash cycle for the amiga branch. Order matters and each step exists
# for a reason - see AMIGA-CORE.md "Flashing".
set -euo pipefail
cd "$(dirname "$0")/../.."

GANDW=$HOME/development/gandw
OCD=/opt/homebrew/bin/openocd

# 1. Erase internal flash. gnwmanager's RAM helper is written over SWD, which
#    bypasses the CPU caches; if cache-enabling firmware is running the helper
#    executes stale lines and dies. Erasing first means nothing runs.
$OCD -c "source [find interface/cmsis-dap.cfg]" -c "transport select swd" \
     -c "source [find target/stm32h7x.cfg]" -c "adapter speed 1000" -c init \
     -c "reset halt" -c "flash erase_sector 0 0 last" -c "reset halt" -c exit \
     2>&1 | grep -E "erased|Error"

# 2. External then internal, both via gnwmanager (the intflash image uses the
#    undocumented 256K bank; OpenOCD writes half and still says Verified OK).
OPENOCD=$GANDW/tools/openocd-cmsisdap $GANDW/tools/gnw flash ext build/gw_retro_go_extflash.bin
OPENOCD=$GANDW/tools/openocd-cmsisdap $GANDW/tools/gnw flash 0x08000000 build/gw_retro_go_intflash.bin -- start 0x08000000
