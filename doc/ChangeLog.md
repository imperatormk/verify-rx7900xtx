# Change History

> **Note**:
>   This document is the change history. Timestamps come from the `date` command, in `YYYY-MM-DD HH:MM:SS` form.
>   Newest entries are listed first.

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
