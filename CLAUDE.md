# WorldShaper — the rules that are not negotiable

A voxel creation game in C++20 and Vulkan 1.3, no engine, 32 voxels to the metre. **Read
`documentation/22-rewrite-handover.md` before doing anything** — §0 is what the project is, §1 is how
to work with the person it is for, §4 is the traps, and §5 is where work starts.

This file exists for the handful of things that are true on every task, so that nobody has to reach
§1 to find them out.

## After every change: build, test, commit, merge, push

Not at the end of a session. Not when a stage finishes. **With the change.**

```bash
build.bat  then  build\bin\ws_tests.exe
git commit -a
git checkout main && git merge --ff-only <branch> && git push origin main
```

A report describes a build. If the build is not in the history the user can reach, the report is
about something nobody can go back to — and a commit sitting on a local branch is not in that
history. `main` was **58 commits behind `origin/main`** when somebody finally looked.

Three things that make this a rule rather than a habit:

- **Build before you commit.** A working tree that has not been compiled since the last edit is a
  draft, not a change. One was nearly merged that way.
- **Kill any stale `WorldShaper.exe` first — and before every MEASUREMENT, not only every build.**
  The linker fails with `LNK1168: cannot open bin\WorldShaper.exe for writing`, which reads like a
  code fault and is not one. The worse half is quieter: a stray left over from a `Start-Process`
  goes on sampling on half the machine, and every figure taken beside it is wrong **gradually** — one
  took five readings of the same command from 18.8 s to 36.2 and looked exactly like a trend rather
  than like contamination (D722). `Get-Process WorldShaper` before a run costs nothing.
- **`--ff-only`.** A branch that has diverged from `main` is something to find out about
  deliberately, not by watching git invent a merge commit.
- **Run `build.bat` from PowerShell, and give it the FULL path** -- `cmd /c "<repo>\build.bat"`, not a relative name and not a `cd` in front of it. Invoking it from
  the Bash tool as `cmd.exe /c "cd /d <path> && build.bat" | tail` **hangs immediately and for
  ever** — no compiler ever starts, the output is empty, and there is nothing to distinguish it from
  a slow build. One sat for an hour. Two things make it worse than a wasted hour: a hung build is
  invisible unless somebody looks at the process list, and **killing the process does not end the
  background task** — stop the task itself, or it stays registered as running long after the thing
  it was running is dead.

**A release is the same loop with a tag on the end.**
`.github/workflows/release.yml` **works, and v0.7.1 and v0.8.0 were built by it** — v0.7.1 was the
first green run this repository has had, v0.8.0 the second, in six minutes eleven (D657). The reason it never worked was written down wrong: it was read as the runner
crashing its own C++ compiler, and the v0.7.0 log says every `.obj` compiled and **`glslc` died on
`clouds.comp`** — the SHADER compiler, out of a Vulkan SDK the workflow asked for as `latest`, so the
build tool changed under the build between runs. It is pinned to 1.4.341.0 now (D641).

**So a release is: bump `project(WorldShaper VERSION ...)`, commit, and dispatch the workflow with
the matching tag.** Dispatch rather than pushing the tag, so a failure leaves no tag pointing at a
build that does not exist; the workflow creates the tag when it publishes. It gates itself on what
has actually gone wrong here — the tag must match the source, the tests must pass, and it **opens its
own zip elsewhere and makes the shipped facility build** before publishing. `tools/package.ps1` is
still the path for a zip you do not want published, and its notes must say that download carries
**no provenance attestation**; a CI release carries one. That
script cannot currently invoke `build.bat` either (`vswhere` does not resolve through it), so run its
steps against an already-gated build: stage, zip, **unpack to a clean directory and run it there**,
hash. That last gate is the one that caught v0.6.0 shipping with a shader path hard-coded to the
build machine — it passed every other check and then opened a black window on every computer but one.

## Measure, never guess

Every performance claim in this repository is a number against a named scene, and the scene is not a
constant. Before comparing two figures, check the `scene:` line and its **content hash**. `--settle`
is required for anything comparable. **Repeat a figure before calling it a regression** — one window
is not a measurement.

`documentation/22-rewrite-handover.md` §4 is thirty traps, every one of which produced a wrong
measurement or a silent failure rather than a compile error. They are worth the twenty minutes.

**The gate command, written out, because it has been quoted wrong from memory and it fails
SILENTLY.** `--clip` is a scripted clip EDIT that parses twelve numbers; the world is `--clip-file`,
and a run given the wrong one loads the estate instead and says nothing. `--refine-all` and
`--no-despeckle` are both load-bearing — without them the ladder is camera-driven and settles on a
different, legitimate world.

```
build\bin\WorldShaper.exe --clip-file clips/sampler.clip --refine-all --no-despeckle --no-clip-cache --no-title --settle --screenshot <out.png> --screenshot-frame 240 --max-seconds 300
```

```
scene: 4 chunks, 1430104 solid voxels, 9826 of 9826 nodes sharpened, content a1f8bc6c656343b7, shape e105a8a6940f0da2
```

`d0d5f84c685be847` is the SAME world with its type table interned in another order (D729) — compare
**shape** whenever a resume is involved, and content only cold against cold. D733.

The two that catch people most often:

- **A counter taken from inside the change is not a control.** When two theories both fit the
  numbers, `git stash` + rebuild + the same command settles it in one run. Reach for the control arm
  before the third theory.
