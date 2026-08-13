@echo off
rem ===========================================================================
rem  Overnight loop - one file, no installation, no dependencies.
rem
rem  Drop this into the root of any git repository and double-click it. It asks
rem  what to work on, then runs Claude Code against that goal over and over
rem  until you stop it: building, testing, committing and pushing each time.
rem
rem  Stopping it: run this file again and say yes to wrapping up. The running
rem  loop finishes what it is doing, commits it, documents it and stops. A file
rem  named STOP-LOOP beside this one does the same, which works over a network
rem  share or a remote desktop.
rem
rem  It is a batch file and a PowerShell script in one. The batch half below
rem  reads the file back off disk, throws away everything up to the marker, and
rem  runs the rest as PowerShell -- so there is only ever one file to copy.
rem ===========================================================================
setlocal
set "LOOP_SELF=%~f0"
set "LOOP_ARGS=%*"
title Overnight loop - %CD%
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText($env:LOOP_SELF); $i=$t.LastIndexOf([char]35+'::PSBEGIN::'); if($i -lt 0){Write-Host 'This file is damaged: the script half is missing.' -ForegroundColor Red; exit 1}; Invoke-Expression $t.Substring($i)"
endlocal
exit /b

#::PSBEGIN::

# ---------------------------------------------------------------------------
# Everything below here is PowerShell. The batch half above never reads it.
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$root = (Get-Location).Path
$self = $env:LOOP_SELF

