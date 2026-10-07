# test.ps1 - run the self-tests and the out-of-process probes.
#
#   .\test.ps1                     every test
#   .\test.ps1 shot                just shot
#   .\test.ps1 picker,envtool       two of them (the comma is required - a
#                                  PowerShell array parameter takes one value
#                                  from the command line, not several)
#   .\test.ps1 -SelfTestOnly       skip the probes (seconds, no windows)
#   .\test.ps1 -ProbeOnly          skip the --selftest pass
#   .\test.ps1 -List               print the plan and stop
#
# Packing is .\build.ps1, and it takes -Only for the same reason this script
# does. A change to src\shot\editor.c should cost one relink and shot's tests,
# not a full build followed by every probe on the machine - the tray and
# editor probes move the real mouse and take the foreground, so they are also
# the slow half. The short loop is:
#
#   .\build.ps1 -Only shot ; .\test.ps1 shot
#
# A single probe is still one command away, with its own switches:
#
#   .\test\probe-tray-stress.ps1 -Rounds 5
#
# Probes that need a real desktop (the tray lives in Explorer's process and
# only exists while the shell is up) cannot be skipped by logic - if one of
# them cannot run here it reports FAILED, so a green run always means the
# tests actually ran.

[CmdletBinding()]
param(
    [string[]]$Target = @(),
    [switch]$SelfTestOnly,
    [switch]$ProbeOnly,
    [switch]$List
)

$ErrorActionPreference = 'Stop'
# The probes and the exes write to stderr; under 'Stop' the first such line
# would abort the run, so relax it and judge each test by its own result.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin  = Join-Path $root 'bin'
$dir  = Join-Path $root 'test'
$logDir = Join-Path $root 'build'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }

# The PowerShell that is running this script, used to launch the probes.
$hostExe = Join-Path $PSHOME 'powershell.exe'
if (-not (Test-Path $hostExe)) { $hostExe = 'powershell.exe' }

# Wrapper the probes are launched through. Four things it has to get right,
# and each of them was a way for a probe's verdict to be somebody else's:
#
#   - it puts the console on UTF-8, so the programs' own Chinese log lines
#     survive instead of arriving as mojibake in the one output you need to
#     read when something goes red;
#   - it merges every stream into stdout, so a probe's warnings are read too;
#   - it forwards any arguments, because several probes are told which program
#     to open (the About box belongs to three of them but is checked three
#     times, once per program);
#   - it re-raises the probe's exit code as the process exit code. A script
#     reached through `&` that calls `exit 3` only sets $LASTEXITCODE in the
#     caller - the host process would still exit 0, and a failing probe would
#     be reported as passing.
$runner = Join-Path $logDir 'run-probe.ps1'
$runnerText = @'
param([Parameter(Mandatory=$true)][string]$Probe, [string[]]$Rest)
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
# Two branches, and no intermediate variable, because both of the obvious ways
# to write this are wrong in a way that shows up as a passing test.
#
# Splatting the parameter directly would pass one null argument when there are
# no arguments, because splatting a null is not the same as passing nothing: the
# null binds to the callee's first positional parameter and takes its default
# with it. Most probes here start with [string]$Exe so it landed there and the
# [int] parameters behind it survived - but probe-lifecycle.ps1 starts with
# [int]$Rounds, so it was handed [int]$null, which is 0 rather than 3, and
# reported "opened its window on every round (0 of 0)" in green.
#
# Normalising into a local first does not fix it, because PowerShell variable
# names are case-insensitive: `$rest = @()` and `$Rest` are the same variable,
# so clearing the local cleared the parameter and the test that followed saw
# an empty array and forwarded nothing. That is the version that made
# probe-about-menu lose its program name and die on a null hashtable key.
#
# Branching on the parameter and calling it the two ways it can be called has
# no variable to get wrong.
if ($Rest) {
    & $Probe @Rest *>&1 | Out-String -Width 200
} else {
    & $Probe *>&1 | Out-String -Width 200
}
if ($null -eq $LASTEXITCODE) { exit 0 } else { exit $LASTEXITCODE }
'@
[System.IO.File]::WriteAllText($runner, $runnerText, [System.Text.Encoding]::ASCII)

