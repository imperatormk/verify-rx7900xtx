# Design Specification

> **Note**: This document describes the current design. For the history of changes, see `ChangeLog.md`.

## Overview

The test copies data between host memory and the AMD Radeon RX 7900 XTX VRAM, then compares every byte. It then copies the first half of that VRAM onto the second half and compares the copy. It repeats both checks 10 times. The same procedure works when physical host memory is smaller than VRAM.

The target device is the HIP device whose name contains `7900 XTX`. Its architecture name is `gfx1100`, and its VRAM size is about 24 GiB.

## Files

| File | Description |
|------|-------------|
| `README.md` | Short usage guide. This document is the full specification |
| `vram_test.cpp` | VRAM test |
| `vram-test` | Test binary produced by `make build` |
| `vram_view.cpp` | Terminal viewer for a JSON log |
| `vram-view` | Viewer binary produced by `make build` |
| `Makefile` | Build, `make run`, and `make view` |
| `doc/design.md` | This design specification |
| `doc/ChangeLog.md` | Change history |

## Build and run

```bash
make run                         # build and run 10 rounds (default goal)
make build                       # build vram-test and vram-view
make run ROUNDS=10 CHUNK_MIB=256
make view                        # show the newest JSON log
make view LOG=vram-test-20261021-153843.json
make clean                       # remove the binaries, gpucore.*, and a leftover .json.tmp
```

| Variable | Default | Description |
|----------|---------|-------------|
| `ROUNDS` | 10 | Number of rounds |
| `CHUNK_MIB` | 256 | Host staging buffer size, in MiB |
| `ROCM` | `/opt/rocm` | ROCm installation prefix |
| `CXX` | `g++` | Host compiler |

The binary is invoked as `./vram-test <rounds> <chunk_mib>`. Running it with no arguments also uses 10 rounds and a 256 MiB chunk.

The only dependency is the ROCm HIP runtime (`libamdhip64`). There is no device kernel. The host uses `hipMalloc`, `hipMemset`, and `hipMemcpy` only.

## One round

1. **Read the capacity.**  
   Print `totalGlobalMem` from `hipGetDeviceProperties`, and the free and total sizes from `hipMemGetInfo`. Also read sysfs `mem_info_vram_total` and `mem_info_vram_used` from the `/sys/class/drm/cardN/device` directory whose PCI bus ID matches the GPU.  
   At startup only, also read `mem_info_vis_vram_total` (the CPU-visible part of VRAM) from the same directory and the size of PCI BAR 0 from the first line of its `resource` file, and print both. When the CPU-visible size is below `mem_info_vram_total`, print a warning that Resizable BAR is not active. The JSON log records `cpu_visible_vram_bytes`, `large_bar` and `pci_bar0_bytes`. This does not change pass or fail. See `bar-check.md`.

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

5. **Check the host copy.**  
   Compare the returned chunk with the value regenerated from the same seed and offset, 8 bytes at a time.

6. **Copy inside VRAM.**  
   Split the allocation in half on an 8-byte boundary. Clear the second half with `hipMemset`, then copy the first half onto it with `hipMemcpyDeviceToDevice`. A copy stops at the next block boundary and continues in the following block. The two halves do not overlap.

7. **Check the VRAM copy.**  
   Read the second half back in the same chunk size. Before each read, zero the staging buffer. Compare every byte with the pattern of the matching offset in the first half, not with the pattern of the destination offset.

8. **Report pass or fail.**  
   On a mismatch, write the round number, the byte offset from the start of the allocation, the expected value, and the actual value to stderr. A VRAM-copy mismatch also includes the source offset. That round fails.

9. **Repeat.**  
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

