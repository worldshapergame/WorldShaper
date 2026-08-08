# parapet — what I could not do, and what the next person needs to know

## 1. The portico flanks have no balustrade, and that is a decision, not an oversight

`part_parapet` runs the north front, both flanks and the two returns of the south front. It stops
at a pedestal centred on `x = ±8.10` (its base reaching to `x = ±7.74`, which is 0.09 clear of
`portico_x`). Between those two pedestals — the whole width of the portico — there is nothing,
because that is where the pediment is.

But `entablature.clip` also carries the cornice out along the **portico flanks**, `x = ±7.65` from
`z = -7.45` to `z = -11.60`, and there is 4.15 m of cornice top there at 11.90 with nothing on it.
Classically that is balustraded too. I did not build it, for one reason: the far end of that run is
the portico's south angle, which is exactly where a pediment puts its corner acroterion (see
`acroterion` in `_order.clip`, which says in as many words that it is "for the pediment corners").
A corner pedestal with a 1.35 m urn on it and a palmette acroterion cannot both stand there, and
`pediment.clip` was still a placeholder while I was working, so there was no way to agree it.

**If the pediment or the roof wants that run**, it is eight lines and it should belong to whoever
owns the angle. If it is to come back to me instead, say so and I will add it; the numbers are
already in my file and the only new decision is how the south end is stopped.

## 2. `tools/views.ps1` dies on a stderr line, intermittently

`$log = & $exe @shot 2>&1 | Out-String` at line 249, under `$ErrorActionPreference = "Stop"`.
Windows PowerShell 5.1 wraps every stderr line from a native executable in a `NativeCommandError`,
so **any** `[WARN ]` the engine prints kills the script mid-run, before it writes the contact sheet
— even though the renderer exited 0 and the frames were fine. It cost me three runs:

```
WorldShaper.exe : [WARN ] frame    frame 0 took 350 ms
    + FullyQualifiedErrorId : NativeCommandError
```

The warning is the *first* frame of a cold world, so re-running the identical command usually
succeeds because the world is then cached. That is a bad thing to have to know. The one-line fix is
to capture the streams separately, or `$ErrorActionPreference = 'Continue'` around that one call —
`Test-Path $png` immediately below is already the real success check, so nothing is lost by not
throwing. I do not own the file, so I have not touched it.

## 3. `-Focus` is scaled by `Metre / 32`, and `-Part` plus `-Focus` together work

Not a fault, but it is not written down anywhere and it looks like one: the focus box you give in
clip metres is multiplied by `Metre/32` before the camera is placed, because a clip built at a
lower detail is a smaller model. So the same `-Focus` string frames different things at `-Metre 12`
and `-Metre 32`. Combining `-Part` with `-Focus` does what you want — it builds only that part and
frames the box — and is the only way to get close enough to read a 0.18 m feature, since framing on
the part alone puts the camera 30 m away from a 32 m long balustrade.

## 4. Nothing about the language was missing

Everything this part needed existed: `revolve` (through `ionic_baluster` and `urn`), the mouldings
with `run=`, `repeat`, `mirror`, `offset`+`round`. No workarounds.

One note on the moulding interface, because it took two contact sheets to get right and the
comment in `clip_script.cpp` is easy to read the other way round. For `cyma` and `cyma_reversa`
the two corners are **not** the ends of the curve, and they are not the same diagonal for both:

- `cyma`: the face stands at the **second** corner's projection at the **first** corner's height,
  and recedes to the first corner's projection at the second corner's height. Convex at the first
  corner's end.
- `cyma_reversa`: the face stands at the first corner's projection at the first corner's height
  and swells to the second's at the second's. Convex at the **second** corner's end.

So `cyma p0 q0 p1 q1` and `cyma_reversa p0 q0 p1 q1` put the swell at opposite ends *and* draw the
profile through opposite diagonals. Written the wrong way round, a moulding comes out as a flat
plane with a curve hidden behind it, which survives a measurement and is obvious on a contact
sheet. A worked example either way in `build_moulding`'s comment would save the next person the
same two renders.
