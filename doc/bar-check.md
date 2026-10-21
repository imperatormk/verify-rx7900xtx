# CPU-visible VRAM (Resizable BAR)

The VRAM copy test passes on a host where only 256 MiB of the 24 GiB VRAM is visible to the CPU. This document explains
why, how `vram-test` now reports it, and how to diagnose and fix it.

## What it is

The CPU reaches VRAM through a PCI memory window, BAR 0. Without Resizable BAR the window is 256 MiB, so only 256 MiB of
VRAM can be mapped into host memory at once. With Resizable BAR enabled the BIOS sizes the window to cover all VRAM
(32 GiB on a 24 GiB RX 7900 XTX).

`hipMemcpy` moves data with the GPU's DMA engines and never needs the window, so the copy test fills and verifies all
24 GiB either way. Software that maps VRAM into the host does need it: device memory allocated as host-accessible (for
example a ROCm/KFD allocation with the public/host-access flag, used by runtimes that keep command buffers, kernel
arguments or program code in CPU-visible VRAM) fails once that window is used up. On the affected host the kernel driver
returned `EINVAL` for every host-visible VRAM allocation we requested.

## What `vram-test` reports

At startup, after the sysfs VRAM line:

```
sysfs VRAM: used 33.48 MiB / total 23.98 GiB
CPU-visible VRAM: 256.00 MiB of 23.98 GiB, PCI BAR 0 256.00 MiB
WARNING: only 256.00 MiB of VRAM is CPU-visible: Resizable BAR is not active.
         Enable "Above 4G Decoding" and "Re-Size BAR Support" in the BIOS (CSM off).
         This copy test still passes; software that maps VRAM into the host fails.
         See doc/bar-check.md.
```

and in the JSON log:

| Field | Meaning |
|---|---|
| `cpu_visible_vram_bytes` | sysfs `mem_info_vis_vram_total` |
| `large_bar` | `true` when the CPU-visible size covers sysfs `mem_info_vram_total` |
| `pci_bar0_bytes` | size of PCI BAR 0 from `/sys/bus/pci/devices/<pci>/resource` |

The warning does not change pass or fail: the copy test still measures the copy path only.

## Data from the affected host (2026-10-21)

RX 7900 XTX, PCI `0000:03:00.0`, kernel `7.0.0-38-generic`, one round of the patched test:

| Source | Value |
|---|---|
| `mem_info_vram_total` | 25,753,026,560 bytes (23.98 GiB) |
| `mem_info_vis_vram_total` | **268,435,456 bytes (256 MiB)** |
| `/sys/bus/pci/devices/0000:03:00.0/resource`, line 1 | `0x00000000a0000000 0x00000000afffffff` = **256 MiB** |
| `dmesg` | `amdgpu 0000:03:00.0: [drm] Detected VRAM RAM=24560M, BAR=256M` |
| `vram-test 1 256` | `result: 1/1 passed`, with the warning above, `"large_bar": false` |

For comparison, an RX 7900 XT host with Resizable BAR enabled reports BAR 0 = 32 GiB and `mem_info_vis_vram_total`
equal to `mem_info_vram_total`.

## Checking by hand

```bash
P=0000:03:00.0                                   # the GPU's PCI address (lspci -D -d 1002:)
C=$(dirname $(grep -l "PCI_SLOT_NAME=$P" /sys/class/drm/card*/device/uevent))
cat $C/mem_info_vram_total $C/mem_info_vis_vram_total     # equal when Resizable BAR is active
head -1 /sys/bus/pci/devices/$P/resource                  # BAR 0: end - start + 1
dmesg | grep -i "Detected VRAM"                           # "BAR=256M" means not resized
lspci -vv -s $P | grep -A3 -i "Resizable BAR"             # capability and current size (as root)
```

## Fix

In the host BIOS/UEFI:

1. Enable **Above 4G Decoding** (also called 64-bit MMIO or Memory Mapped I/O above 4 GB).
2. Enable **Re-Size BAR Support** (Resizable BAR / Smart Access Memory), set to Auto or Enabled.
3. Boot in UEFI mode with **CSM disabled**: Resizable BAR is not available under legacy boot.
4. On servers, make sure the PCIe root port/switch above the GPU has enough 64-bit MMIO space for a 32 GiB BAR
   (some platforms need a "MMIO High Size" or similar option raised).

After rebooting, `dmesg` should show `BAR=32768M` (or at least the VRAM size), `mem_info_vis_vram_total` should equal
`mem_info_vram_total`, and `vram-test` prints no warning and writes `"large_bar": true`. If the kernel still reports
256 MiB, the window was not resized: `amdgpu` resizes BAR 0 at probe only when the platform leaves room for it, and the
`lspci -vv` "Resizable BAR" capability shows the sizes the device supports and the current one.
