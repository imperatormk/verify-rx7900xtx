# Design Specification

> **Note**: This document describes the current design. For the history of changes, see `ChangeLog.md`.

## Overview

The test copies data between host memory and the AMD Radeon RX 7900 XTX VRAM, then compares every byte. It repeats that check 10 times. The same procedure works when physical host memory is smaller than VRAM.

The target device is the HIP device whose name contains `7900 XTX`. Its architecture name is `gfx1100`, and its VRAM size is about 24 GiB.

## Files

| File | Description |
|------|-------------|
| `README.md` | Short usage guide. This document is the full specification |
| `vram_test.cpp` | VRAM test |
| `vram-test` | Binary produced by `make build` |
| `Makefile` | Build and `make run` |
| `doc/design.md` | This design specification |
| `doc/ChangeLog.md` | Change history |

## Build and run

```bash
make run                         # build and run 10 rounds (default goal)
make build                       # build vram-test
make run ROUNDS=10 CHUNK_MIB=256
make clean                       # remove vram-test and gpucore.*
```

| Variable | Default | Description |
|----------|---------|-------------|
| `ROUNDS` | 10 | Number of rounds |
| `CHUNK_MIB` | 256 | Host staging buffer size, in MiB |
| `ROCM` | `/opt/rocm` | ROCm installation prefix |
| `CXX` | `g++` | Host compiler |

The binary is invoked as `./vram-test <rounds> <chunk_mib>`. Running it with no arguments also uses 10 rounds and a 256 MiB chunk.

The only dependency is the ROCm HIP runtime (`libamdhip64`). There is no device kernel. The host uses `hipMalloc` and `hipMemcpy` only.

## One round

1. **Read the capacity.**  
   Print `totalGlobalMem` from `hipGetDeviceProperties`, and the free and total sizes from `hipMemGetInfo`. Also read sysfs `mem_info_vram_total` and `mem_info_vram_used` from the `/sys/class/drm/cardN/device` directory whose PCI bus ID matches the GPU.

2. **Copy until VRAM is full.**  
   Call `hipMalloc` against free VRAM, at most 1 GiB per allocation. On failure, shrink the request by 64 MiB and retry. Copy from the host into each block with `hipMemcpyHostToDevice` in `CHUNK_MIB` units (256 MiB by default).

3. **Confirm that VRAM is exhausted.**  
   All of the following must hold:
   - HIP free memory after allocation is at most 256 MiB.
   - Unaccounted memory, `free_before - allocated - free_after`, is at most 256 MiB.
   - sysfs free memory after allocation (`total - used`) is at most 256 MiB, and used memory is not more than 256 MiB below the allocated size.  
   The test does not use the increase in sysfs usage. After `hipDeviceReset`, the driver can keep VRAM charged while the next `hipMalloc` reuses it. The allocation loop itself continues until free memory is at most 64 MiB, or until a larger allocation no longer succeeds.

4. **Copy VRAM back to the host.**  
   Use `hipMemcpyDeviceToHost` with the same chunk size. The staging buffer is zeroed before each copy.

5. **Check for an exact match.**  
   Compare the returned chunk with the value regenerated from the same seed and offset, 8 bytes at a time.

6. **Report pass or fail.**  
   On a mismatch, write the round number, the byte offset from the start of VRAM, the expected value, and the actual value to stderr. That round fails.

7. **Repeat.**  
   At the end of the round, free the VRAM and call `hipDeviceReset` so the runtime cache releases it. The next round starts again by reading free capacity. The process succeeds only when all 10 rounds succeed. A failed round does not stop the remaining rounds.

## When host memory is smaller than VRAM

The host holds a single staging buffer. Its size starts at `CHUNK_MIB` and is reduced so that it stays within one quarter of `MemAvailable` in `/proc/meminfo` and within 1 GiB. The test fails before the first round if less than 1 MiB is available.

Byte word `i` in VRAM is defined below. `i` is the word index from the start of the buffer.

```text
z = seed + i
z = (z xor (z >> 30)) * 0xbf58476d1ce4e5b9
z = (z xor (z >> 27)) * 0x94d049bb133111eb
word = z xor (z >> 31)
```

Each round takes its seed from `getrandom`. The host does not retain the original VRAM image. On readback it rebuilds the expected bytes with the same formula and compares them with the copy.

The staging buffer is allocated with `hipHostMalloc` first. If that fails, the test falls back to `aligned_alloc`.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Every round passed |
| 1 | One or more rounds failed |
| 2 | Device not found, invalid arguments, or the host buffer could not be allocated |

## Output

Stdout reports the device name, VRAM total and free sizes, host memory total and available sizes, each round's seed, the allocated size, free memory after allocation, sysfs usage, copy progress, and the per-round result with elapsed time. On a terminal, progress updates a single line. Otherwise it prints a line about every 4 GiB. Failure details go to stderr.
