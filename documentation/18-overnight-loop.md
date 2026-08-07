# 18 - The overnight loop

## What it is

`loop.bat` in the repository root. Double-click it, say what to work on, and it runs Claude
Code against that goal over and over until you stop it — while you are asleep or away, on a
machine that stays on.

Each iteration builds, runs the whole test suite, takes screenshots and reads them, updates
the documentation, commits and pushes. Then the next one starts.

## Running it

```bash
loop.bat
```

It asks two things:

| Question | What it decides |
| --- | --- |
| What should it work on | The goal, passed to every iteration. "optimize the game", "fix the path tracer's speckle", "make the chisel nicer to use". |
| Let it use subagents and workflows | Whether iterations may fan out. Off by default: single-handed work is slower but every conclusion is traceable to something that was actually read. |

Options, if you want them:

```bash
loop.bat -MaxIterations 8 -TimeoutMinutes 90 -PauseSeconds 30 -Model opus
```

## Stopping it

**Run `loop.bat` again.** It notices the loop already running and offers to wrap up. Say yes
and the running loop finishes its current iteration, then does one final pass that commits
what is done, reverts what is not, updates the documentation and writes a closing journal
entry. Nothing is lost.

Closing the window or pressing Ctrl+C stops it too, but can leave work uncommitted. Creating
a file called `STOP-LOOP` in the root does the same as asking for a wrap-up, which is useful
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
  == success  turns 42  15.0 min  cost 3.14 USD
```

`~` is thinking, `->` is a tool being used (yellow for a subagent), `!!` is a tool that
failed, `==` is the iteration finishing with its turn count and cost.

## The journal is the memory

Every iteration is a **fresh session that remembers nothing** of the ones before it. What
crosses between them is one file:

```
%LOCALAPPDATA%\WorldShaper\loop\journal.md
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

`tools/loop-prompt.txt`, with the goal and iteration number substituted in. It is a plain
text file — edit it to change how the loop behaves. In short, it says:

- Do **one** focused thing. Carried all the way to committed and documented beats four
  half-finished.
- **Rewrite anything** that needs rewriting, including core engine pieces. A cautious patch on
  a design that is wrong costs more over a night than the rewrite would have.
- Usage is not metered, so do not economise on effort — but context is finite and the loop is
  continuous, so the journal is the deliverable that survives.
- Measure, do not guess. A change that cannot be shown to be an improvement gets reverted and
  written down rather than committed.
- Never weaken a test to make it pass.
- Every iteration ends with: it builds, all tests pass, the renderer still looks right in a
  screenshot that was actually *read*, documentation matches the code, it is pushed, and the
  journal is appended to.

`tools/loop-wrapup-prompt.txt` is the last iteration's version: finish what is in flight,
commit or revert it deliberately, confirm the build and tests, push, and write the closing
entry.

## If it refuses to start

Before the loop begins, it asks Claude one trivial question and waits for an answer. Two
seconds, a fraction of a penny, and it turns "nothing happened all night" into "it would not
start, and here is what it said."

**"The Claude CLI is not logged in."** The command-line `claude` keeps its own credentials,
separate from any Claude app installed beside it. Being signed in there says nothing about
being signed in here. Sign it in once:

```bash
claude
```

then `/login` inside it, close that window, and run `loop.bat` again.

**Anything else** is printed as Claude's own words rather than a guess at the cause. If the
model name is the problem, `loop.bat -Model opus` uses whatever the CLI's alias resolves to.

## Worth knowing before you leave it running

- It runs with permission checks bypassed. That is what "unattended" means: it can edit any
  file in the repository, run any command, and push to GitHub without asking.
- It pushes to the **current branch**. Put it on a branch of its own if you would rather
  review the night's work before it reaches `main`.
- It never force-pushes and never rewrites published history.
- An iteration that runs past `-TimeoutMinutes` (default 60) is killed and noted in the
  journal, so one wedged iteration cannot consume the whole night.
- After every iteration the loop runs the test suite **itself**, independently of whatever the
  agent claimed, and writes a warning into the journal if it is failing.