# Which probes belong to which program. They are listed rather than globbed so
# that `.\test.ps1 picker` cannot pick up a shot probe sitting in the same
# folder, and so the order they run in stays fixed: several of them drive the
# same tray icon and the same overlay, and one at a time is the only way they
# can be trusted.
#
# Two fields beyond file and label:
#
#   args   passed to the probe. The About box is one piece of shared code and
#          is checked once per program that can open it, so the probe is told
#          which one to open rather than being duplicated.
#   noExe  for an entry that is not about one program's binary. The lifecycle
#          pass opens all four in turn, and the harness's "is it built" and
#          "--selftest" steps do not apply to it.
$plan = @(
    @{ name = 'shot'; probes = @(
        @{ file = 'probe-tray-click.ps1';  label = 'tray click' },
        @{ file = 'probe-tray-menu.ps1';   label = 'tray menu' },
        @{ file = 'probe-tray-real.ps1';   label = 'real mouse' },
        # Focus first, before the event matrix: the matrix sends real chords
        # through the keyboard, and whether that is possible at all is the
        # question this answers.
        @{ file = 'probe-focus.ps1';       label = 'who owns the keyboard' },
        @{ file = 'probe-hotkey.ps1';      label = 'shortcut dialog' },
        # After it, because it clears the stored shortcut to reach the default
        # and puts it back afterwards: the pair is only meaningful in that
        # order.
        @{ file = 'probe-hotkey-conflict.ps1'; label = 'shortcut conflict asks' },
        @{ file = 'probe-mosaic.ps1';      label = 'mosaic tool' },
        @{ file = 'probe-toolbar-tip.ps1'; label = 'toolbar tips' },
        @{ file = 'probe-paintcost.ps1';   label = 'repaint cost under a drag' },
        @{ file = 'probe-events-shot.ps1'; label = 'event matrix' },
        @{ file = 'probe-capture-e2e.ps1'; label = 'end-to-end capture' },
        @{ file = 'probe-tray-stress.ps1'; label = '20 captures back to back' },
        @{ file = 'probe-about-menu.ps1';  label = 'about from the tray menu'; args = @('shot') }
    )},
    @{ name = 'picker'; probes = @(
        @{ file = 'probe-picker-wheel.ps1';  label = 'wheel, harmony, palette' },
        @{ file = 'probe-events-picker.ps1'; label = 'event matrix' },
        # First, and with the real cursor: the eyedropper is a fullscreen
        # topmost window, so while it is armed it owns hit-testing over the
        # notification area too, and the shot tray icon stops being a screenshot
        # and becomes a colour. It drives the actual pointer because that
        # stand-down is decided by where the cursor is, which a posted
        # WM_MOUSEMOVE cannot exercise.
        @{ file = 'probe-tray-isolation.ps1'; label = 'tray isolation' },
        # Through the tray menu, not probe-about: picker has no window button,
        # its 关于 lives in the tray menu, and probe-about looks for a button in
        # the window and throws when there is none. probe-about-menu drives
        # the entry point a user actually clicks.
        @{ file = 'probe-about-menu.ps1';     label = 'about from the tray menu'; args = @('picker') }
    )},
    @{ name = 'envtool'; probes = @(
        @{ file = 'probe-list-click.ps1';    label = 'list selection' },
        @{ file = 'probe-events-envtool.ps1'; label = 'event matrix' },
        @{ file = 'probe-about.ps1';         label = 'about box'; args = @('envtool') },
        @{ file = 'probe-delete-click.ps1';  label = 'delete button' }
        # probe-delete-click.ps1 spent a while red and out of this list: its
        # transcribed layout had missed the banner band the window grew, so it
        # clicked 34px below the real 删除 button and reported the strip dead
        # while 复制值 beside it appeared to work. The formula now follows
        # build_layout (banH + gap included), it is back in the default run,
        # and a layout change in envtool.c has to keep it in step or this
        # turns red again - which is the probe doing its job.
    )},
    @{ name = 'slice'; probes = @(
        @{ file = 'probe-slice.ps1';         label = 'batch split and canvas' },
        @{ file = 'probe-slice-ui.ps1';      label = 'window buttons' },
        @{ file = 'probe-events-slice.ps1';  label = 'event matrix' },
        @{ file = 'probe-about.ps1';         label = 'about box'; args = @('slice') }
    )},
    # Last, because it starts and stops everything several times over and
    # would otherwise be measuring the leftovers of the probes above it.
    @{ name = 'lifecycle'; noExe = $true; probes = @(
        @{ file = 'probe-lifecycle.ps1';     label = 'open, close, open again' }
    )}
)

