# Turning Claude Code's stream-json into something a person can watch go past.
#
# Its own file so it can be tested without starting a loop: dot-source it and feed it lines.
# See tools/loop.ps1.

function Trim-To($text, $max) {
    if ($null -eq $text) { return "" }
    $t = "$text"
    if ($t.Length -le $max) { return $t }
    return $t.Substring(0, $max) + "..."
}

# What it is thinking, every tool it reaches for and with what, every subagent it starts, and
# what the whole thing cost. A malformed line is printed raw rather than swallowed — a stream
# that has stopped being JSON is itself the news.
function Show-Event($line) {
    if (-not $line -or -not $line.TrimStart().StartsWith("{")) { return }
    try { $e = $line | ConvertFrom-Json } catch { Write-Host "  $line" -ForegroundColor DarkGray; return }

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
                            "Workflow"   { "workflow" }
                            default      { "" }
                        }
                        $colour = if ($block.name -eq "Agent" -or $block.name -eq "Workflow") { "Yellow" } else { "Cyan" }
                        Write-Host ("  -> " + $block.name + "  " + (Trim-To "$detail" 160)) -ForegroundColor $colour
                    }
                }
            }
        }
        "user" {
            # Tool results. Only the failures are worth the room.
            foreach ($block in $e.message.content) {
                if ($block.type -eq "tool_result" -and $block.is_error) {
                    $t = (($block.content | Out-String) -replace '\s+', ' ').Trim()
                    Write-Host ("  !! " + (Trim-To $t 200)) -ForegroundColor Red
                }
            }
        }
        "result" {
            # subtype says "success" even when is_error is set — it describes how the turn
            # ended, not whether it achieved anything. Trusting it printed a cheerful green
            # line over "Not logged in", so is_error is what decides the word and the colour.
            $bits = @()
            $bits += if ($e.is_error) { "FAILED" } else { "done" }
            if ($e.num_turns)      { $bits += "turns $($e.num_turns)" }
            if ($e.duration_ms)    { $bits += ("{0:N1} min" -f ($e.duration_ms / 60000.0)) }
            if ($e.total_cost_usd) { $bits += ("cost {0:N2} USD" -f $e.total_cost_usd) }
            $colour = if ($e.is_error) { "Red" } else { "Green" }
            Write-Host ("  == " + ($bits -join "  ")) -ForegroundColor $colour
            if ($e.is_error -and $e.result) {
                Write-Host ("     " + (Trim-To (($e.result -replace '\s+', ' ').Trim()) 300)) -ForegroundColor Red
            }
        }
    }
}