# ---- arguments ------------------------------------------------------------
# Parsed by hand because this is run through -Command rather than -File, so
# PowerShell's own parameter binding is not available.
$opt = @{
    Goal           = ""
    Model          = "claude-opus-5"
    MaxIterations  = 0        # 0 means until stopped
    TimeoutMinutes = 60       # one wedged iteration must not eat the whole night
    PauseSeconds   = 10
    Agents         = ""       # y / n, asked if not given
    # What to carry on with when the primary model's usage is spent.
    #
    # "none" by default, deliberately: quietly finishing the night on a smaller
    # model is a change to the work, not to the schedule, and the difference does
    # not show up until somebody reads what was built. The loop waits for the
    # reset instead, which keeps every iteration the model that was asked for.
    #
    # Set it to carry on rather than wait. The CLI re-tries the primary at the
    # start of each session, so it climbs back on its own once the window resets.
    FallbackModel  = "none"
    # Where the session compacts its own context instead of letting it grow.
    #
    # This is the largest lever there is, because an iteration's price is its
    # context multiplied by its turns and only this bounds the first term.
    # Iteration 1 peaked at 209,858 tokens and resent 26.4 million; held at 100k
    # the same 200 turns resend roughly half that. Compaction summarises rather
    # than truncates, so nothing is lost outright - it is the cheapest of the
    # three levers and the only one that costs no work.
    Autocompact    = 100000
    # A hard ceiling on one iteration's usage, in USD-equivalent at API rates.
    #
    # 0 is off, and off is the default DELIBERATELY: the cap cuts an iteration
    # wherever it happens to be, so it can stop one mid-edit and waste what it
    # had done. It is the only thing that GUARANTEES a window buys several
    # iterations rather than one, so it is here - but as something chosen rather
    # than something arrived at. Iteration 1 was 11.38, for scale.
    MaxBudget      = 0
    # The account this loop is meant to run as. Empty means "whichever one Claude
    # Code is signed in as when it starts", which is then held for the whole run.
    Account        = ""
    # How long to keep waiting when the subscription's usage is spent, in hours.
    # 0 means wait indefinitely, which is the point of an overnight loop.
    MaxWaitHours   = 0
}
if ($env:LOOP_ARGS) {
    $tokens = [regex]::Matches($env:LOOP_ARGS, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') }
    for ($i = 0; $i -lt $tokens.Count; $i++) {
        $name = $tokens[$i].TrimStart('-', '/')
        $match = $opt.Keys | Where-Object { $_ -eq $name }
        if ($match -and $i + 1 -lt $tokens.Count) {
            $value = $tokens[$i + 1]
            if ($opt[$match] -is [int]) { $opt[$match] = [int]$value } else { $opt[$match] = $value }
            $i++
        }
    }
}

# ---- where state lives ----------------------------------------------------
# Outside the repository, keyed by folder name, so an agent committing its own
# work never sweeps the journal and the logs into the history -- and so two
# projects running this at once do not share a journal.
$slug = (Split-Path $root -Leaf) -replace '[^A-Za-z0-9._-]', '_'
$stateDir = Join-Path $env:LOCALAPPDATA "claude-loop\$slug"
if (-not (Test-Path $stateDir)) { New-Item -ItemType Directory -Path $stateDir -Force | Out-Null }
$journal  = Join-Path $stateDir "journal.md"
$logDir   = Join-Path $stateDir "logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$lockFile = Join-Path $stateDir "running.lock"
$wrapFile = Join-Path $stateDir "wrap-up.request"
$goalFile = Join-Path $stateDir "goal.txt"
$stopFile = Join-Path $root "STOP-LOOP"

function Write-Banner($text, $colour = "Cyan") {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor $colour
    Write-Host $text -ForegroundColor $colour
    Write-Host ("=" * 78) -ForegroundColor $colour
}

function Trim-To($text, $max) {
    if ($null -eq $text) { return "" }
    $t = "$text"
    if ($t.Length -le $max) { return $t }
    return $t.Substring(0, $max) + "..."
}

# One line of Claude's stream-json, turned into something a person can watch go
# past: what it is thinking, every tool with its argument, every subagent, and
# what the whole thing cost. A malformed line is printed raw rather than
# swallowed -- a stream that has stopped being JSON is itself the news.
function Show-Event($line) {
    if (-not $line -or -not $line.TrimStart().StartsWith("{")) { return }
    try { $e = $line | ConvertFrom-Json } catch { Write-Host "  $line" -ForegroundColor DarkGray; return }

    # Where the window actually goes, tallied as it goes past.
    #
    # An iteration's price is its context size times its turn count, because the whole context is
    # resent on every turn. Iteration 1 measured a 210,000-token context over 200 turns: 26.4
    # million tokens resent, one feature, one window. None of that was visible anywhere -- the
    # only number reported was a dollar figure that is not even a bill -- so the thing you would
    # change is the thing you could not see.
    if ($e.type -eq "assistant" -and $e.message.usage) {
        $u = $e.message.usage
        $ctx = [int]$u.input_tokens + [int]$u.cache_read_input_tokens + [int]$u.cache_creation_input_tokens
        $script:ctxResent += [int]$u.cache_read_input_tokens
        if ($ctx -gt $script:ctxPeak) { $script:ctxPeak = $ctx }
    }

    switch ($e.type) {
        "system" {
            if ($e.subtype -eq "init") {
                Write-Host ("  [session {0}  model {1}]" -f $e.session_id, $e.model) -ForegroundColor DarkCyan
            }
        }
        "assistant" {
            foreach ($block in $e.message.content) {
                switch ($block.type) {
                    "thinking" {
                        $t = ($block.thinking -replace '\s+', ' ').Trim()
                        if ($t) { Write-Host ("  ~ " + (Trim-To $t 300)) -ForegroundColor DarkMagenta }
                    }
                    "text" {
                        $t = ($block.text -replace '\s+', ' ').Trim()
                        if ($t) { Write-Host ("  " + (Trim-To $t 300)) -ForegroundColor Gray }
                    }
                    "tool_use" {
                        $i = $block.input
                        $detail = switch ($block.name) {
                            "Bash"       { $i.command }
                            "PowerShell" { $i.command }
                            "Read"       { $i.file_path }
                            "Write"      { $i.file_path }
                            "Edit"       { $i.file_path }
                            "Glob"       { $i.pattern }
                            "Grep"       { $i.pattern }
                            "Agent"      { "$($i.subagent_type): $($i.description)" }
                            "Task"       { "$($i.subagent_type): $($i.description)" }
                            "Workflow"   { "workflow" }
                            default      { "" }
                        }
                        $colour = if ($block.name -eq "Agent" -or $block.name -eq "Task" -or $block.name -eq "Workflow") { "Yellow" } else { "Cyan" }
                        Write-Host ("  -> " + $block.name + "  " + (Trim-To "$detail" 160)) -ForegroundColor $colour
                    }
                }
            }
        }
        "user" {
            foreach ($block in $e.message.content) {
                if ($block.type -eq "tool_result" -and $block.is_error) {
                    $t = (($block.content | Out-String) -replace '\s+', ' ').Trim()
                    Write-Host ("  !! " + (Trim-To $t 200)) -ForegroundColor Red
                }
            }
        }
        "result" {
            # subtype says "success" even when is_error is set -- it describes how
            # the turn ended, not whether it achieved anything. Trusting it printed
            # a cheerful green line over "Not logged in", so is_error decides.
            $bits = @()
            $bits += if ($e.is_error) { "FAILED" } else { "done" }
            if ($e.num_turns)      { $bits += "turns $($e.num_turns)" }
            if ($e.duration_ms)    { $bits += ("{0:N1} min" -f ($e.duration_ms / 60000.0)) }
            # NOT a bill. On a subscription nothing is charged for this: the figure
            # is what those tokens would have cost at API prices, and it is useful
            # only as a measure of how much of the plan's window an iteration ate.
            # Printed as "cost ... USD" it read as money and was reasonably taken
            # for money.
            if ($e.total_cost_usd) { $bits += ("plan usage worth {0:N2} USD at API rates" -f $e.total_cost_usd) }
            # Kept, so the journal can record what a window is actually buying.
            # Without it the only record of the burn rate scrolls off the screen.
            $script:lastResult = @{
                turns = $e.num_turns
                minutes = if ($e.duration_ms) { $e.duration_ms / 60000.0 } else { 0 }
                cost = $e.total_cost_usd
                failed = [bool]$e.is_error
            }
            $colour = if ($e.is_error) { "Red" } else { "Green" }
            Write-Host ("  == " + ($bits -join "  ")) -ForegroundColor $colour
            if ($e.is_error -and $e.result) {
                Write-Host ("     " + (Trim-To (($e.result -replace '\s+', ' ').Trim()) 300)) -ForegroundColor Red
            }
        }
    }
}

# Is a loop already running here? A stale lock from a machine switched off
# mid-run must not be mistaken for one, so the pid is checked, not the file.
#
# ...and the pid ALONE is not enough, which cost a real morning. Windows reuses
# process ids freely. A loop was killed by the machine being switched off at the
# wall, its lock survived, and by the next day pid 17276 belonged to a Realtek
# audio component started that morning -- so this function found a live process,
# reported the loop as running, and the wrap-up request went to a file nobody was
# ever going to read. The symptom is the worst kind: it says the thing you want to
# hear.
#
# So the lock records the pid AND the process start time, and both have to match.
# A recycled id cannot fake the second: it is the moment that process began, to a
# hundred nanoseconds. Reading StartTime can be refused for a process owned by
# another account, and a refusal is treated as "not mine" -- which is the safe
# direction, because the cost of clearing a lock that was live is one message and
# the cost of trusting one that is dead is a night that never runs.
#
# A lock written by an older copy of this file has no second field. It cannot be
# verified, so it is treated as stale and removed; the next run writes one that
# can be. That is a one-time self-heal rather than a migration.
function Get-RunningLoop {
    if (-not (Test-Path $lockFile)) { return $null }
    $recorded = (Get-Content $lockFile -Raw).Trim()
    if (-not $recorded) { return $null }
    $parts = $recorded -split '\s+'
    # Emits NOTHING. A helper that ends in `return $null` writes a null into this
    # function's output and the explicit `return $null` after it writes a second,
    # so the caller gets a two-element array -- which tests as TRUE and would have
    # this report a running loop always. `Remove-Item` is silent, so this is.
    $drop = { Remove-Item $lockFile -Force -ErrorAction SilentlyContinue }
    if ($parts.Count -lt 2) { & $drop; return $null }
    $live = Get-Process -Id ([int]$parts[0]) -ErrorAction SilentlyContinue
    if (-not $live) { & $drop; return $null }
    $began = $null
    try { $began = $live.StartTime.Ticks } catch { $began = $null }
    if ($null -eq $began -or "$began" -ne $parts[1]) { & $drop; return $null }
    return $live
}

# Ask Claude one trivial question before committing a night to it. Two seconds
# and a fraction of a penny, against finding out in the morning that nothing
# ran. Judged on the reply and never on the exit code: a process started with
# -PassThru and no -Wait does not reliably carry one here, and an empty value
# compares as not-zero, which failed every run however well it had gone.
# ---- who is signed in, and how it will be billed --------------------------
#
# This is the `claude` command's OWN sign-in, and it is not necessarily the
# account showing in whatever Claude app is open beside it -- checked by hand on
# one machine, the two held different accounts and nothing anywhere said so. It
# is the only one that decides what gets spent, which is why the loop asks about
# it before it asks anything else, holds the answer, and checks it again before
# every iteration. If it changes underneath, the loop stops rather than quietly
# spending a different subscription.
function Get-ClaudeAccount($claudePath) {
    $raw = ""
    try { $raw = & $claudePath auth status --json 2>&1 | Out-String } catch { }
    $json = $null
    try { $json = $raw | ConvertFrom-Json } catch { }
    if (-not $json) {
        return @{ ok = $false; why = "could not read 'claude auth status'." }
    }
    if (-not $json.loggedIn) {
        return @{ ok = $false; auth = $true; why = "Claude Code is not signed in." }
    }
    return @{
        ok           = $true
        email        = "$($json.email)"
        method       = "$($json.authMethod)"
        provider     = "$($json.apiProvider)"
        subscription = "$($json.subscriptionType)"
    }
}

# Subscription, never API credits.
#
# The CLI bills the API when it is handed an API key, and it picks one up from
# the environment without saying so -- which on an unattended loop is a bill
# instead of a night's work. These are cleared in THIS process, and Start-Process
# hands the child a copy of this environment, so every iteration inherits the
# cleared set. Nothing outside this window is touched.
function Use-SubscriptionBilling {
    $stripped = @()
    foreach ($name in @("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_BASE_URL",
                        "ANTHROPIC_MODEL", "CLAUDE_CODE_USE_BEDROCK", "CLAUDE_CODE_USE_VERTEX",
                        "AWS_BEARER_TOKEN_BEDROCK")) {
        if ([Environment]::GetEnvironmentVariable($name)) {
            Set-Item -Path ("Env:" + $name) -Value "" -ErrorAction SilentlyContinue
            Remove-Item -Path ("Env:" + $name) -ErrorAction SilentlyContinue
            $stripped += $name
        }
    }
    return $stripped
}

function Test-ClaudeReady($claudePath, $model) {
    $inFile  = Join-Path $stateDir "preflight-in.txt"
    $outFile = Join-Path $stateDir "preflight-out.txt"
    Set-Content -Path $inFile -Value "Reply with one word: ready" -Encoding utf8

    $p = Start-Process -FilePath $claudePath `
        -ArgumentList @("-p", "--model", $model, "--output-format", "json") `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile `
        -RedirectStandardError ($outFile + ".err") -NoNewWindow -PassThru
    if (-not $p.WaitForExit(180000)) {
        try { $p.Kill($true) } catch {}
        return @{ ok = $false; why = "Claude did not answer within three minutes." }
    }
    $p.WaitForExit()

    $text = if (Test-Path $outFile) { Get-Content $outFile -Raw } else { "" }
    $err  = if (Test-Path ($outFile + ".err")) { Get-Content ($outFile + ".err") -Raw } else { "" }

    if ($text -match "authentication_failed" -or $text -match "Not logged in" -or
        $err  -match "authentication_failed" -or $err  -match "Not logged in") {
        return @{ ok = $false; auth = $true; why = "The Claude CLI is not logged in." }
    }
    $json = $null
    try { $json = $text | ConvertFrom-Json } catch { }
    if ($json) {
        if ($json.is_error) {
            $said = if ($json.result) { $json.result } else { "no message" }
            return @{ ok = $false; why = ("Claude answered with an error: " + (Trim-To ($said -replace '\s+', ' ') 300)) }
        }
        return @{ ok = $true }
    }
    $detail = if ($err -and $err.Trim()) { $err } else { $text }
    if (-not $detail -or -not $detail.Trim()) { $detail = "nothing at all" }
    return @{ ok = $false; why = ("Claude replied with something unreadable: " + (Trim-To ($detail -replace '\s+', ' ') 300)) }
}

# ---- running out of usage, and coming back ---------------------------------
#
# A spent subscription is not a failure and must not be treated as one. Left
# alone it looks exactly like an iteration that refused to start, so the loop
# would count it towards the three-strikes rule and shut down -- turning "come
# back in two hours" into "nothing happened all night".
#
# So: recognise it, wait, and pick up the same iteration afterwards. The wait
# does not burn anything; it sleeps, and every ten minutes it asks the same
# trivial question the preflight asks. When that answers, the usage is back.
function Test-UsageExhausted($logPath, $errPath) {
    $text = ""
    if (Test-Path $logPath) { $text += (Get-Content $logPath -Raw) }
    if (Test-Path $errPath) { $text += (Get-Content $errPath -Raw) }
    if (-not $text) { return @{ hit = $false } }

    # Deliberately broad. The wording of a limit message is not something this
    # file controls, and the cost of missing one is a night; the cost of a false
    # positive is ten minutes of sleeping and one tiny question.
    $patterns = @(
        "usage limit reached", "usage limit will reset", "out of usage",
        "rate.?limit", "429", "quota", "insufficient credit",
        "upgrade to increase your usage", "limit_error", "exceeded your"
    )
    $hit = $false
    foreach ($p in $patterns) { if ($text -imatch $p) { $hit = $true; break } }
    if (-not $hit) { return @{ hit = $false } }

    # A reset time if one was offered, so the wait is exact rather than a poll.
    # Epoch seconds or milliseconds; anything else falls through to polling,
    # which is why nothing here has to be clever.
    $until = $null
    $m = [regex]::Match($text, '(?i)reset[^0-9]{0,40}(\d{10,13})')
    if ($m.Success) {
        $n = [double]$m.Groups[1].Value
        if ($n -gt 99999999999) { $n = $n / 1000 }
        try { $until = [DateTimeOffset]::FromUnixTimeSeconds([long]$n).LocalDateTime } catch { }
    }
    return @{ hit = $true; until = $until }
}

# Sleeps in short slices so that stopping stays instant while it waits. A loop
# that cannot be stopped for two hours is not stoppable.
function Wait-ForUsage($claudePath, $model, $until, $maxHours, $stopPath, $wrapPath) {
    $waitedMinutes = 0
    if ($until) {
        Write-Host ("  usage is spent. It resets at " + $until.ToString("HH:mm") + "; waiting.") -ForegroundColor Yellow
    } else {
        Write-Host "  usage is spent. Waiting, and trying again every ten minutes." -ForegroundColor Yellow
    }
    Write-Host "  (create STOP-LOOP, or close this window, to stop while it waits)" -ForegroundColor DarkGray

    while ($true) {
        if ((Test-Path $stopPath) -or (Test-Path $wrapPath)) {
            Write-Host "  stop requested while waiting." -ForegroundColor Yellow
            return @{ resumed = $false; stopped = $true }
        }
        if ($maxHours -gt 0 -and $waitedMinutes -ge ($maxHours * 60)) {
            return @{ resumed = $false; stopped = $false }
        }

        # Until the stated reset if there was one, otherwise ten minutes.
        $sliceTarget = 10
        if ($until) {
            $left = [math]::Ceiling(($until - (Get-Date)).TotalMinutes)
            if ($left -gt 0) { $sliceTarget = $left } else { $sliceTarget = 1; $until = $null }
        }

        for ($m = 0; $m -lt $sliceTarget; $m++) {
            for ($s = 0; $s -lt 12; $s++) {
                if ((Test-Path $stopPath) -or (Test-Path $wrapPath)) {
                    Write-Host "  stop requested while waiting." -ForegroundColor Yellow
                    return @{ resumed = $false; stopped = $true }
                }
                Start-Sleep -Seconds 5
            }
            $waitedMinutes++
        }

        Write-Host ("  waited " + $waitedMinutes + " min; checking whether usage is back...") -ForegroundColor DarkGray
        $probe = Test-ClaudeReady $claudePath $model
        if ($probe.ok) {
            Write-Host "  usage is back. Carrying on." -ForegroundColor Green
            return @{ resumed = $true; stopped = $false; waited = $waitedMinutes }
        }
    }
}

# ---- the prompts ----------------------------------------------------------
# Deliberately free of anything specific to one project: each iteration works
# out how *this* repository builds and tests itself, the same way a person
# dropped into it would.

$workPrompt = @'
You are working on the project in {{ROOT}} unattended, in a loop, with nobody watching.
This is iteration {{ITERATION}}. The person who started the loop asked for this:

    {{GOAL}}

Read {{JOURNAL}} first. It is what previous iterations wrote down, and the only memory you
have of them: each iteration is a fresh session and remembers nothing. Do not repeat work it
records, and do not undo it without saying why.

Work out how this project builds, tests and runs before you change anything — read its
README, its CLAUDE.md if it has one, its build files, its CI configuration. The journal
should record what you found so later iterations do not have to look again.

What an iteration costs, and how not to waste it
------------------------------------------------
Your context is resent to the model on EVERY turn. So the price of an iteration is roughly its
context size multiplied by its number of turns, and it is decided by what you READ rather than by
what you write. Measured on iteration 1 of this loop: a 210,000-token context over 200 turns
resent 26.4 MILLION tokens and spent a whole usage window on one feature.

So:

- NEVER read a large file whole when you want part of it. Grep for the line, then read with an
  offset and a limit around it. A 4,700-line file read whole is roughly 60,000 tokens, and you
  pay for it again on every later turn. Iteration 1 read one such file four times.
- Never re-read a file you have already read this session. It is still in your context.
- Prefer `grep -n` to reading, and read the smallest span that answers the question.
- Do not print whole logs. Grep them for the lines that matter.
- Fewer, larger steps beat many small ones, because every turn pays for the whole context again.

If the context is getting large and the work is not finished, FINISH ANYWAY: commit what is done,
write the journal, and leave the rest as the next iteration's `Next:`. A fresh session with a
small context does the remainder for a fraction of what carrying on would cost.

ultracode

What you are allowed to do
--------------------------
Anything the goal needs. Rewrite core pieces, replace a data structure, throw out an
algorithm that has stopped earning its place, restructure a subsystem. Nothing is too central
to change if changing it is what the goal actually requires. A cautious patch on a design that
is wrong costs more, over a night of iterations, than the rewrite would have.

The bar is not "small", it is "shown to work". A large change that builds, passes the tests,
measures better and is written down is welcome. A small one that was never verified is not.

Usage is not metered. Do not economise on effort: read more of the code than you think you
need, measure twice, try to refute your own conclusion before acting on it. Time spent being
sure is the point of running this overnight.

Your context is finite and this loop is continuous — many iterations follow, each a fresh
session. The journal is the only thing that crosses between them. Treat it as the deliverable.

{{AGENTS_RULE}}

How to work
-----------
- Do one focused piece of work. One. Carried all the way to committed, verified and written
  down beats four left half-finished; the next iteration takes the next piece.
- Measure, do not guess. If it is a rendering or performance change, capture evidence before
  and after and compare numbers rather than impressions. If the project has a way to take a
  screenshot, take one and *read the image*.
- A change you cannot show to be an improvement does not get committed. Revert it and write
  in the journal what you tried and what it measured. A negative result recorded is worth
  more than a hopeful change shipped.
- Never weaken, skip or delete a test to make it pass. If a test fails, either the change is
  wrong or the test is wrong, and you have to work out which and say so.
- Do not add a dependency whose licence would stop this project being published freely.

Every iteration must end with all of these true
-----------------------------------------------
1. It builds, however this project is built, with no errors and no new warnings.
2. Its whole test suite passes — all of it, not a subset. If it has no tests and the change
   is testable, adding one counts as the work.
3. It still runs, and you have checked rather than assumed.
4. Documentation matches the code. Read the relevant docs before changing behaviour and
   update them in the same commit when behaviour changes.
5. It is committed and pushed on the current branch, with a message explaining *why* in
   prose. Never force-push. Never rewrite published history.
6. The journal is appended to.

If any of these cannot be made true, stop changing things, get back to a state where they
are, and write what happened. Leaving the repository broken costs the next iteration
everything it has.

Before you finish, append to {{JOURNAL}}
----------------------------------------
    ## Iteration {{ITERATION}} — {{STAMP}}
    Goal given: <the goal, one line>
    What I did: <one or two sentences>
    Measured: <the numbers, before and after — or "nothing measurable" and why>
    Committed: <the commit hash and subject, or "nothing committed" and why>
    Next: <the single most useful thing for the next iteration to do>
    Do not retry: <anything that measured worse, so nobody tries it again>

Be honest. The journal is read by an agent with no other memory, so a hopeful summary is a
lie that costs the next several hours. If you achieved nothing, write that you achieved
nothing, and why.
'@

$wrapPrompt = @'
You are working on the project in {{ROOT}} unattended. The person who started this loop has
asked it to stop, so this is the LAST iteration. It is a wrap-up, not new work.

The goal they had given the loop was:

    {{GOAL}}

Read {{JOURNAL}} first — it is the record of every iteration before this one.

Start nothing new. Nothing speculative, no "while I am here". The one job is to leave the
repository in a state someone could pick up cold tomorrow morning.

{{AGENTS_RULE}}

Finish what is in flight
------------------------
1. Look at what is uncommitted:  git status  and  git diff
2. For each loose change decide, deliberately:
   - Finished and shown to be an improvement: verify it properly and commit it.
   - Unfinished, or not shown to be an improvement: revert it, and write in the journal what
     it was and what it measured, so the knowledge survives even though the code does not.
   Never commit something unverified to avoid throwing it away. Never throw something away
   silently.
3. Leave nothing uncommitted. The working tree must be clean.

Then confirm the repository is sound
------------------------------------
- It builds, however this project builds.
- Its whole test suite passes.
- It still runs, checked rather than assumed.
- Documentation matches the code as it now stands.

Then push
---------
Commit anything outstanding and push on the current branch. Never force-push. If the push
fails, say so loudly in the journal — a night's work nobody knows about is the worst outcome
available.

Then write the closing entry in {{JOURNAL}}
-------------------------------------------
    ## Wrap-up — {{STAMP}}
    Goal given: <the goal>
    Iterations run: <how many, from the entries above>
    What actually changed: <the commits, by subject, and what each is worth>
    Measured: <the numbers that moved, before and after>
    Reverted at wrap-up: <anything discarded, and what it measured>
    Repository state: builds / tests / pushed — say plainly whether each is true
    Where to pick up: <the most useful next thing, with enough detail to start cold>
    Do not retry: <everything tried across the loop that measured worse>

Be honest. If the loop achieved little, say so. A wrap-up that overstates what happened is
worse than none, because it is believed.
'@

$agentsYes = @'
- ultracode is on and subagents are allowed. Orchestrate where the work genuinely divides:
  parallel investigation, adversarial verification of a finding before you believe it, sweeps
  across many files. Do not fan out on work that is one edit in one file — a workflow around
  a single change is cost without benefit.
'@

$agentsNo = @'
- ultracode is on, but do the work yourself: no subagents, no workflows, no Agent tool. Take
  the thoroughness from it and not the fan-out. Spend the effort on reading more of the code
  before changing it, on trying to refute your own finding, and on measuring twice — in this
  one session, with your own hands.
'@

# ---- checks worth doing before spending a night on it ----------------------
$claude = Get-Command claude -ErrorAction SilentlyContinue
if (-not $claude) {
    Write-Banner "Claude Code is not on PATH" "Red"
    Write-Host "Install it, or run this from a terminal where 'claude' works."
    Read-Host "Press Enter to close"
    exit 1
}
if (-not (Test-Path (Join-Path $root ".git"))) {
    Write-Banner "This folder is not a git repository" "Red"
    Write-Host "$root"
    Write-Host "The loop commits and pushes its work, so it needs one. Run 'git init' first,"
    Write-Host "or move this file into the repository you meant."
    Read-Host "Press Enter to close"
    exit 1
}

# A second copy of this file is a request to stop, not a second loop.
$already = Get-RunningLoop
if ($already) {
    $runningGoal = if (Test-Path $goalFile) { (Get-Content $goalFile -Raw).Trim() } else { "(unknown)" }
    Write-Banner "A loop is already running here (pid $($already.Id))" "Yellow"
    Write-Host "It is working on: $runningGoal"
    Write-Host ""
    if (Test-Path $wrapFile) {
        Write-Host "A wrap-up has already been asked for. It will stop after the current iteration." -ForegroundColor Yellow
        Read-Host "Press Enter to close"
        exit 0
    }
    Write-Host "Wrapping up lets it finish the iteration it is in, commit and push what is done," -ForegroundColor Yellow
    Write-Host "revert what is not, update the documentation and write a closing journal entry." -ForegroundColor Yellow
    Write-Host "Nothing is lost. Killing the window instead can leave work uncommitted." -ForegroundColor Yellow
    Write-Host ""
    if ((Read-Host "Ask it to wrap up and stop? (y/N)") -match '^[Yy]') {
        Set-Content -Path $wrapFile -Value "requested" -Encoding utf8
        Write-Host ""
        Write-Host "Asked. It will finish this iteration, run one wrap-up pass, and stop." -ForegroundColor Green
    } else {
        Write-Host "Left running." -ForegroundColor DarkGray
    }
    Read-Host "Press Enter to close"
    exit 0
}

$branch = (git rev-parse --abbrev-ref HEAD 2>$null)
if (-not $branch) { $branch = "(no commits yet)" } else { $branch = $branch.Trim() }
if (git status --porcelain) {
    Write-Host ""
    Write-Host "The working tree has uncommitted changes:" -ForegroundColor Yellow
    git status --short
    Write-Host ""
    Write-Host "The loop commits and pushes on its own, so these would be swept up with its work." -ForegroundColor Yellow
    if ((Read-Host "Continue anyway? (y/N)") -notmatch '^[Yy]') { exit 0 }
}

Write-Banner "Overnight loop"
Write-Host "Project: $root"
Write-Host "Branch:  $branch"
Write-Host "Journal: $journal"
Write-Host "Logs:    $logDir"
Write-Host ""
Write-Host "It will build, test, document, commit and push on every iteration, without" -ForegroundColor Yellow
Write-Host "asking. To stop it cleanly, run this file again and say yes to wrapping up." -ForegroundColor Yellow
# ---- which account, before anything else ----------------------------------
#
# Asked rather than assumed, and asked FIRST, because everything after it spends
# the answer. The failure this replaces is silent: `claude` keeps its own
# sign-in, it is not necessarily the account showing in whatever Claude app is
# open beside it, and a whole night can go to the wrong subscription with nothing
# anywhere saying so. Checked by hand on this machine: the command line held one
# account while a different one was in use elsewhere.
$stripped = Use-SubscriptionBilling
if ($stripped.Count -gt 0) {
    Write-Host ("Ignoring for this run, so the subscription is used and not API credit: " +
                ($stripped -join ", ")) -ForegroundColor Yellow
}

# -Account picks one outright and never prompts, which is what a scheduled run
# wants. LOOP_ACCOUNT_CONFIRMED does the same for anything else scripted.
$askAccount = (-not $opt.Account) -and (-not $env:LOOP_ACCOUNT_CONFIRMED)

$account = Get-ClaudeAccount $claude.Source
while ($true) {
    Write-Host ""
    if ($account.ok) {
        Write-Host ("Signed in as: " + $account.email) -ForegroundColor Cyan
        Write-Host ("Plan:         " + $account.subscription + " subscription, billed as usage") -ForegroundColor Cyan
    } else {
        Write-Host ("Signed in as: nobody - " + $account.why) -ForegroundColor Yellow
    }

    if (-not $askAccount -and $account.ok) { break }

    Write-Host ""
    if ($account.ok) {
        $choice = Read-Host "Use this account, or sign in with a different one? (U)se / (S)witch / (Q)uit"
        if ($choice -match '^[Qq]') { Write-Host "Nothing started." ; exit 0 }
        if ($choice -notmatch '^[Ss]') { break }
    } else {
        $choice = Read-Host "Sign in now? (Y/n)"
        if ($choice -match '^[Nn]') { Write-Host "Nothing started." ; exit 0 }
    }

    # The supported sign-in, run for you rather than quoted at you. It opens a
    # browser, you pick the account there, and it writes the credential store
    # this loop reads. Run inline so it inherits this console: redirected, the
    # code it prints has nowhere to appear and it looks like a hang.
    Write-Host ""
    Write-Host "Opening the Claude sign-in. Choose the account you want the loop to use." -ForegroundColor Cyan
    Write-Host ""
    & $claude.Source auth login
    Write-Host ""

    $account = Get-ClaudeAccount $claude.Source
    if (-not $account.ok) {
        Write-Host ("Still not signed in: " + $account.why) -ForegroundColor Yellow
    }
}

if (-not $account.ok) {
    Write-Banner "Not starting: $($account.why)" "Red"
    Read-Host "Press Enter to close"
    exit 1
}

# Subscription, not API credit. `claude.ai` is the subscription sign-in; an API
# key would report differently, and on an unattended loop that is a bill rather
# than a night's work.
if ($account.method -ne "claude.ai" -or $account.provider -ne "firstParty") {
    Write-Banner "Not starting: this login would bill API credit, not a subscription." "Red"
    Write-Host ("It is signed in as " + $account.email + " by " + $account.method +
                " (" + $account.provider + ").") -ForegroundColor Yellow
    Write-Host "Sign in with a subscription account instead:  claude auth login" -ForegroundColor Yellow
    Read-Host "Press Enter to close"
    exit 1
}

# A named account is a requirement, not a preference: it refuses rather than
# quietly running as somebody else.
if ($opt.Account -and ($opt.Account -ne $account.email)) {
    Write-Banner "Not starting: signed in as the wrong account." "Red"
    Write-Host ("Wanted " + $opt.Account + ", but the command line is signed in as " +
                $account.email + ".") -ForegroundColor Yellow
    Write-Host "Run loop.bat without -Account to choose one, or:  claude auth login" -ForegroundColor Yellow
    Read-Host "Press Enter to close"
    exit 1
}

$opt.Account = $account.email
Write-Host ""
Write-Host ("Account:   " + $account.email + "  (" + $account.subscription + " subscription)") -ForegroundColor Green
Write-Host  "Billing:   subscription usage, not API credit" -ForegroundColor Green
if ($opt.FallbackModel -and $opt.FallbackModel -ne "none") {
    Write-Host ("Model:     " + $opt.Model + ", then " + $opt.FallbackModel + " when that runs out") -ForegroundColor Green
} else {
    Write-Host ("Model:     " + $opt.Model + " only; it waits for the reset when that runs out") -ForegroundColor Green
}
$ctxLine = if ($opt.Autocompact -gt 0) { "compacts at {0:N0} tokens" -f $opt.Autocompact } else { "no context limit" }
$budgetLine = if ($opt.MaxBudget -gt 0) { ", capped at {0:N2} USD-equivalent an iteration" -f $opt.MaxBudget } else { "" }
Write-Host ("Context:   " + $ctxLine + $budgetLine) -ForegroundColor Green

# What the plan will actually buy, said before the night rather than after it.
#
# Measured here: one cold iteration doing a real feature end to end was 121 turns
# and 48.9 minutes, and it spent a Pro window on its own. That is not a fault in
# the iteration -- it built, measured, documented, committed and pushed -- it is
# what the most expensive model costs. Somebody who wanted six iterations and got
# one deserves to have been told which knob decides that.
if ($account.subscription -match 'pro' -and $opt.Model -match 'opus') {
    Write-Host ""
    Write-Host "Note: Opus on a Pro plan is roughly one long iteration per window." -ForegroundColor Yellow
    if ($opt.FallbackModel -and $opt.FallbackModel -ne "none") {
        Write-Host ("      It will carry on with " + $opt.FallbackModel + " rather than wait.") -ForegroundColor Yellow
    } else {
        Write-Host "      It waits for the reset rather than finish the night on a smaller model." -ForegroundColor Yellow
    }
}
if (-not $opt.Goal) {
    Write-Host ""
    $opt.Goal = Read-Host "What should it work on"
}
if (-not $opt.Goal) { Write-Host "Nothing to do."; exit 0 }

if (-not $opt.Agents) {
    Write-Host ""
    $opt.Agents = Read-Host "Let it use subagents and workflows? (y/N)"
}
$agentsRule = if ($opt.Agents -match '^[Yy]') { $agentsYes } else { $agentsNo }
Write-Host ("Subagents: " + $(if ($opt.Agents -match '^[Yy]') { "allowed" } else { "off" })) -ForegroundColor DarkGray


Write-Host ""
Write-Host "Checking Claude answers before starting..." -ForegroundColor DarkGray
$ready = Test-ClaudeReady $claude.Source $opt.Model
if (-not $ready.ok) {
    Write-Banner "Not starting: $($ready.why)" "Red"
    if ($ready.auth) {
        Write-Host "Sign in once and it stays signed in. Claude Code and this loop share the" -ForegroundColor Yellow
        Write-Host "same login, so signing in to either one is enough:" -ForegroundColor Yellow
        Write-Host ""
        Write-Host "    claude auth login" -ForegroundColor White
        Write-Host ""
        Write-Host "Then run this file again." -ForegroundColor Yellow
    } else {
        Write-Host "If the model name is the problem, try:  loop.bat -Model opus" -ForegroundColor Yellow
    }
    Read-Host "Press Enter to close"
    exit 1
}
Write-Host "Claude answered. Starting." -ForegroundColor Green

Write-Banner "Working on: $($opt.Goal)"
if (Test-Path $stopFile) { Remove-Item $stopFile -Force }
if (Test-Path $wrapFile) { Remove-Item $wrapFile -Force }
Set-Content -Path $goalFile -Value $opt.Goal -Encoding utf8
# The pid and the moment this process began, because a pid on its own is a number
# Windows hands out again -- see Get-RunningLoop for the morning that cost.
Set-Content -Path $lockFile `
    -Value ("{0} {1}" -f $PID, [System.Diagnostics.Process]::GetCurrentProcess().StartTime.Ticks) `
    -Encoding utf8

try {
    $iteration = 0
    $wrapping = $false
    while ($true) {
        # A wrap-up is a real iteration, not a cancellation: it gets a whole turn
        # to finish what is in flight and write it down. Checked before the stop
        # file so that asking to wrap up always beats a bare stop.
        if (-not $wrapping -and (Test-Path $wrapFile)) {
            Write-Banner "Wrap-up requested. One last iteration to finish and document." "Yellow"
            Remove-Item $wrapFile -Force -ErrorAction SilentlyContinue
            $wrapping = $true
        }
        elseif (Test-Path $stopFile) {
            Write-Banner "STOP-LOOP found. Wrapping up rather than dropping the work." "Yellow"
            Remove-Item $stopFile -Force
            $wrapping = $true
        }
        elseif ($opt.MaxIterations -gt 0 -and $iteration -ge $opt.MaxIterations) {
            Write-Banner "Reached $($opt.MaxIterations) iterations. Wrapping up." "Yellow"
            $wrapping = $true
        }

        # Still the same account, and still on the subscription.
        #
        # Checked every iteration rather than once, because signing in elsewhere
        # changes the shared credential store underneath a running loop -- and
        # the failure that would cause is silent: it keeps working, on somebody
        # else's usage, until morning.
        $now = Get-ClaudeAccount $claude.Source
        if (-not $now.ok -or $now.email -ne $opt.Account -or $now.method -ne "claude.ai") {
            $became = if ($now.ok) { $now.email + " by " + $now.method } else { $now.why }
            Write-Banner "The signed-in account changed. Stopping." "Red"
            Write-Host ("Started as " + $opt.Account + ", now " + $became + ".") -ForegroundColor Yellow
            Add-Content $journal ("`n> Loop stopped at iteration $iteration : the account changed from " +
                                  $opt.Account + " to " + $became + ".`n")
            break
        }

        $iteration++
        $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        $prompt = if ($wrapping) { $wrapPrompt } else { $workPrompt }
        $prompt = $prompt.Replace("{{GOAL}}", $opt.Goal)
        $prompt = $prompt.Replace("{{ITERATION}}", "$iteration")
        $prompt = $prompt.Replace("{{JOURNAL}}", $journal)
        $prompt = $prompt.Replace("{{STAMP}}", $stamp)
        $prompt = $prompt.Replace("{{ROOT}}", $root)
        $prompt = $prompt.Replace("{{AGENTS_RULE}}", $agentsRule)

        $promptFile = Join-Path $stateDir "prompt.txt"
        Set-Content -Path $promptFile -Value $prompt -Encoding utf8
        $log = Join-Path $logDir ("iteration-{0:D4}.log" -f $iteration)

        $title = if ($wrapping) { "Wrap-up iteration $iteration - $stamp" } else { "Iteration $iteration - $stamp" }
        Write-Banner $title
        Write-Host "Log: $log"

        $headBefore = (git rev-parse HEAD 2>$null)
        $started = Get-Date
        $script:ctxResent = 0
        $script:ctxPeak = 0

        $iterationArgs = @("-p", "--model", $opt.Model, "--dangerously-skip-permissions",
                           "--verbose", "--output-format", "stream-json")
        if ($opt.FallbackModel -and $opt.FallbackModel -ne "none") {
            $iterationArgs += @("--fallback-model", $opt.FallbackModel)
        }
        if ($opt.Autocompact -gt 0) {
            $iterationArgs += @("--autocompact", "$($opt.Autocompact)")
        }
        if ($opt.MaxBudget -gt 0) {
            $iterationArgs += @("--max-budget-usd", "$($opt.MaxBudget)")
        }

        # stream-json so the window shows the work as it happens rather than a
        # wall of text an hour later. stdin from a file because the prompt is
        # long and full of quotes and newlines, and a mangled command line makes
        # an agent that works from a corrupted instruction -- worse than one that
        # fails outright.
        $process = Start-Process -FilePath $claude.Source `
            -ArgumentList $iterationArgs `
            -RedirectStandardInput $promptFile -RedirectStandardOutput $log `
            -RedirectStandardError ($log + ".err") -NoNewWindow -PassThru

        # Follow the log while it is still being written. Opened share-all,
        # because the child holds it open for writing.
        $stream = $null; $reader = $null; $killed = $false
        $deadline = (Get-Date).AddMinutes($opt.TimeoutMinutes)
        while ($true) {
            if (-not $reader -and (Test-Path $log)) {
                try {
                    $stream = [System.IO.FileStream]::new($log, [System.IO.FileMode]::Open,
                              [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                    $reader = [System.IO.StreamReader]::new($stream)
                } catch { $reader = $null }
            }
            if ($reader) { while ($null -ne ($line = $reader.ReadLine())) { Show-Event $line } }
            if ($process.HasExited) { break }
            if ((Get-Date) -gt $deadline) {
                Write-Host "  ! ran past $($opt.TimeoutMinutes) minutes; stopping it" -ForegroundColor Red
                try { $process.Kill($true) } catch {}
                $killed = $true
                Add-Content $journal "`n## Iteration $iteration - $stamp`nKilled after $($opt.TimeoutMinutes) minutes with no result.`n"
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if ($reader) {
            while ($null -ne ($line = $reader.ReadLine())) { Show-Event $line }
            $reader.Dispose(); $stream.Dispose()
        }
        if (-not $killed) { $process.WaitForExit() }

        $minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
        Write-Host "Finished in $minutes min." -ForegroundColor DarkGray

        if (Test-Path ($log + ".err")) {
            $stderrText = Get-Content ($log + ".err") -Raw
            if ($stderrText -and $stderrText.Trim()) {
                Write-Host "  stderr: $(Trim-To ($stderrText -replace '\s+',' ') 300)" -ForegroundColor Red
            }
        }

        # Out of usage is not a failed iteration. Handled before the
        # ended-in-seconds check below, which would otherwise count it towards
        # three strikes and stop the loop for the night.
        $spent = Test-UsageExhausted $log ($log + ".err")
        if ($spent.hit) {
            Add-Content $journal ("`n> Iteration $iteration hit the usage limit at $stamp. Waiting for it to reset.`n")
            $waited = Wait-ForUsage $claude.Source $opt.Model $spent.until $opt.MaxWaitHours $stopFile $wrapFile
            if ($waited.resumed) {
                # Same iteration again, from a fresh session. Nothing was
                # committed, so there is nothing half-done to reconcile.
                Add-Content $journal ("`n> Usage came back after " + $waited.waited + " minutes. Retrying iteration $iteration.`n")
                $iteration--
                continue
            }
            if ($waited.stopped) {
                Write-Banner "Stopped while waiting for usage." "Yellow"
                break
            }
            Write-Banner "Usage did not come back within $($opt.MaxWaitHours) hours. Stopping." "Yellow"
            break
        }

        # An iteration ending in seconds did no work; it refused to start. Say
        # what Claude actually said rather than guessing at causes.
        if ($minutes -lt 0.5) {
            $said = ""
            if (Test-Path $log) {
                foreach ($line in (Get-Content $log)) {
                    if ($line -match '"type"\s*:\s*"result"') {
                        try { $said = ($line | ConvertFrom-Json).result } catch { }
                    }
                }
            }
            Write-Banner "Iteration $iteration ended in seconds without doing anything" "Red"
            if ($said) { Write-Host "Claude said: $said" -ForegroundColor Red }
            if ("$said" -match "logged in|authentication") {
                Write-Host ""
                Write-Host "Sign the command-line Claude in once:  claude   then  /login" -ForegroundColor Yellow
                Read-Host "Press Enter to close"
                exit 1
            }
            if ($iteration -ge 3) {
                Write-Host "Three iterations have done nothing. Stopping rather than spinning." -ForegroundColor Red
                Read-Host "Press Enter to close"
                exit 1
            }
        }

        # The loop's own check, independent of anything the agent claimed. Both
        # of these hold in any repository, whatever it is written in.
        $headAfter = (git rev-parse HEAD 2>$null)
        if ($headAfter -ne $headBefore) {
            Write-Host "Committed: $(git log -1 --oneline)" -ForegroundColor Green
        } elseif (-not $wrapping) {
            Write-Host "No commit this iteration." -ForegroundColor Yellow
        }
        if ($script:lastResult) {
            Add-Content $journal ("`n> Iteration {0}: {1} turns, {2:N1} min, model {5}. Peak context {3:N0} tokens, {4:N1}M tokens resent. Plan usage worth {6:N2} USD at API rates - nothing is billed on a subscription.`n" -f
                                  $iteration, $script:lastResult.turns, $script:lastResult.minutes,
                                  $script:ctxPeak, ($script:ctxResent / 1000000.0), $opt.Model,
                                  $script:lastResult.cost)
            Write-Host ("  context: peak {0:N0} tokens, {1:N1}M resent over the iteration" -f
                        $script:ctxPeak, ($script:ctxResent / 1000000.0)) -ForegroundColor DarkGray
            $script:lastResult = $null
        }

        if (git status --porcelain) {
            Write-Host "Working tree left dirty:" -ForegroundColor Yellow
            git status --short | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
            Add-Content $journal "`n> Loop check after iteration $iteration : the working tree was left dirty. Commit or revert it.`n"
        }

        if ($wrapping) {
            Write-Banner "Wrapped up. Committed and documented, or written down as not." "Green"
            break
        }
        if ($opt.PauseSeconds -gt 0) {
            Write-Host "Next iteration in $($opt.PauseSeconds) s." -ForegroundColor DarkGray
            Write-Host "To finish cleanly: run this file again and say yes to wrapping up." -ForegroundColor DarkGray
            Start-Sleep -Seconds $opt.PauseSeconds
        }
    }
}
finally {
    if (Test-Path $lockFile) { Remove-Item $lockFile -Force -ErrorAction SilentlyContinue }
}

Write-Banner "Loop finished after $iteration iterations." "Green"
Write-Host "Journal: $journal"
Write-Host "Read the last entry to see where it left off."
Read-Host "Press Enter to close"