# An empty -Target means every program in $plan. The default is empty rather
# than a second copy of the names for that reason: adding a program is one
# entry in $plan, and `.\test.ps1` with no arguments cannot quietly stop
# covering it.
$known = @($plan | ForEach-Object { $_.name })
if ($Target.Count) {
    $unknown = @($Target | Where-Object { $known -notcontains $_ })
    if ($unknown.Count) {
        throw "unknown target(s): $($unknown -join ', ') (expected $($known -join ', '))"
    }
}
if ($SelfTestOnly -and $ProbeOnly) {
    throw "-SelfTestOnly and -ProbeOnly ask for opposite things"
}
$selected = if ($Target.Count) { @($plan | Where-Object { $Target -contains $_.name }) } else { $plan }

if ($List) {
    Write-Host "would run:" -ForegroundColor Cyan
    foreach ($t in $selected) {
        if ($t.noExe) {
            Write-Host "  (no --selftest: this entry spans all four programs)"
        } elseif (-not $ProbeOnly) { Write-Host ("  {0}.exe --selftest" -f $t.name) }
        if (-not $SelfTestOnly) {
            foreach ($p in $t.probes) {
                $a = if ($p.args) { " " + ($p.args -join ' ') } else { '' }
                Write-Host ("  {0}: {1}{2}" -f $t.name, $p.label, $a)
            }
        }
    }
    exit 0
}

# Leftovers from an interrupted run are not harmless. A stale instance keeps its
# window on the desktop, wins activation, and swallows the real clicks and key
# presses that the tray, editor and window-button probes depend on - the symptom
# is a probe failing on a program that is fine, which is worse than a red run
# because it teaches the reader to ignore red. Cleaning up before the run is
# cheaper than diagnosing that afterwards.
#
# Only processes whose executable lives in this tree's bin are touched, so a
# separately installed copy of the same tool is never killed by accident.
function Stop-StaleInstances {
    $names = @('shot', 'picker', 'envtool', 'slice')
    $stale = @(Get-Process -Name $names -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and $_.Path.StartsWith($bin, [StringComparison]::OrdinalIgnoreCase) }
        catch { $false }   # no rights to read Path; not ours to judge, so leave it
    })
    if (-not $stale.Count) { return }
    Write-Host ("`nstopping {0} leftover instance(s) from an earlier run:" -f $stale.Count) -ForegroundColor Yellow
    foreach ($s in $stale) {
        Write-Host ("  {0}.exe pid {1}" -f $s.ProcessName, $s.Id)
        try { Stop-Process -Id $s.Id -Force -ErrorAction Stop } catch {
            Write-Host ("    could not stop it: {0}" -f $_.Exception.Message) -ForegroundColor Red
        }
    }
    # Give the window manager a moment to drop the windows, or the first probe
    # races the teardown and sees a window that is on its way out.
    Start-Sleep -Milliseconds 600
    $left = @(Get-Process -Name $names -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and $_.Path.StartsWith($bin, [StringComparison]::OrdinalIgnoreCase) }
        catch { $false }
    })
    if ($left.Count) {
        Write-Host ("  {0} could not be stopped - real-input probes may report the wrong program" -f $left.Count) -ForegroundColor Red
    }
}

$script:results = @()
function Add-Result($name, $state, $secs, $note) {
    $script:results += [pscustomobject]@{
        Test = $name; State = $state; Secs = $secs; Note = $note
    }
}

