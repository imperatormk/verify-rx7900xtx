# verify-rx7900xtx

VRAM test for the AMD Radeon RX 7900 XTX (`gfx1100`, about 24 GiB).

Each round fills free VRAM from host memory, copies it back, and checks that every byte matches. It then copies the first half of that VRAM onto the second half and checks that copy. `make run` does this 10 times. The test still works when host RAM is smaller than VRAM: the host keeps only a staging chunk and rebuilds the expected bytes from a per-round seed.

## Usage

Requires the ROCm HIP runtime (`/opt/rocm` by default).

```bash
make run                         # build and run 10 rounds
make run ROUNDS=10 CHUNK_MIB=256
make clean
```

| Variable | Default | Description |
|----------|---------|-------------|
| `ROUNDS` | 10 | Number of rounds |
| `CHUNK_MIB` | 256 | Host staging buffer size, in MiB |

## CPU-visible VRAM

At startup the test also reports how much VRAM the CPU can map (sysfs `mem_info_vis_vram_total`) and the size of PCI BAR 0. When it is smaller than VRAM, Resizable BAR is not active: the test prints a warning and writes `"large_bar": false`, `cpu_visible_vram_bytes` and `pci_bar0_bytes` to the JSON log. The copy test itself still passes on such a host, because `hipMemcpy` does not use that window. See [`doc/bar-check.md`](doc/bar-check.md) for diagnosis and the BIOS fix.

## One round

1. Read the VRAM capacity.
2. Allocate the free VRAM and copy a random pattern from the host until it is full.
3. Confirm that both HIP and sysfs show at most 256 MiB free.
4. Copy the VRAM contents back to the host and compare every byte with the original pattern.
5. Clear the second half, copy the first half onto it inside VRAM, and compare the second half with the first half's pattern.
6. Print pass or fail. A mismatch reports the round, byte offset, expected value, and actual value.
7. Free the device and repeat. The run passes only when every round passes. A failure does not skip the remaining rounds.

Each run also writes `vram-test-YYYYMMDD-HHMMSS.json` in the current directory. After every round the file is replaced with the JSON for the rounds finished so far, with `result` set to `"in_progress"`. The final replacement sets `result` to `"passed"` or `"failed"` and adds `exit_code`. Progress stays on the terminal. `make clean` does not delete the log.

## Limitations

The test calls the HIP host API only (`hipMalloc`, `hipMemset`, and `hipMemcpy`). It does not launch a device kernel. A mismatch or a HIP error shows that the copy path failed. It does not show whether the fault is in HIP or in a layer below it: the kernel driver, firmware, PCIe, the copy engine, or VRAM. The host check and the VRAM check name different copies. They do not name the layer.

A pass means the recorded bytes matched the seed pattern on those rounds. The test does not detect:

- Faults in compute units, shaders, or matrix instructions
- Cache behavior that appears only while a kernel is running
- Corrected ECC errors that leave the final bytes unchanged
- Engines other than this host and VRAM copy path
- Faults rarer than the rounds in the run, or the same checks with other seeds, chunk sizes, or devices
- Temperature during a copy. Sensors are read only before the round and after the copies, and temperature does not change pass or fail
- Power, clocks, fan speed, or device error counters
- Host access to VRAM through the PCI BAR. A host with a 256 MiB BAR (Resizable BAR off) passes; the startup report and `large_bar` in the log show it

## View a log

`vram-view` prints one JSON log as a terminal report: device and memory, a bar chart of host and VRAM bandwidth, one meter per copy, and the GPU temperature sensors. At the end of the report, line charts plot host bandwidth (`host → vram`, `vram → host`, and readback), VRAM copy bandwidth, and each temperature sensor across the rounds. A dashed line marks critical on the temperature chart. An O/X table then lists the host check, the VRAM check, and the round. It wraps by whole columns. The last section is a Summary: the machine, the method, whether the run found a problem, and what the log does and does not establish. It ends by stating that a failure cannot be assigned to HIP or to a layer below it. On each temperature meter, `┃` is critical and the right end is emergency. A reading past emergency extends the meter past that mark. Temperature does not change pass or fail.

```bash
make view                        # newest vram-test-*.json
make view LOG=vram-test-20261021-153843.json
./vram-view --plain file.json
```

Color is used when stdout is a terminal, unless `NO_COLOR` is set. `--plain` turns color off. `--width N` sets the report width.

| Exit code | Meaning |
|-----------|---------|
| 0 | Every round passed |
| 1 | One or more rounds failed |
| 2 | Setup failed (no device, bad arguments, or no host buffer) |

## Details

See [`doc/design.md`](doc/design.md) for the allocation rules, the byte pattern, and the output format. Change history is in [`doc/ChangeLog.md`](doc/ChangeLog.md).
