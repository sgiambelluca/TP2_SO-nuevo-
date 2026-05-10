# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

MASS OS — a bare-metal x86-64 kernel (TP2 Sistemas Operativos, ITBA) that boots on QEMU/VirtualBox/real hardware via Pure64 + BMFS. Implements memory management, processes, scheduling, and syscalls from scratch.

## Build & run

All compilation happens **inside the Docker container** `TP_SO_2` (image `agodio/itba-so-multiarch:3.1`), which provides the `x86_64-linux-gnu-gcc/ld` cross-toolchain and `nasm`. Building on the host directly will fail.

| Command | Purpose |
|---------|---------|
| `./create.sh` | One-time: pull image and create `TP_SO_2` container with cwd mounted at `/root` |
| `./compile.sh` | Build with First-Fit memory manager (default), QEMU image |
| `MM=BUDDY ./compile.sh` | Build with Buddy System memory manager |
| `./compile.sh vbox` / `./compile.sh usb` | Build VMDK / raw IMG instead of qcow2 |
| `./run.sh` | Boot QEMU on `Image/x64BareBonesImage.qcow2` (fallback to `.img`), 512MB RAM, IDE disk |
| `make clean` | Clean all subprojects |

`compile.sh` does `make clean` + `make all` inside the container, propagating `TARGET` and `MM`. The MM choice is a **compile-time switch** (`-DMM_FF` or `-DMM_BUDDY`); a single image embeds exactly one allocator. To compare allocators, rebuild and rerun.

If `./run.sh` fails with "permission denied" on the image, the container wrote it as root: `sudo chown $USER Image/x64BareBonesImage.qcow2`.

### Tests

There is no host-side test runner. The kernel ships a built-in test suite invoked from inside the booted shell:

```
testMM      # 5-test memory manager suite, expected: 20 OK / 0 FAIL
ps          # list processes
bmFPS / bmCPU / bmMEM / bmKEY  # benchmarks
```

To test both allocators, rebuild with each `MM` value and rerun `testMM`.

## Architecture

Four top-level components, each with its own Makefile, assembled into a single bootable disk image by `Image/Makefile`:

- **Bootloader/** — Pure64 + BMFS (vendored). Loads the packed kernel from disk into memory and jumps to it. Rarely touched.
- **Toolchain/ModulePacker** — Host-side tool that concatenates `kernel.bin` + userland modules into `packedKernel.bin` for the bootloader.
- **Kernel/** — The kernel proper. Linked at `0x100000` via `kernel.ld`, output as raw `binary` (not ELF). Entry: `loader.asm` → `kernelMain` → `initializeKernelBinary` → `main` (calls `scheduler_start`, never returns).
- **Userland/** — Compiled into two modules loaded by the kernel at fixed addresses:
  - `0000-sampleCodeModule.bin` → loaded at `0x400000`, this is the **shell** (the first foreground process spawned)
  - `0001-sampleDataModule.bin` → loaded at `0x500000`

The kernel heap lives at `0x600000` and is 8 MB (see `HEAP_START`/`HEAP_SIZE` in `Kernel/c/kernel.c`).

### Boot flow

`loader.asm` → `kernelMain` (sets up GDT/segments) → `initializeKernelBinary` in `kernel.c`:
1. `loadModules` copies userland modules from end-of-kernel to their fixed load addresses.
2. `clearBSS`, `load_idt`.
3. `mm_init(HEAP_START, HEAP_SIZE)`.
4. `process_init`, `scheduler_init`.
5. Creates `idle` process (PID 0, just `hlt`s) and `shell` process (foreground, entry = `0x400000`).
6. Returns; assembly then calls `main()` → `scheduler_start()` → `scheduler_start_asm` (loads first PCB's `rsp`, `popState`, `iretq` into userland).

### Memory manager

`Kernel/include/memoryManager.h` defines a stable interface; `Kernel/c/memoryManager/` has two implementations selected by the `MM` make variable:

- `memoryManagerFF.c` — First-Fit free list
- `memoryManagerBuddy.c` — Buddy system (power-of-two splits/coalesces)

Both expose `mm_init / mm_malloc / mm_malloc_kernel / mm_free / mm_status`. The `_kernel` variant excludes the allocation from `alloc_count` so `mm_status` reflects only userland-visible allocations made via `sys_malloc`.

### Processes & scheduler

- `process.c` — fixed-size PCB table (`MAX_PROCESSES = 64`), 4 KB stacks (`mm_malloc`'d), priorities 1–5, states `READY/RUNNING/BLOCKED/ZOMBIE`. PIDs, fd[2] (stdin/stdout), parent_pid, waitpid support.
- `scheduler.c` — round-robin with priority-derived quanta. `scheduler_tick` is called from the timer IRQ (irq00) in `interrupts.asm`; cooperative yield via `int 0x80` with `force_switch`. Both handlers in `interrupts.asm` save/restore full register state on the process's stack and pass `rsp` to/from C.
- The scheduler is the kernel's main loop — `scheduler_start_asm` is the only path that ever leaves the kernel into userland.

### Syscalls

`Kernel/include/syscallDispatcher.h` lists all 29 syscalls (`CANT_SYS = 29` in `defs.h`). Dispatched via `int 0x80` through `syscalls[]` table in `syscallDispatcher.c`. Userland calls them via `Userland/asm/userlib.asm` wrappers and `Userland/c/userlib.c` C wrappers. Adding a syscall requires: bumping `CANT_SYS`, writing the `sys_*` function, adding it to the dispatcher table, and exposing a wrapper in `userlib`.

### Userland linkage

Userland code is freestanding and links against only `userlib` — no libc. Each user "program" is built as a flat binary loaded at a fixed address, with `_loader.c` calling `main`. Currently only the shell exists as a real user binary; `testMM` and other shell commands run **in-process inside the shell** (same address space as the shell's userland module), not as separate processes — they call into shared userland helpers via direct function calls plus syscalls.

## Conventions

- Code, comments, and commit messages are in **Spanish** (the repo is a course assignment at ITBA). Match this style when editing.
- Kernel C is compiled `-ffreestanding -nostdlib -mno-red-zone -fno-pie -std=c99` with strict warnings (`-Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes`). No SSE/MMX/floating-point. Don't pull in libc.
- Headers are always in `Kernel/include/` or `Userland/c/include/`; sources include them as `#include "foo.h"` (the Makefiles add `-I./include` / `-Ic/include`).
- `make clean` is invoked before every `compile.sh` run, so incremental builds across MM changes are safe.