function Invoke-SelfTest($name) {
    $exe = Join-Path $bin "$name.exe"
    Write-Host ("`nself-test: {0}.exe --selftest" -f $name) -ForegroundColor Cyan
    if (-not (Test-Path $exe)) {
        Write-Host "  not built - run .\build.ps1 -Only $name" -ForegroundColor Red
        Add-Result "$name --selftest" 'FAIL' 0 'not built'
        return
    }
    # Start-Process -Wait -PassThru, not `& $exe --selftest 2>&1`. These are
    # GUI-subsystem binaries: the call operator does not wait for those, so
    # $LASTEXITCODE would never be set and every self-test would be reported
    # as passing without having run once - the one failure mode that matters
    # most, because it is silent. Output is deliberately left on the console
    # instead of captured, so the report reads as it happens and the exe's own
    # "self-test PASSED" line stays visible.
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $sp = Start-Process -FilePath $exe -ArgumentList '--selftest' -Wait -PassThru -NoNewWindow
    $sw.Stop()
    if ($sp.ExitCode -ne 0) {
        Write-Host ("  SELF-TEST FAILED (exit {0})" -f $sp.ExitCode) -ForegroundColor Red
        Add-Result "$name --selftest" 'FAIL' $sw.Elapsed.TotalSeconds "exit $($sp.ExitCode)"
    } else {
        Write-Host "  self-test passed" -ForegroundColor Green
        Add-Result "$name --selftest" 'PASS' $sw.Elapsed.TotalSeconds ''
    }
}

function Invoke-Probe($target, $probe, $noExe) {
    $script = Join-Path $dir $probe.file
    $name   = "$target / $($probe.label)"
    Write-Host ("`nchecking {0} {1}..." -f $target, $probe.label) -ForegroundColor Cyan
    if (-not $noExe) {
        $exe = Join-Path $bin "$target.exe"
        if (-not (Test-Path $exe)) {
            Write-Host ("  {0}.exe is not built - run .\build.ps1 -Only {0}" -f $target) -ForegroundColor Red
            Add-Result $name 'FAIL' 0 'not built'
            return
        }
    }
    if (-not (Test-Path $script)) {
        Write-Host "  $script is missing - skipped" -ForegroundColor Yellow
        Add-Result $name 'SKIP' 0 'no script'
        return
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    # Each probe runs in its own PowerShell, and that is not tidiness - it is
    # what makes the result mean anything.
    #
    # Invoked in-process with `&`, a script that never reaches an `exit` leaves
    # $LASTEXITCODE exactly as the previous probe left it. Four of the probes
    # in this plan are in that position (diag-shot, tray click, tray menu,
    # tray real), so their verdict was whatever the probe before them had
    # returned: the same test could come out green or red purely by moving up
    # or down the list, and a probe that failed without printing FAIL - the
    # one failure mode that is invisible - came out green whenever the probe
    # above it had passed. A child process has its own $LASTEXITCODE, so the
    # code read below is the probe's own and nobody else's.
    #
    # Nothing is lost by moving the run out of process: the output was already
    # captured and printed after the probe finished, not as it happened.
    #
    # The child is also told to write UTF-8. The programs log their own
    # messages in UTF-8, and with the console on a legacy code page every
    # Chinese line in a failure comes back as mojibake - which is exactly the
    # output you need to read when a check goes red.
    $outFile = Join-Path $logDir ("probe-{0}.log" -f ($name -replace '[^A-Za-z0-9]+', '-'))
    # @($null) is a one-element array holding a null, and Start-Process rejects
    # that outright - so the arguments are only appended when there are any.
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $runner, $script)
    if ($probe.args) { $argList += @($probe.args) }
    $p = Start-Process -FilePath $hostExe `
            -ArgumentList $argList `
            -Wait -PassThru -NoNewWindow -RedirectStandardOutput $outFile -RedirectStandardError "$outFile.err"
    $code = $p.ExitCode
    $sw.Stop()

    $out = @()
    if (Test-Path $outFile) {
        $text = [System.IO.File]::ReadAllText($outFile, [System.Text.Encoding]::UTF8)
        if ($text) { $out = @($text -split "`r?`n" | Where-Object { $_ -ne '' }) }
    }
    $errFile = "$outFile.err"
    if ((Test-Path $errFile) -and (Get-Item $errFile).Length -gt 0) {
        $errText = [System.IO.File]::ReadAllText($errFile, [System.Text.Encoding]::UTF8)
        if ($errText) { $out += @($errText -split "`r?`n" | Where-Object { $_ -ne '' }) }
    }

    # One filter for every probe. They all announce themselves on RESULT and
    # FAIL lines; the extra patterns are the progress lines the tray probes
    # emit per round, which is what makes a 20-round run look like progress
    # instead of a pause.
    $out | ForEach-Object {
        $line = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.ToString() } else { "$_" }
        if ($line -match 'RESULT|FAIL|SKIP|ok \[|round |since round|after \d+ rounds|crash log delta|^size \d+') {
            $tone = if ($line -match 'FAIL|APP DIED') { 'Red' }
                    elseif ($line -match 'SKIP') { 'Yellow' }
                    elseif ($line -match 'RESULT' -and $line -notmatch 'failed') { 'Green' }
                    else { 'DarkGray' }
            Write-Host "  $line" -ForegroundColor $tone
        }
    }

    # A probe fails either by its exit code or by saying so. Both are read,
    # because a probe can be wired up wrong in either direction at once.
    #
    # The exit code is trustworthy now that each probe runs in its own process:
    # a script that dies on an uncaught exception takes its host down with a
    # non-zero code, which is exactly the case the in-process version could
    # not see. The words are still read as well, because a probe that reports
    # a failure without setting a code is a real thing - diag-shot used to be
    # one - and it must not come out green.
    $shouted = @($out | Where-Object { "$_" -match 'FAIL|APP DIED' })
    $verdict = @($out | Where-Object { "$_" -match 'RESULT:' })
    if ($code -ne 0) {
        Write-Host ("  FAILED (exit {0})" -f $code) -ForegroundColor Red
        Add-Result $name 'FAIL' $sw.Elapsed.TotalSeconds "exit $code"
    } elseif ($shouted.Count) {
        Write-Host "  FAILED (reported a failure but exited 0)" -ForegroundColor Red
        Add-Result $name 'FAIL' $sw.Elapsed.TotalSeconds 'failed in output'
    } else {
        # A probe with no RESULT line has not been asked to state a verdict -
        # some of them narrate what they found and save a screenshot. That is
        # noted rather than failed on, so the summary still shows which of the
        # runs asserted nothing.
        $note = if ($verdict.Count) { '' } else { 'no RESULT line - ran, asserted nothing' }
        if ($note) { Write-Host "  probe ran (asserted nothing)" -ForegroundColor Yellow }
        else { Write-Host "  probe passed" -ForegroundColor Green }
        Add-Result $name 'PASS' $sw.Elapsed.TotalSeconds $note
    }
}

