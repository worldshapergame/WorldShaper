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
- **Kill any stale `WorldShaper.exe` first.** The linker fails with `LNK1168: cannot open
  bin\WorldShaper.exe for writing`, which reads like a code fault and is not one.
- **`--ff-only`.** A branch that has diverged from `main` is something to find out about
  deliberately, not by watching git invent a merge commit.

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

Still true, and it is what makes parallel work safe here: give each agent an isolated worktree and
files nobody else is in. Two agents in one shader is a lost afternoon.

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
- `documentation/12-plain-english.md` — the same story written for the person the work is for. It is
  the only document with that audience, so keep it current.
- `documentation/21-renderer-rewrite.md` — the plan. §8.0 is the ledger of what is done.
- `documentation/09-performance-budgets.md` — what the numbers are judged against. Exceeding a budget
  is a bug, not a trade-off.

When reality disagrees with a document, the document is corrected in the same change.
