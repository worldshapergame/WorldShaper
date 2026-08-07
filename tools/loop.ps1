# The overnight loop. Started by loop.bat in the repository root; see documentation/18-overnight-loop.md.
#
# Asks once what to work on, then runs Claude Code against that goal over and over until it
# is stopped. Each iteration is a fresh session — they share nothing but a journal file, which
# is the whole of the loop's memory.

[CmdletBinding()]
param(
    [string]$Goal = "",
    [int]$MaxIterations = 0,            # 0 means until stopped
    [int]$TimeoutMinutes = 60,          # a hung iteration must not eat the whole night
    [int]$PauseSeconds = 10,
    [string]$Model = "claude-opus-5",
    [switch]$DryRun                     # print what would run, run nothing
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# Logs and the journal live outside the repository, so an unattended agent committing its
# work does not sweep them into the history.
$stateDir = Join-Path $env:LOCALAPPDATA "WorldShaper\loop"
if (-not (Test-Path $stateDir)) { New-Item -ItemType Directory -Path $stateDir -Force | Out-Null }
$journal = Join-Path $stateDir "journal.md"
$logDir  = Join-Path $stateDir "logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

$stopFile = Join-Path $root "STOP-LOOP"
$lockFile = Join-Path $stateDir "running.lock"    # holds the pid of the live loop
$wrapFile = Join-Path $stateDir "wrap-up.request" # asks that loop to finish and stop
$goalFile = Join-Path $stateDir "goal.txt"

# Is a loop already running? A stale lock from a machine that was switched off mid-run must
# not be mistaken for one, so the pid is checked rather than the file's existence.
function Get-RunningLoop {
    if (-not (Test-Path $lockFile)) { return $null }
    $recorded = (Get-Content $lockFile -Raw).Trim()
    if (-not $recorded) { return $null }
    $live = Get-Process -Id ([int]$recorded) -ErrorAction SilentlyContinue
    if (-not $live) { Remove-Item $lockFile -Force -ErrorAction SilentlyContinue; return $null }
    return $live
}

. (Join-Path $PSScriptRoot "loop-format.ps1")

function Write-Banner($text, $colour = "Cyan") {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor $colour
    Write-Host $text -ForegroundColor $colour
    Write-Host ("=" * 78) -ForegroundColor $colour
}

# ---- checks worth doing before spending a night on it ---------------------------------
$claude = Get-Command claude -ErrorAction SilentlyContinue
if (-not $claude) {
    Write-Host "Claude Code is not on PATH. Install it, or open a terminal where 'claude' works." -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}
if (-not (Test-Path (Join-Path $root ".git"))) {
    Write-Host "$root is not a git repository, so nothing could be pushed." -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}
if (-not (Test-Path (Join-Path $root "build.bat"))) {
    Write-Host "No build.bat here. Is this the WorldShaper root?" -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}

# ---- a second copy of this script is a request to stop, not a second loop ---------------
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
    $answer = Read-Host "Ask it to wrap up and stop? (y/N)"
    if ($answer -match '^[Yy]') {
        Set-Content -Path $wrapFile -Value "requested" -Encoding utf8
        Write-Host ""
        Write-Host "Asked. It will finish the current iteration, then run one wrap-up pass and stop." -ForegroundColor Green
        Write-Host "Watch the other window; it may take a while if it is mid-build." -ForegroundColor Green
    } else {
        Write-Host "Left running." -ForegroundColor DarkGray
    }
    Read-Host "Press Enter to close"
    exit 0
}

$branch = (git rev-parse --abbrev-ref HEAD).Trim()
$dirty = (git status --porcelain)
if ($dirty) {
    Write-Host ""
    Write-Host "The working tree has uncommitted changes:" -ForegroundColor Yellow
    git status --short
    Write-Host ""
    Write-Host "The loop commits and pushes on its own, so these would be swept up with its work." -ForegroundColor Yellow
    $answer = Read-Host "Continue anyway? (y/N)"
    if ($answer -notmatch '^[Yy]') { exit 0 }
}

if (-not $Goal) {
    Write-Banner "WorldShaper overnight loop"
    Write-Host "Branch:  $branch"
    Write-Host "Journal: $journal"
    Write-Host "Logs:    $logDir"
    Write-Host ""
    Write-Host "It will build, test, screenshot, document, commit and push on every iteration," -ForegroundColor Yellow
    Write-Host "without asking. Stop it with Ctrl+C, or by creating a file named STOP-LOOP here." -ForegroundColor Yellow
    Write-Host ""
    $Goal = Read-Host "What should it work on"
}
if (-not $Goal) { Write-Host "Nothing to do."; exit 0 }

