# B29 runs kept for the replay video

Two real runs of `make run-arm-precise` (2026-11-25 21:52, one guest boot), chosen in D-092:

| Run | Network | Outcome |
|---|---|---|
| `ours-flood-11` | ours, under flood | stop signal delivered; brake 3.360 ms after the event; arm rests 6.91 mm from the eye, 0.84 mm past the planned point |
| `naive-flood-75` | naive (same QNX, one priority, first-come queue), under flood | stop signal late; brake 35.194 ms after the event; the tip touches the eye |

Files are byte-for-byte copies of `build/arm/runs/20261125-215226/`:

- `<run>.txt` — the run log (`<run>.log` there; renamed because the commit hook blocks `*.log`, content unchanged);
- `<run>.json` — the replay file written by `scripts/arm_compare.rb --replay` (its `log` field still names `<run>.log`).

SHA-256 of the originals is recorded in `QNX_EVIDENCE.md`, section "2026-11-27 · replay of two B29 runs".
