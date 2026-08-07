# 18 - The overnight loop

## What it is

`loop.bat` in the repository root. One file, no installation, nothing beside it. Double-click
it, say what to work on, and it runs Claude Code against that goal over and over until you
stop it — while you are asleep or away, on a machine that stays on.

Each iteration works out how the project builds and tests itself, does one focused piece of
work, verifies it, updates the documentation, commits and pushes. Then the next one starts.

## It is not specific to this project

Copy `loop.bat` into the root of any git repository and it works there. It knows nothing
about WorldShaper: it never mentions `build.bat` or `ws_tests.exe`, and each iteration is told
to read the project's README, build files and CI configuration to find out how *that* project
is built, tested and run.

State is kept per folder, under `%LOCALAPPDATA%\claude-loop\<folder name>\`, so two projects
can run their own loops at once without sharing a journal.

It is a batch file and a PowerShell script in one file. The batch half reads the file back off
disk, discards everything up to a marker, and runs the rest as PowerShell — which is why there
is only ever one thing to copy.

## Running it

```bash
loop.bat
```

It asks two things:

| Question | What it decides |
| --- | --- |
| What should it work on | The goal, passed to every iteration. |
| Let it use subagents and workflows | Whether iterations may fan out. Off by default: single-handed is slower, but every conclusion stays traceable to something actually read. |

Options, if you want them:

```bash
loop.bat -MaxIterations 1 -TimeoutMinutes 90 -PauseSeconds 30 -Model opus
```

`-MaxIterations 1` is the right way to watch one iteration through before trusting it with a
whole night.

## Stopping it

**Run `loop.bat` again.** It notices the loop already running and offers to wrap up. Say yes
and the running loop finishes its current iteration, then does one final pass that commits
what is done, reverts what is not, updates the documentation and writes a closing journal
entry. Nothing is lost.

Closing the window or pressing Ctrl+C stops it too, but can leave work uncommitted. A file
called `STOP-LOOP` beside `loop.bat` does the same as asking for a wrap-up, which is useful
over a network share or a remote desktop.

## What you see while it runs

The window streams the work as it happens, not a summary afterwards:

```
  [session 4f2a…  model claude-opus-5]
  ~ the face cache is probably refusing slots here
  -> Read  shaders/pathtrace.comp
  -> PowerShell  .\build.bat
  -> Agent  Explore: map the tracer
  refusals down from 12% to 0.7%
  !! error C2039: not a member
  == done  turns 42  15.0 min  cost 3.14 USD
```

`~` is thinking, `->` a tool being used (yellow for a subagent), `!!` a tool that failed, `==`
the iteration finishing. A failed iteration says `FAILED` and prints what Claude said.

## The journal is the memory

Every iteration is a **fresh session that remembers nothing** of the ones before it. One file
crosses between them:

```
%LOCALAPPDATA%\claude-loop\<folder>\journal.md
```

Each iteration reads it first and appends to it last: what it did, what it measured, what it
committed, what to do next, and what not to retry. Read it in the morning; it is the report.

It lives outside the repository deliberately, so an agent committing its own work does not
sweep the journal and the logs into the history.

| File | What it is |
| --- | --- |
| `journal.md` | The loop's memory, and your morning report |
| `logs\iteration-NNNN.log` | Raw stream-json for one iteration |
| `prompt.txt` | Exactly what the last iteration was told |
| `running.lock` | The pid of the live loop, so a second launch can find it |

## What each iteration is told

The prompts are inside `loop.bat` — edit them there. In short:

- Do **one** focused thing, carried all the way to committed and documented.
- **Rewrite anything** that needs rewriting, including core pieces. A cautious patch on a
  design that is wrong costs more over a night than the rewrite would have.
- Usage is not metered, so do not economise on effort — but context is finite and the loop is
  continuous, so the journal is the deliverable that survives.
- Measure, do not guess. A change that cannot be shown to be an improvement gets reverted and
  written down rather than committed.
- Never weaken a test to make it pass.
- Every iteration ends with: it builds, all tests pass, it still runs and that was checked,
  documentation matches, it is pushed, and the journal is appended to.

## If it refuses to start

Before the loop begins it asks Claude one trivial question and waits for an answer. Two
seconds and a fraction of a penny, and it turns "nothing happened all night" into "it would
not start, and here is what it said."

**"The Claude CLI is not logged in."** The command-line `claude` keeps its own credentials,
separate from any Claude app installed beside it. Sign it in once:

```bash
claude
```

then `/login` inside it, close that window, and run `loop.bat` again.

**Anything else** is printed as Claude's own words rather than a guess at the cause.

## Worth knowing before you leave it running

- It runs with permission checks bypassed. That is what unattended means: it can edit any file
  in the repository, run any command, and push without asking.
- It pushes to the **current branch**. Put it on a branch of its own if you would rather review
  the night's work before it reaches `main`.
- It never force-pushes and never rewrites published history.
- An iteration that runs past `-TimeoutMinutes` (default 60) is killed and noted in the
  journal, so one wedged iteration cannot consume the whole night.
- After every iteration the loop checks two things itself, independently of anything the agent
  claimed: whether a commit was made, and whether the working tree was left dirty. Both hold in
  any repository, whatever it is written in. A dirty tree is written into the journal as a
  warning for the next iteration.
- Three iterations that end in seconds stop the loop rather than let it spin all night.
