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

## One round

1. Read the VRAM capacity.
2. Allocate the free VRAM and copy a random pattern from the host until it is full.
3. Confirm that both HIP and sysfs show at most 256 MiB free.
4. Copy the VRAM contents back to the host and compare every byte with the original pattern.
5. Clear the second half, copy the first half onto it inside VRAM, and compare the second half with the first half's pattern.
6. Print pass or fail. A mismatch reports the round, byte offset, expected value, and actual value.
7. Free the device and repeat. The run passes only when every round passes. A failure does not skip the remaining rounds.

Each run also writes `vram-test-YYYYMMDD-HHMMSS.log` in the current directory. The file is a few kilobytes: host, GPU, one line per round, and the final pass count. Progress stays on the terminal. `make clean` does not delete the log.

| Exit code | Meaning |
|-----------|---------|
| 0 | Every round passed |
| 1 | One or more rounds failed |
| 2 | Setup failed (no device, bad arguments, or no host buffer) |

## Details

See [`doc/design.md`](doc/design.md) for the allocation rules, the byte pattern, and the output format. Change history is in [`doc/ChangeLog.md`](doc/ChangeLog.md).