# Subagents are a real choice rather than a setting, because they change what the night costs
# and what the journal ends up saying. Fanning out finds more at once; doing it single-handed
# keeps every conclusion traceable to something that was actually read.
Write-Host ""
$useAgents = Read-Host "Let it use subagents and workflows? (y/N)"
if ($useAgents -match '^[Yy]') {
    $agentsRule = @"
- ultracode is on and subagents are allowed. Orchestrate where the work genuinely divides:
  parallel investigation, adversarial verification of a finding before you believe it, sweeps
  across many files. Do not fan out on work that is one edit in one file — a workflow around a
  single change is cost without benefit.
"@
} else {
    $agentsRule = @"
- ultracode is on, but do the work yourself: no subagents, no workflows, no Agent tool. Take
  the thoroughness from it and not the fan-out. Spend the effort on reading more of the code
  before changing it, on trying to refute your own finding, and on measuring twice — in this
  one session, with your own hands.
"@
}
Write-Host ("Subagents: " + $(if ($useAgents -match '^[Yy]') { "allowed" } else { "off" })) -ForegroundColor DarkGray

$template = Get-Content (Join-Path $PSScriptRoot "loop-prompt.txt") -Raw
$wrapTemplate = Get-Content (Join-Path $PSScriptRoot "loop-wrapup-prompt.txt") -Raw

Write-Banner "Working on: $Goal"
if (Test-Path $stopFile) { Remove-Item $stopFile -Force }
if (Test-Path $wrapFile) { Remove-Item $wrapFile -Force }
Set-Content -Path $goalFile -Value $Goal -Encoding utf8
Set-Content -Path $lockFile -Value $PID -Encoding utf8

