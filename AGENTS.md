# AGENTS.md — MASS OS

Bare-metal x86-64 kernel (ITBA SO TP2). Builds inside a Docker container; runs on QEMU/VirtualBox/real hardware via Pure64+BMFS.

## Build & run (Docker required)

- **One-time setup:** `./create.sh`  
  Creates container `TP_SO_2` from `agodio/itba-so-multiarch:3.1`.
- **Compile:** `./compile.sh`  
  Runs `make clean all` **inside** the container. Do **not** run `make` directly on the host.
- **Memory manager switch:** `MM=BUDDY ./compile.sh` (default is First-Fit).  
  Compile-time switch (`-DMM_FF` / `-DMM_BUDDY`); a single image embeds exactly one allocator. To compare both, rebuild and rerun.
- **Alias:** `make buddy` in the container is equivalent to `make MM=BUDDY all`.
- **Run:** `./run.sh`  
  Boots QEMU with `Image/x64BareBonesImage.qcow2` (fallback `.img`), 512 MB RAM.
- **Permission fix:** If `./run.sh` fails with "permission denied", the container wrote the image as root:  
  `sudo chown $USER Image/x64BareBonesImage.qcow2`

## Architecture

Four top-level components, each with its own Makefile, assembled into one bootable disk image by `Image/Makefile`:

- `Bootloader/` — Pure64 + BMFS (vendored). Loads packed kernel from disk.
- `Toolchain/ModulePacker` — Host-side tool that concatenates `kernel.bin` + userland modules into `packedKernel.bin`.
- `Kernel/` — Kernel proper. Linked at `0x100000` via `kernel.ld`, output as raw binary (not ELF). Entry: `loader.asm` -> `kernelMain` -> `initializeKernelBinary` -> `main` -> `scheduler_start` (never returns).
- `Userland/` — Two flat binary modules loaded by the kernel at fixed addresses:
  - `0000-sampleCodeModule.bin` -> `0x400000` (the shell + all user code)
  - `0001-sampleDataModule.bin` -> `0x500000`

Kernel heap: `0x600000`, 8 MB (`HEAP_START` / `HEAP_SIZE` in `Kernel/c/kernel/kernel.c`).

**Kernel-internal allocations** (process stacks, argv copies) must use `mm_malloc_kernel()`, which is tracked separately from user `mm_malloc()` in `MemStatus.used_kernel`.

**MVar** (multiple-reader/writer synchronization): writers block in `wq` when FULL, readers block in `rq` when EMPTY. Unblocking is **priority-based** (`wq_pop_highest` / `rq_pop_highest`) with an anti-starvation cooldown — do not revert to FIFO `wq_pop` / `rq_pop`.

## Adding a user-space command

All user code lives in the **single** `0000-sampleCodeModule.bin`. To add a new standalone command (e.g. `wc`, `filter`):

1. Implement the function with signature `int64_t cmd_name(int argc, char *argv[])` in a `Userland/c/commands/*.c` file.
2. Register it in `Userland/c/shell/syscall.c` -> `registry[]` (`name` -> `fn`).
3. Register the **name** in `Userland/c/shell/userlib.c` -> `is_child_command()` so the shell spawns it as a new process instead of running it as a built-in.
4. Update `help` text in `Userland/c/shell/help.c` if desired.

## Syscalls

The only kernel<->userland channel is `int 0x80`.  
To add a syscall:

1. Bump `CANT_SYS` in `Kernel/include/defs.h`.
2. Implement `sys_*` in the kernel.
3. Add it to the `syscalls[]` table in `Kernel/c/syscall/syscallDispatcher.c`.
4. Expose a wrapper in `Userland/asm/userlib.asm` and `Userland/c/include/syscall.h`.
5. **Critical:** Update the hardcoded `cmp rax, 44` in `Kernel/asm/interrupts.asm` (`_irq128Handler`) to match the new `CANT_SYS`. If this assembly check is out of sync, the new syscall will return `-1` (invalid) even if the table is correct.

## Testing

Run these **inside the running OS shell**. They must work as both foreground and background (`&`):

- `test_mm <max_bytes>` — infinite alloc/free loop; prints only on error. Must pass with at least one MM.
- `test_processes <max_procs>` — process creation/kill loop.
- `test_prio <target>` — priority scheduler test.
- `test_sync <n> <use_sem>` — race-condition test; result must be `0` when `use_sem=1`. (`<pairs>` and `<increments>` are hardcoded in the test.)
- `test_named_pipe` — named pipe IPC test (runs as a child process).
- `ps` — list processes.