- **Every audit agreeing is not evidence when they all read the same source.** Three checks once
  reported "agrees, perfectly" with 304 visible faults on screen, because all three were downstream
  of the one reader that was wrong.

## Subagents: allowed now, and only for work that ends in code

**This rule was reversed by the user on 2026-08-16**: *"you can use a ton of agents but make sure
they all actually produce code."* It used to read *no subagents and no workflows, do the work
inline*, and the reason it read that way is still true and is now the condition rather than the ban —
a conclusion nobody can trace to something actually read is worth nothing here, and an agent that
comes back with a survey, a plan or a recommendation has produced exactly that.

So: an agent is given a change to make, not a question to answer. It builds it, it runs
`ws_tests`, it takes the two arms of its own control flag, and it hands back a diff and the numbers.
Anything it could not make work comes back said plainly, which is worth more than a clean claim.
Whoever dispatched them still reads every diff, integrates them one at a time, and builds and tests
after each — the merge is not delegated, and neither is the report.

### And "produce code" means WRITE A FEATURE. It does not mean verify, fix or measure.

**Sharpened by the user on 2026-08-18, and it is a narrowing of the rule above, not a restatement
of it.** An agent is dispatched to build a thing that was not there. It is **not** dispatched to:

- **verify, review, refute or audit** somebody else's change;
- **fix a bug** — find one, reproduce one, or work out what broke;
- **measure** anything, take a baseline, or build an instrument.

Those are the dispatcher's own work, and they are the work that requires having read the thing. Do
them inline. **Unless the user asks for one of them by name, in which case do exactly the one they
asked for.**

The reason is D674 and it cost a full evening. Three agents were sent to fix a rule that was not
broken and a fourth to refute what they built; the arithmetic that settled it — `kRefineSplitAt` is
eight pixels at 0.002 of their distance, a node is eight voxels a side, so metre 32 is correct out
to 31.25 m — is four lines and needed no agent at all. Every one of the three had to build its own
instrument before it could start, which is three measurements of the same thing and three chances
to measure it wrong. **An agent sent to check something reads the same source the dispatcher would
have read, and comes back with a conclusion nobody can trace** — which is the original ban's own
reason, wearing a diff as a disguise.

### Dispatching an agent is not a reason to stop working

**Asked for directly on 2026-08-18.** An agent runs for tens of minutes. Handing one a task and then
posting a status line is a session that has bought nothing with that time — the agent would have
finished at the same moment either way, and the wait was spent by nobody.

So: **dispatch, then immediately pick up other work.** When that piece is finished, check whether the
agents are done; if they are not, pick up the next piece. Idle only when there is genuinely nothing
left that does not collide.

The one constraint is the one that already governs parallel agents — **files nobody else is in.**
Whatever is picked up must not touch what the agents are touching, because the integrator resolving
a conflict against its own uncommitted work is the lost afternoon this rule exists to prevent. In
practice that means: agents in `src/` → work in `clips/` or `documentation/`; agents in one
subsystem → work in another; and always commit what is finished before the diffs come back, so the
integration starts from a clean tree.

**A report is not work.** Writing to the user about what an agent is doing does not count as the
work; it is what happens around it.

Still true, and it is what makes parallel work safe here: give each agent an isolated worktree and
files nobody else is in. Two agents in one shader is a lost afternoon.

**And a worktree is not an isolated MACHINE, which is the half nobody thinks of.** Every agent is
told to kill a stale `WorldShaper.exe` before it builds — correctly, it is the rule above — and
`Get-Process WorldShaper | Stop-Process -Force` on a shared machine kills **every other agent's run
as well as its own**. Two of four agents in one wave reported it: one lost roughly a third of its
runs outright, the other had its timings taken beside two strays it did not own. The measurements
that survive are the ones taken when `Get-Process WorldShaper` read zero immediately before them,
and a run killed at second forty of sixty looks exactly like a crash — truncated output, exit 255,
no dump.

So: **parallel agents may build in parallel and must not MEASURE in parallel.** Either give one
agent at a time the machine for its numbers, or accept that every timing from a wave is noise and
re-take the ones that matter afterwards. Correctness gates — a test suite, a content hash — are
unaffected and can run whenever.

## Say it in terms of the build

The person this is for does not read code and does not run the measurements. What they have is the
build. So: commit before every report, and then **the report to them is a very short list of what
they should see in game. Nothing else.**

**Asked for directly on 2026-08-15, and it is firm: do NOT write a "what I did not do" section, do
NOT write "what would mean it failed", and do not pad the report with method, measurements,
refutations or caveats.** The list is what they should SEE, and nothing else — no failure modes to
watch for, no things to check, no next steps. Those belong in
`13-decision-log.md` and in `22-rewrite-handover.md`, where the next session reads them — not in the
reply. A caveat only reaches the report when it changes what they should look at in the build. The
same goes for the next step: put it in the handover, and mention it only if it needs a decision from
them.

## Where things are written down

- `documentation/22-rewrite-handover.md` — start here. §5 is the current work.
- `documentation/13-decision-log.md` — every decision with its reason, **including the ones that
  turned out wrong**. Those are the most useful entries in the repository.
- `documentation/21-renderer-rewrite.md` — the plan. §8.0 is the ledger of what is done.
- `documentation/09-performance-budgets.md` — what the numbers are judged against. Exceeding a budget
  is a bug, not a trade-off.

When reality disagrees with a document, the document is corrected in the same change.
