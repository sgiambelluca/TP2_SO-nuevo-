# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Canonical guidance lives in AGENTS.md

This repo's full build/run instructions, architecture, syscall-addition recipe, testing
notes, and the **critical invariants** (Ctrl+C / ZOMBIE reaping, scheduler priority &
aging, semaphores, MVar) are maintained in `AGENTS.md`. It is the single source of truth —
read it before changing kernel internals, and keep it updated when those invariants change.

@AGENTS.md

## Orientation not covered in AGENTS.md

- **You are on Windows.** `create.sh` / `compile.sh` / `run.sh` are bash + Docker scripts.
  Run them via the Bash tool (Git Bash), not PowerShell. Docker Desktop must be running.
  Never run `make` on the host — it only works inside the `TP_SO_2` container (see AGENTS.md).
- **Design rationale docs** live in `Docs/` — read these when touching the matching subsystem:
  `informe-scheduler-aging.md`, `informe-semaphores.md`, `informe-mvar-handoff.md`,
  `informe-mvar-userland.md`, plus `Docs/TP2-Enunciado.md` (the assignment spec) and
  `Docs/TODO.md`. The user-facing command reference is in `README.md`.
- **`Bootloader/` is vendored** (Pure64 + BMFS, third-party). Don't modify it; only the
  kernel/userland/toolchain layers are project code.
- **Language:** code, comments, and commit messages are in **Spanish**.
