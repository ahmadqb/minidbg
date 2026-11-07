# minidbg

A minimal command-line debugger for x86-64 Linux, written in C++17 on top of `ptrace`.
Built as a learning project to understand how debuggers like GDB work under the hood:
process control, breakpoints, single-stepping, registers and ELF symbols.

## Features

- Launches a program under `ptrace` and takes control right after `exec`
- Breakpoints by function name (`break add`) or raw address (`break *0x...`)
- `continue`, `step` (one instruction), `regs`, and memory examine (`x <addr> [n]`)
- Resolves function names from the ELF `.symtab`, including the PIE load base via `/proc/<pid>/maps`
- Disables ASLR for the target (`personality(ADDR_NO_RANDOMIZE)`) so addresses are stable between runs

## Build

Requires Linux x86-64 and a C++17 compiler.

```
g++ -std=c++17 -Wall -g minidbg.cpp -o minidbg
gcc -g -O0 target.c -o target        # sample program to debug
```

## Usage

```
./minidbg ./target
```

| Command | Description |
|---|---|
| `break <func>` / `b` | Breakpoint at the first instruction of a function |
| `break *<addr>` | Breakpoint at a raw hex address, e.g. `break *0x555555555149` |
| `continue` / `c` | Resume until the next breakpoint or program exit |
| `step` / `s` | Execute exactly one instruction |
| `regs` | Print the general-purpose registers |
| `x <addr> [n]` | Print `n` 8-byte words starting at `addr` (default 1) |
| `quit` / `q` | Kill the target and exit |

## Example: catching a function return

Output abridged. Addresses will differ on your machine.

```
$ ./minidbg ./target
Started ./target (pid 32286), 9 function symbols
(minidbg) break add
Breakpoint at 0x555555555149
(minidbg) continue
Breakpoint hit at 0x555555555149
pc = 0x555555555149  <add+0x0>
(minidbg) x 0x7fffffffdbc8            # rsp: return address pushed by `call`
0x00007fffffffdbc8: 0x000055555555518c
(minidbg) break *0x55555555518c       # stop right after add() returns
(minidbg) continue
Breakpoint hit at 0x55555555518c
pc = 0x55555555518c  <main+0x2b>
(minidbg) regs                        # rax = 1, the result of add(0, 1)
```

## How it works

- **Launch:** `fork()`; the child calls `PTRACE_TRACEME`, disables ASLR and `exec`s the target. The parent waits for the initial stop.
- **Breakpoints:** save the original byte at the address and overwrite it with `0xCC` (INT3). When it executes, the kernel sends SIGTRAP to the target. `rip` then points one byte past the INT3, so the debugger rewinds it by 1.
- **Resuming from a breakpoint:** restore the original byte, single-step one instruction, re-insert `0xCC`, then `PTRACE_CONT`.
- **Single-step:** `PTRACE_SINGLESTEP` (the CPU's trap flag).
- **Symbols:** read `STT_FUNC` entries from `.symtab`; for PIE binaries, add the load base found in `/proc/<pid>/maps`.
- **Registers and memory:** `PTRACE_GETREGS` / `PTRACE_SETREGS` and `PTRACE_PEEKDATA` / `PTRACE_POKEDATA`.

## Limitations

- x86-64 Linux only; single-threaded targets; does not follow `fork`/`exec`
- Needs a non-stripped binary (a `.symtab` section)
- `break <func>` stops at the function's first instruction, before the prologue (no DWARF line info yet)
- Memory reads show `0xCC` where a breakpoint is active
- Ctrl-C kills both debugger and target; use `quit` instead
- No disassembly yet

## Roadmap

- [ ] Disassemble at `rip` (Capstone)
- [ ] Hardware watchpoints via the x86 debug registers (DR0-DR3)
- [ ] Backtrace by walking the `rbp` chain
- [ ] Python test harness driving the debugger over stdin/stdout
- [ ] Skip the prologue using DWARF line information
- [ ] Proper Ctrl-C handling (interrupt the target, return to the prompt)