# The lock has to go even if this window is closed with the X or Ctrl+C, or the next launch
# would think a dead loop is still running and only ever offer to wrap it up.
$cleanup = {
    if (Test-Path $lockFile) { Remove-Item $lockFile -Force -ErrorAction SilentlyContinue }
}
Register-EngineEvent PowerShell.Exiting -Action $cleanup | Out-Null
try {

$iteration = 0
$wrapping = $false
while ($true) {
    # A wrap-up is a real iteration, not a cancellation: it gets a whole turn to finish what
    # is in flight, commit it, and write the closing entry. Checked before the stop file so
    # that asking to wrap up always beats a bare stop.
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
    elseif ($MaxIterations -gt 0 -and $iteration -ge $MaxIterations) {
        Write-Banner "Reached $MaxIterations iterations. Wrapping up." "Yellow"
        $wrapping = $true
    }

    $iteration++
    $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    # One replacement per statement. Windows PowerShell will not carry a method chain across
    # a line break, and the failure is a parse error at the end of the file rather than here.
    $prompt = if ($wrapping) { $wrapTemplate } else { $template }
    $prompt = $prompt.Replace("{{GOAL}}", $Goal)
    $prompt = $prompt.Replace("{{ITERATION}}", "$iteration")
    $prompt = $prompt.Replace("{{JOURNAL}}", $journal)
    $prompt = $prompt.Replace("{{STAMP}}", $stamp)
    $prompt = $prompt.Replace("{{AGENTS_RULE}}", $agentsRule)

    $promptFile = Join-Path $stateDir "prompt.txt"
    Set-Content -Path $promptFile -Value $prompt -Encoding utf8
    $log = Join-Path $logDir ("iteration-{0:D4}.log" -f $iteration)

    $title = if ($wrapping) { "Wrap-up iteration $iteration - $stamp" } else { "Iteration $iteration - $stamp" }
    Write-Banner $title
    Write-Host "Log: $log"

    if ($DryRun) {
        Write-Host "--- would run: claude -p (prompt below) --dangerously-skip-permissions ---" -ForegroundColor DarkGray
        Write-Host $prompt
        break
    }

    # stdin from the prompt file rather than the command line: the prompt is long, contains
    # quotes and newlines, and a command line that gets mangled produces an agent working from
    # a corrupted instruction, which is worse than one that fails outright.
    $started = Get-Date
    # The model is named explicitly rather than left to whatever the session default happens
    # to be: an overnight run is the last place to discover it was quietly on a smaller one.
    #
    # stream-json, so the window shows the work as it happens rather than a wall of text an
    # hour later: each thought, each file touched, each command run. Watching an overnight run
    # for two minutes and seeing it is doing something sensible is worth more than any summary.
    $process = Start-Process -FilePath $claude.Source `
        -ArgumentList @("-p", "--model", $Model, "--dangerously-skip-permissions",
                        "--verbose", "--output-format", "stream-json") `
        -RedirectStandardInput $promptFile `
        -RedirectStandardOutput $log `
        -RedirectStandardError ($log + ".err") `
        -NoNewWindow -PassThru

    # Follow the log while it is still being written. Opened share-all, because the child has
    # it open for writing and an exclusive read would fail.
    $stream = $null
    $reader = $null
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $killed = $false
    while ($true) {
        if (-not $reader -and (Test-Path $log)) {
            try {
                $stream = [System.IO.FileStream]::new($log, [System.IO.FileMode]::Open,
                          [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                $reader = [System.IO.StreamReader]::new($stream)
            } catch { $reader = $null }
        }
        if ($reader) {
            while ($null -ne ($line = $reader.ReadLine())) { Show-Event $line }
        }
        if ($process.HasExited) { break }
        if ((Get-Date) -gt $deadline) {
            Write-Host "  ! ran past $TimeoutMinutes minutes; stopping it" -ForegroundColor Red
            try { $process.Kill($true) } catch {}
            $killed = $true
            Add-Content $journal "`n## Iteration $iteration - $stamp`nKilled after $TimeoutMinutes minutes with no result.`n"
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
    Write-Host "Finished in $minutes min (exit $($process.ExitCode))." -ForegroundColor DarkGray

    # An iteration that dies in seconds is not doing work, it is refusing to start — a wrong
    # model name, an expired login. Said plainly on the first one, because the alternative is
    # finding a thousand identical failures in the morning.
    if ($iteration -eq 1 -and $process.ExitCode -ne 0 -and $minutes -lt 0.5) {
        Write-Host ""
        Write-Host "Iteration 1 failed immediately. Claude is not starting, so nothing will happen" -ForegroundColor Red
        Write-Host "all night. Common causes: the model name '$Model' is not one this CLI accepts," -ForegroundColor Red
        Write-Host "or you are not logged in. What it said:" -ForegroundColor Red
        if (Test-Path ($log + ".err")) { Get-Content ($log + ".err") -Tail 15 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red } }
        if (Test-Path $log)            { Get-Content $log -Tail 15            | ForEach-Object { Write-Host "  $_" -ForegroundColor Red } }
        Write-Host ""
        Write-Host "Try:  loop.bat -Model opus" -ForegroundColor Yellow
        Read-Host "Press Enter to close"
        exit 1
    }

    if (Test-Path ($log + ".err")) {
        $stderrText = (Get-Content ($log + ".err") -Raw)
        if ($stderrText -and $stderrText.Trim()) {
            Write-Host "  stderr:" -ForegroundColor Red
            Get-Content ($log + ".err") -Tail 8 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
        }
    }

    # A quick truth check of our own, independent of anything the agent claimed. If the
    # repository is broken the next iteration needs to know that before it does anything else.
    $testsExe = Join-Path $root "build\bin\ws_tests.exe"
    if (Test-Path $testsExe) {
        $testOutput = & $testsExe 2>&1 | Select-String "Status:" | Select-Object -Last 1
        if ($testOutput -notmatch "SUCCESS") {
            Write-Host "TESTS ARE FAILING after iteration $iteration." -ForegroundColor Red
            Add-Content $journal "`n> Loop check after iteration $iteration : the test suite was FAILING. Fix this before anything else.`n"
        } else {
            Write-Host "Tests pass." -ForegroundColor Green
        }
    }

    if ($wrapping) {
        Write-Banner "Wrapped up. Everything committed and documented, or written down as not." "Green"
        break
    }

    if ($PauseSeconds -gt 0) {
        Write-Host "Next iteration in $PauseSeconds s." -ForegroundColor DarkGray
        Write-Host "To finish cleanly: run loop.bat again and say yes to wrapping up." -ForegroundColor DarkGray
        Start-Sleep -Seconds $PauseSeconds
    }
}

}
finally {
    if (Test-Path $lockFile) { Remove-Item $lockFile -Force -ErrorAction SilentlyContinue }
}

Write-Banner "Loop finished after $iteration iterations. Journal: $journal" "Green"
Write-Host "Read the last entry there to see where it left off."
Read-Host "Press Enter to close"