## Critical constraints

- **Zero `-Wall` warnings.** Kernel and userland compile with `-Wall -Wextra -Werror -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -Wformat -Wstrict-prototypes -Wno-unused-parameter -ffreestanding -nostdlib -mno-red-zone -fno-common -fno-pie -fno-exceptions -fno-asynchronous-unwind-tables -mno-mmx -mno-sse -mno-sse2 -fno-builtin-malloc -fno-builtin-free -fno-builtin-realloc -std=c99`.
- **No busy waiting** except where explicitly allowed (`loop` command, `test_sync` without semaphores).
- **No deadlocks, no race conditions.** Semaphore value updates must use atomic instructions (`xchg` / `lock cmpxchg`).
- **No binaries in repo** — `.o`, `.bin`, `.img`, `.qcow2`, `.vmdk` are already in `.gitignore`.
- **Spanish** for code, comments, and commit messages.
- `make`, `make all`, and `make <memory_manager>` are reserved **exclusively** for compilation inside the Docker image. Other tasks (run QEMU, pull image, etc.) must use the provided scripts.

## Known deviations

- The enunciado requires **all** commands to be real processes. Some are still in-process built-ins:
  - `clear`, `ps`, `printTime`, `printDate`, `registers`, `testDiv0`, `invOp`, `playBeep`
- Shell background (`&`) and two-stage pipes (`cmd1 | cmd2`) **are** already implemented in `Userland/c/shell/userlib.c`.
- Keyboard shortcuts `Ctrl+C` (kill foreground) and `Ctrl+D` (EOF), and `+`/`-` (font size) **are** implemented.
  - `Ctrl+C` kills the **newest** foreground process (highest PID), avoiding killing the shell when it is waiting for a child.
  - `Ctrl+D` sends `0x04` (EOT) to the keyboard buffer; `fd_read` interprets it as EOF.

## Critical invariants (Ctrl+C / process kill / ZOMBIE reaping)

These bugs were fixed and their invariants **must not be regressed**:

- `process_kill` must **never** free `stack_base` or `argv` when killing the current process from interrupt context — only mark ZOMBIE + set `force_switch = 1`. (`Kernel/c/process/process.c`)
- `process_kill_foreground` must select the foreground process with the **highest PID** (newest child) and only kill if `fg_count > 1`, to avoid killing the shell itself. (`Kernel/c/process/process.c`)
- Both `scheduler_tick` and `scheduler_yield_impl` must reap ZOMBIEs **after** confirming a valid `next` — call `scheduler_remove(cur)`, free `stack_base` and `argv`, mark slot `FREE`. (`Kernel/c/scheduler/scheduler.c`)
- `process_waitpid` must free `stack_base` and `argv` before setting `child->state = PROCESS_FREE`. (`Kernel/c/process/process.c`)
- `process_exit` must call `scheduler_remove(current_process)` in all exit paths. (`Kernel/c/process/process.c`)
- `_irq01Handler` (keyboard ISR) must check `force_switch` before `popState` — same pattern as `_irq128Handler` (syscall ISR). (`Kernel/asm/interrupts.asm`)

## Critical invariants (MVar / scheduler priority)

- `mvar_cleanup_for_process` must wake blocked processes after removing a killed PID — if MVar is EMPTY and `wq_count > 0`, pop and unblock a writer; if FULL and `rq_count > 0`, pop and unblock a reader. Without this, killing a blocked process deadlocks the MVar. (`Kernel/c/syscall/mvar.c`)
- `wq_pop_highest` / `rq_pop_highest` use a **cooldown** to prevent starvation: after `priority` consecutive serves of the highest-priority level, the lowest-priority entry gets one turn. Do not replace with pure FIFO or pure priority — both cause bugs (FIFO ignores `nice`; pure priority starves lower-priority writers). (`Kernel/c/syscall/mvar.c`)
- `scheduler_next_ready` gives foreground processes an **effective +1 priority boost** for selection. Without this, a foreground shell at priority 3 is starved when any background process has priority ≥ 3. (`Kernel/c/scheduler/scheduler.c`)

## Repo-specific gotchas

- `compile.sh` validates that the container mounts the current directory at `/root`; if you moved the repo, recreate the container.
- `Kernel/c/syscall/syscallDispatcher.c` uses `CANT_SYS` for the `syscalls[]` array size, but the bounds check in `Kernel/asm/interrupts.asm` is a hardcoded literal (`cmp rax, 44`). They must be kept in sync.
