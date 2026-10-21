# Change History

> **Note**:
>   This document is the change history. Timestamps come from the `date` command, in `YYYY-MM-DD HH:MM:SS` form.
>   Newest entries are listed first.

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
