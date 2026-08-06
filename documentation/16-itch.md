# 16 — itch.io

## What I can and cannot do

Worth being exact, because the split is not obvious:

| | Who does it | Why |
|---|---|---|
| Create the account | **you** | It needs an email, a password and agreeing to terms. I do not create accounts or enter passwords. |
| Create the project page | **you** | itch.io has no API for making a page. `butler`, its command-line tool, uploads *build files* and nothing else. Title, description, screenshots, tags, pricing and visibility are all edited on the website. |
| `butler login` | **you**, once | It opens a browser and hands the token straight to butler, which stores it. Nothing else ever sees it — that is exactly why this step is yours and not mine. |
| Upload a build, now and every release after | **me** | `tools/push-itch.ps1`. Once butler holds the credential, pushing is one command with no secret in it. |
| Write the page copy | **me** | Below, ready to paste. |
| Update the page text later | **you**, pasting | Same reason as creating it — no API. |

So: unlimited uploads from me, but the page itself is yours to click through. If itch ever
adds page authoring to their API this changes; today it does not exist.

## Setting it up

1. Make the account at [itch.io](https://itch.io/register).
2. **Create a new project** — Dashboard → *Create new project*.
   - Kind of project: **Downloadable**.
   - Classification: **Games**.
   - Pricing: **No payments** (or *Donate*, if you want the option open).
   - Platform: tick **Windows**.
   - Note the URL you choose. If it is not `worldshapergame/worldshaper`, pass the right one
     to the push script with `-Target`.
3. Install butler: <https://itch.io/docs/butler/installing.html>
4. Run `butler login` once. A browser opens; approve it.

After that, every release is:

```powershell
tools\package.ps1
tools\push-itch.ps1
```

itch.io keeps its own version history per channel, so pushing a new build never removes the
old one, and butler only uploads the parts of the zip that changed.

## Does itch.io stop the "this might be a virus" warning?

Partly, and only for some players. The full reasoning is in `15-releases.md`; the short of it:

- A player who **downloads the zip from the itch.io page in a browser** gets exactly the same
  Windows SmartScreen warning as from GitHub. The browser is what attaches the Mark of the
  Web, and that is what triggers the warning.
- A player who installs through the **itch.io desktop app** does not, because the app fetches
  the files itself and does not attach that tag.

So it is free and it helps, but only for the app users. Steam removes it for everyone and
costs $100 once. A code-signing certificate removes it everywhere without a store.

## Page copy, ready to paste

### Short description (the one-liner under the title)

> A voxel creation game. Everything is a real voxel, thirty-two of them to the metre.

### Description

> **WorldShaper** is a voxel creation game where everything is a real voxel — no smoke and
> mirrors, no shapes faked with maths at the last moment. They are 32 to the metre, a little
> over 3 cm each, which is small enough to carve a doorframe rather than a doorway.
>
> This is an early build. What it has is the part everything else is built on: the renderer,
> the world, and the tools to shape it.
>
> **What you can do right now**
>
> - Carve and place at 3 cm resolution, with a tool that works both on a surface you are
>   touching and out in mid-air at whatever distance you set.
> - Copy any region and stamp it back — turned, resized, repeated. A row of copies can each
>   carry a share of the transform, so a spiral staircase or a tapering spire is one gesture
>   rather than forty.
> - Undo any of it, without limit.
>
> **What is different about it**
>
> - **No view distance setting, and no level of detail.** A mountain forty kilometres away
>   costs about what a rock at your feet costs, for the same number of pixels on screen. The
>   renderer works out how carefully to look, continuously, with no switch-over point where
>   things pop.
> - **Nothing is created or destroyed by accident.** Every change to the world is a recorded,
>   replayable instruction, and a running ledger of every voxel is checked against a full
>   recount. Turning a copy uses a method that provably cannot lose or invent a single voxel.
> - **It is free, and the source is public.** MIT-0: do anything you like with it.
>
> **Controls** are in the README inside the download, and on the GitHub page.
>
> **Requirements:** Windows 10 or later, and a graphics card with Vulkan 1.3 drivers.
>
> Source, roadmap and the full design notes:
> <https://github.com/worldshapergame/WorldShaper>

### Tags

`voxel`, `sandbox`, `building`, `creative`, `open-source`, `singleplayer`, `3d`

### Details to set

- **Release status:** In development
- **Platforms:** Windows
- **Licence:** MIT-0 (in the description; itch has no field for it)
