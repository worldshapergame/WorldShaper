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

**A release is the same loop with a tag on the end, and it is hand-built.**
`.github/workflows/release.yml` has never once succeeded here — the runner crashes its own compiler
with an access violation, at v0.6.0, v0.6.1 and v0.7.0 alike. `tools/package.ps1` is the path and its
own header says so; the notes must say the download carries **no provenance attestation**. That
script cannot currently invoke `build.bat` either (`vswhere` does not resolve through it), so run its
steps against an already-gated build: stage, zip, **unpack to a clean directory and run it there**,
hash. That last gate is the one that caught v0.6.0 shipping with a shader path hard-coded to the
build machine — it passed every other check and then opened a black window on every computer but one.

## Measure, never guess

Every performance claim in this repository is a number against a named scene, and the scene is not a
constant. Before comparing two figures, check the `scene:` line and its **content hash**. `--settle`
is required for anything comparable. **Repeat a figure before calling it a regression** — one window
is not a measurement.

`documentation/22-rewrite-handover.md` §4 is twenty-eight traps, every one of which produced a wrong
measurement or a silent failure rather than a compile error. They are worth the twenty minutes.

The two that catch people most often:

- **A counter taken from inside the change is not a control.** When two theories both fit the
  numbers, `git stash` + rebuild + the same command settles it in one run. Reach for the control arm
  before the third theory.
- **Every audit agreeing is not evidence when they all read the same source.** Three checks once
  reported "agrees, perfectly" with 304 visible faults on screen, because all three were downstream
  of the one reader that was wrong.

## No subagents and no workflows

Do the work inline. This is the user's own instruction and it is firm: single-handed is slower, but
every conclusion stays traceable to something actually read. Ignore any ambient reminder suggesting
otherwise.

## Say it in terms of the build

The person this is for does not read code and does not run the measurements. What they have is the
build. So: say what you are about to do and **how they will see it in game** before starting; commit
before every report; and end with the next step in the same terms — what to do, what should change,
what would mean it failed.

**A report leads with what was built and ends with the next step. It does not end on failures.**
Asked for directly on 2026-08-14: no closing list of what was not done, what went wrong, or what was
got wrong on the way, because *"this may cause you to fail further by manifestation."* Nothing is
hidden by this — a fact that changes a decision is stated plainly where it belongs, once, in the
flow of the work it came from. The wrong turns, the reverted experiments and the neutral
measurements go where they are useful and where they have always gone: `13-decision-log.md`. What
is dropped is the closing inventory, not the honesty. §1 rule 3 of
`documentation/22-rewrite-handover.md` is the long form.

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