$total = [Diagnostics.Stopwatch]::StartNew()
Stop-StaleInstances
foreach ($t in $selected) {
    if (-not $t.noExe) { if (-not $ProbeOnly) { Invoke-SelfTest $t.name } }
    if (-not $SelfTestOnly) {
        foreach ($p in $t.probes) { Invoke-Probe $t.name $p $t.noExe }
    }
}
$total.Stop()

$failed   = @($results | Where-Object { $_.State -eq 'FAIL' })
$skipped  = @($results | Where-Object { $_.State -eq 'SKIP' })
Write-Host "`nsummary" -ForegroundColor Cyan
foreach ($r in $results) {
    $mark = switch ($r.State) { 'PASS' { 'ok  ' } 'SKIP' { 'skip' } default { 'FAIL' } }
    $tone = switch ($r.State) { 'PASS' { 'Green' } 'SKIP' { 'Yellow' } default { 'Red' } }
    $note = if ($r.Note) { "  ($($r.Note))" } else { '' }
    Write-Host ("  {0}  {1,6:n1}s  {2}{3}" -f $mark, $r.Secs, $r.Test, $note) -ForegroundColor $tone
}
Write-Host ("`n{0} passed, {1} failed, {2} skipped in {3:n1}s" -f `
    ($results.Count - $failed.Count - $skipped.Count), $failed.Count, $skipped.Count,
    $total.Elapsed.TotalSeconds) -ForegroundColor $(if ($failed.Count) { 'Red' } else { 'Green' })
exit $(if ($failed.Count) { 1 } else { 0 })
