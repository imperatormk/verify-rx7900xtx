# Change History

> **Note**:
>   This document is the change history. Timestamps come from the `date` command, in `YYYY-MM-DD HH:MM:SS` form.
>   Newest entries are listed first.

## 2026-10-21 19:46:16 — Report CPU-visible VRAM and the PCI BAR 0 size

- At startup `vram-test` reads sysfs `mem_info_vis_vram_total` and the size of PCI BAR 0, prints them, and warns when the CPU-visible size is below VRAM (Resizable BAR not active)
- The JSON log gains `cpu_visible_vram_bytes`, `large_bar` and `pci_bar0_bytes`. Pass and fail are unchanged: the copy test does not use the BAR window
- `doc/bar-check.md` explains the check, records the data from a 256 MiB BAR host, and gives the BIOS fix

## 2026-10-21 18:40:56 — State the layer limit at the end of the Summary

- The Summary ends by stating that a mismatch or a HIP error cannot separate a HIP fault from the kernel driver, firmware, PCIe, the copy engine, or VRAM
- The host check and the VRAM check name different copies, not the layer. Compute paths, corrected ECC, other engines, and rarer faults are outside the result

## 2026-10-21 18:25:51 — Add a Summary to the end of the report

- `vram-view` ends with a Summary in prose: the machine, the method, whether a failure was recorded, and what the log does and does not establish
- The Summary wraps on word boundaries within the report width

## 2026-10-21 18:10:14 — List every round as an O/X table

- The last block of `vram-view` is a table of host, VRAM, and round results
- `O` is pass and `X` is fail. The table wraps between columns so a round is not split

## 2026-10-21 18:03:18 — Plot bandwidth across the rounds

- `vram-view` draws host bandwidth and VRAM copy bandwidth as line charts at the end of the report, before the temperature chart
- Host rates share one scale. The VRAM copy rate uses its own scale
- The horizontal axis is the round number

## 2026-10-21 17:58:28 — Show the temperature chart at the end of the report

- `vram-view` prints the temperature line chart after the bandwidth summary and the per-round meters

## 2026-10-21 16:40:55 — Plot temperature from the start of the run to the end

- `vram-view` draws one line per sensor through the before and after sample of every round
- The horizontal axis is the round number. Dashed lines mark critical
- A log with no temperature samples omits the chart

## 2026-10-21 16:12:49 — Record GPU temperatures and show the thresholds

- Each round records edge, junction, and mem at the start and after the copies, in millidegrees Celsius, with critical and emergency limits
- `vram-view` draws a meter per sensor. `┃` is critical and the right end is emergency. A reading past a limit is marked on the meter and in the text
- Temperature does not change pass or fail

## 2026-10-21 15:52:06 — Add a terminal viewer for the JSON log

- `vram-view` draws a JSON log with allocation meters and bandwidth charts
- `make view` shows the newest log. `LOG=` selects a file. `--plain` turns color off
- `README.md` and `doc/design.md` describe the viewer

## 2026-10-21 15:38:34 — Replace the JSON log after every round

- After each round, write the rounds finished so far to a temporary file and `rename` it over the JSON log
- `result` stays `"in_progress"` until the run finishes, when it becomes `"passed"`, `"failed"`, or `"setup_failed"` and `exit_code` is added
- `make clean` removes a leftover temporary file and keeps the JSON log

## 2026-10-21 15:13:20 — Write the result log as JSON

- The result file is now `vram-test-YYYYMMDD-HHMMSS.json`
- Each round is an object. Mismatches include numeric offsets and hex expected and actual values
- `README.md` and `doc/design.md` describe the JSON object

## 2026-10-21 14:52:10 — Write a compact result log

- Each run writes `vram-test-YYYYMMDD-HHMMSS.log` with the host, GPU, one line per round, and the final result
- Progress stays on the terminal. `make clean` does not delete the log
- `README.md` and `doc/design.md` describe the file

## 2026-10-21 14:25:24 — Add a VRAM-to-VRAM copy check

- After the host copy check, each round clears the second half of the allocation and copies the first half onto it with `hipMemcpyDeviceToDevice`
- The second half is read back and compared with the first half's pattern
- `README.md` and `doc/design.md` describe the extra steps

## 2026-10-21 13:57:35 — Add a short README

- Added `README.md` with a short version of the usage and the per-round check
- Listed `README.md` in `doc/design.md`. The full procedure stays in that document

## 2026-10-21 13:54:07 — Use English for all repository text

- Rewrote user-facing messages, comments, `doc/design.md`, and this history in English
- Left the VRAM test procedure unchanged

## 2026-10-21 13:50:06 — Add the RX 7900 XTX VRAM test

- Added `vram_test.cpp`, which checks capacity, fills free VRAM, confirms it is exhausted, copies the data back, and compares every byte, for 10 rounds
- When host memory is smaller than VRAM, the test copies a pattern that can be rebuilt from the seed and the offset, one chunk at a time
- `make run` builds and runs the test. `ROUNDS` and `CHUNK_MIB` set the round count and the chunk size
- Documented the procedure, pass criteria, and exit codes in `doc/design.md`
