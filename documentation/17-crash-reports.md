# 17 - Crash reports

## Why this exists

A build launched by double-clicking has nowhere to print to. Before this, every way of
dying looked identical from the outside: the window appeared, and then it was gone. An
access violation, a failed `WS_CHECK`, a lost GPU device and running out of memory were
indistinguishable, so "it crashed" was the whole of every bug report anyone could give.

Now each of those writes a file naming itself.

## What a player has to do

Nothing. On a crash a dialog names the report and the folder it is in. Both files in that
folder are worth sending.

## Where the files are

`%LOCALAPPDATA%\WorldShaper\` — not next to the executable, because an installed copy may
sit somewhere the player has no permission to write.

| File | What it is |
| --- | --- |
| `crash-<date>-<time>.<ms>-<pid>.log` | The readable report |
| `crash-<date>-<time>.<ms>-<pid>.dmp` | A minidump, for opening in a debugger |
| `worldshaper.log` | The current session's log |
| `worldshaper.log.prev` | The previous session's, kept because a player who crashes and relaunches would otherwise have already overwritten the interesting one |

Milliseconds and the process id are in the name because two crashes inside one second must
not overwrite each other, and the second one is often the more interesting.

## What a report contains

- **Fault** — what went wrong, in words. An access violation also says which way and where:
  `write to 0x0` is a null pointer without any further work; a huge address is a bad index
  rather than a missing object.
- **Context** — the GPU and driver, and where the camera was standing at the time: frame
  number, chunk, and position inside it. A fault that only happens in one place is
  answerable; a fault "somewhere" is not.
- **Call stack** — module plus offset for every frame, and function names with file and line
  where symbols are available. A player's machine has no `.pdb`, so only module+offset
  survives there; matched against the build's own symbols afterwards, it still pins the
  crash down.
- **Last log lines** — several hundred, oldest first. A failed `WS_CHECK` prints its message
  before it aborts, so the report ends up explaining itself.

## What is covered

| Kind of death | Route |
| --- | --- |
| Access violation, illegal instruction, divide by zero, stack overflow | Unhandled exception filter |
| Failed `WS_CHECK`, failed `WS_VK` (a lost device lands here) | `abort` → `SIGABRT` |
| Uncaught C++ exception | `std::terminate` |
| Pure virtual call | `_set_purecall_handler` |
| Bad CRT argument | `_set_invalid_parameter_handler`, whose default is to kill the process without a word |

`SIGSEGV`, `SIGILL` and `SIGFPE` are deliberately **not** hooked. The CRT will translate an
access violation into `SIGSEGV` if a handler exists, and that path throws away the exception
record: no fault description, and a stack that starts inside the signal machinery instead of
at the fault. Left alone they reach the filter with everything intact.

## Proving it works

`--crash-test KIND` breaks on purpose, each kind through a different route, so a report that
never arrives points at which mechanism is missing rather than at the whole system.

```bash
WorldShaper.exe --crash-test frame --screenshot-frame 90
```

`read`, `write`, `check`, `throw`, `divzero`, `frame` (faults inside the running game, which
is what proves the camera and device context is carried), and `report` (writes a report and
carries on, for a hang or a wedged device).

## Notes for later

- The dialog is suppressed for `--headless` and scripted screenshots. A modal dialog in an
  automated run is a hang.
- Reports are never uploaded anywhere. Sending one is the player's choice, by hand.
- The handler allocates nothing: the report is assembled in a fixed buffer, and the log tail
  it reads is lock-free, because the thread that just faulted may be the one holding the log
  mutex.