The same run writes `vram-test-YYYYMMDD-HHMMSS.json` in the current directory. The name uses the local time at start. The file is one JSON object and omits progress lines. It records the time, hostname, kernel, HIP version, device name, architecture, PCI address, VRAM and host memory sizes in bytes, the round count, and the chunk size. `tests` is an array of round objects. A passing round has `result` `"pass"`, the seed as a hex string, the allocated size, free memory after allocation, and `host` and `vram` objects with seconds and GiB/s. A failing round has `result` `"fail"`, an `error` string, and, when bytes differ, a `mismatch` object with `offset`, `expected`, and `actual`. A VRAM-copy mismatch also has `source_offset`. Pattern words are hex strings because a JSON number cannot hold every 64-bit value. The object ends with `passed`, `failed`, and `result`.

After each round the program writes that object, including only the rounds finished so far, to `vram-test-YYYYMMDD-HHMMSS.json.tmp` in the same directory, flushes it, and `rename`s it over the destination. `result` is `"in_progress"` and `exit_code` is omitted. When every round has finished, or when setup fails before the first round, one more replacement sets `result` to `"passed"`, `"failed"`, or `"setup_failed"` and adds `exit_code`. A reader therefore sees either the previous complete document or the new one. `make clean` deletes a leftover temporary file and leaves the JSON log in place.

Each round also reads the GPU temperature sensors from the `amdgpu` hwmon node under the same sysfs device. The hwmon directory name is not fixed, so the test selects the node whose `name` file is `amdgpu`. It records every `tempN_input` with its `tempN_label`, plus `tempN_crit` and `tempN_emergency` when those files exist. Values are integers in millidegrees Celsius. The test reads them at the start of the round and again after the copies, including when the round fails. On this GPU the labels are `edge`, `junction`, and `mem`. A missing sensor file omits that field. Temperature does not change the pass or fail result.

In the round object, `temperature` is an array. Each element has `name`, `before_millicelsius`, `after_millicelsius` when that sample exists, and `critical_millicelsius` and `emergency_millicelsius` when the driver publishes them.

`vram-view` reads one of these files and draws it for a person. With no file argument it picks the newest `vram-test-*.json` in the current directory. The report shows the device, how much of VRAM the run allocated, host memory in use, and the copy rates. Host transfers (`host → vram`, `vram → host`, and the VRAM-copy readback) share one vertical chart and one meter scale. The `vram → vram` rate uses a separate, higher scale. A failing round prints its `error` and, when present, the mismatch offset and the expected and actual words. At the end of the report, line charts draw host bandwidth and VRAM copy bandwidth across the rounds, then each temperature sensor from the first sample of the run to the last. The horizontal axis is the round number, because the log does not store a timestamp for each sample. Host rates share one scale. The VRAM copy rate uses its own scale. A dashed line marks each critical temperature. After the charts, an O/X table lists every round: `O` is pass and `X` is fail, for the host check, the VRAM check, and the round. The table wraps between columns so a round number and its marks stay on one line. The report ends with a Summary in prose. It states the host, kernel, HIP runtime, device, and memory sizes from the log; the fill, compare, and VRAM-to-VRAM method; whether any recorded round failed; and what those results establish, including bandwidth and temperature when the log has them. It also states what the log does not establish, such as temperature during a copy and any round that was not written. The Summary ends with the limit of the method: a mismatch or a HIP error can show that the copy path failed, but the log cannot tell whether the fault is in HIP or below it, in the kernel driver, firmware, PCIe, the copy engine, or VRAM. The host check and the VRAM check name different copies, not the layer. Compute units, shaders, matrix instructions, caches used only by a kernel, corrected ECC errors that leave the final bytes unchanged, engines other than this copy path, and faults rarer than the recorded rounds are outside the result. The Summary wraps on word boundaries within the report width. For each temperature sensor it also draws a meter from 0°C to emergency. `┃` marks critical and the right end marks emergency. The line also states the gap, such as `54.0°C under critical`. At or above critical the filled part turns red, the part above critical uses a second glyph, and the line says `OVER critical by`. At or above emergency the scale grows to the reading so the overflow is drawn past an interior emergency mark, and the line says `OVER emergency by`. A log with no `temperature` array omits the meters. `--plain` disables color. Color is otherwise on when stdout is a terminal and `NO_COLOR` is unset.
