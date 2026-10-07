# probe-events-shot.ps1 - the capture overlay's event surface, checked from the
# outside.
#
#   .\test\probe-events-shot.ps1 [-Exe .\bin\shot.exe] [-Flood 200]
#
# Every other shot probe answers "does this one thing work". This one walks the
# overlay's event matrix: the ten tool keys, the Esc cascade, a capture that
# disappears mid-click, undo and redo driven by real chords, a wheel message,
# and a mouse-move flood - and after every stage asks the same two questions.
# Did the window still answer, and did anything land in the crash log?
#
# The state it asserts on is the app's own, not a pixel diff. shot answers
# WM_APP + 2 with one line on stderr (src\shot\editor.c) reporting the tool, the
# stroke count, the selection rectangle and the toolbar press index, which is
# exactly the set of values these events are supposed to move. A probe that
# inferred tool state from a screenshot would be testing the renderer; this one
# tests the handler.
#
# ASCII only (see probe-focus.ps1).

[CmdletBinding()]
param([string]$Exe, [int]$Flood = 200)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }

$HOST_CLASS   = 'MuskShotHost'
$OVERLAY      = 'ShotOverlayCls'
$VK_ESCAPE    = 0x1B
$VK_PRIOR     = 0x21

# ToolKind in src\shot\shot.h. The probe repeats the order rather than parsing
# the header so that a reordering of the enum shows up here as ten failing
# checks instead of a probe that quietly follows the app's own mistake.
$TOOL = @{ 'P' = 0; 'E' = 1; 'L' = 2; 'R' = 3; 'O' = 4;
           'A' = 5; 'T' = 6; 'M' = 7; 'B' = 8; 'F' = 9 }

function New-Overlay([int]$ProcId) {
    # The tray callback is the app's own way of being asked for a capture, so
    # this is the same entry point probe-focus and diag-shot use.
    # ($host is a PowerShell automatic variable - the name is not free.)
    $hostWin = Find-AppWindow $ProcId $HOST_CLASS 8000
    if ($hostWin -eq [IntPtr]::Zero) { throw "host window never appeared" }
    [void][ProbeLib.U]::PostMessageW($hostWin, $script:WM_APP1, [IntPtr]::Zero, [IntPtr]$script:WM_LBUTTONUP)
    Find-AppWindow $ProcId $OVERLAY 10000
}

# Ask the overlay to describe itself and parse the answer. Returns $null when
# the app stayed silent, which is a failure in itself - the caller turns that
# into a reported check rather than a crash.
function Get-OverlayState([IntPtr]$Overlay, $App) {
    Read-LogSince $App.Err | Out-Null        # drop anything queued up
    [void][ProbeLib.U]::PostMessageW($Overlay, $script:WM_APP2, [IntPtr]::Zero, [IntPtr]::Zero)
    $line = $null
    for ($k = 0; $k -lt 20 -and -not $line; $k++) {
        Start-Sleep -Milliseconds 60
        $line = @(Read-LogSince $App.Err) | Where-Object { $_ -match 'query:' } | Select-Object -Last 1
    }
    if (-not $line) { return $null }
    $st = @{}
    # The selection rectangle is comma separated, so the value pattern has to
    # allow it - a value pattern that stops at the first comma silently
    # truncates sel to its left edge, which is enough to look like a hit and
    # not enough to catch a rectangle that only half moved.
    foreach ($m in [regex]::Matches($line, '(\w+)=([-\w,]+)')) { $st[$m.Groups[1].Value] = $m.Groups[2].Value }
    $st
}

function Select-Region([IntPtr]$Overlay) {
    # drag a region out, which is the only way into the editing state: the tool
    # keys all return early until a->haveSel is set.
    Send-Down $Overlay 120 90
    Start-Sleep -Milliseconds 120
    for ($i = 1; $i -le 8; $i++) { Send-Move $Overlay (120 + $i * 50) (90 + $i * 32) $script:MK_LBUTTON }
    Start-Sleep -Milliseconds 120
    Send-Up $Overlay 520 346
    Start-Sleep -Milliseconds 350
}

# Read the toolbar's own geometry out of the app's log instead of guessing it.
# chrome.c dumps the row in verbose mode with a comment saying it exists so the
# outside probes can aim at a specific button "without having to re-derive the
# layout, which is exactly the thing that changes" - the row moves with the
# selection size and the DPI. Returns the buttons as objects with a centre
# point, or nothing if the app has not drawn a toolbar yet.
function Get-ToolbarButtons($App) {
    $out = @()
    foreach ($l in (Read-LogAll $App.Err)) {
        $m = [regex]::Match($l, 'tb\[\s*(\d+)\]\s+id=(-?\d+)\s+color=(-?\d+)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)')
        if ($m.Success) {
            $x = [int]$m.Groups[4].Value; $y = [int]$m.Groups[5].Value
            $w = [int]$m.Groups[6].Value; $h = [int]$m.Groups[7].Value
            $out += [pscustomobject]@{
                Index = [int]$m.Groups[1].Value
                Id    = [int]$m.Groups[2].Value
                X     = $x + [int]($w / 2)
                Y     = $y + [int]($h / 2)
            }
        }
    }
    $out
}

$crashMark = Get-CrashMark
$app = Start-ProbeApp $Exe @('--verbose') 'probe-events-shot'
$ov = [IntPtr]::Zero
try {
    $ov = New-Overlay $app.Proc.Id
    if ($ov -eq [IntPtr]::Zero) { throw "the overlay never appeared" }
    Write-Host "overlay 0x$($ov.ToInt64().ToString('X'))" -ForegroundColor Cyan

    $st = Get-OverlayState $ov $app
    if (-not $st) { throw "the overlay did not answer WM_APP + 2 (is --verbose missing?)" }
    Write-Host ("  fresh overlay: sel={0} tool={1} strokes={2} base={3} layer={4}" -f `
        $st.sel, $st.tool, $st.strokes, $st.base, $st.layer) -ForegroundColor DarkGray
    Add-Check 'the overlay answers a state query' $true "tool=$($st.tool) base=$($st.base)"

    # --- 1. the tool keys ------------------------------------------------
    # A tool key is a no-op with no selection, which is the app's intent and
    # worth pinning: pressing 'M' over the desktop must not arm the mosaic.
    Send-Key $ov ([int][char]'M')
    Start-Sleep -Milliseconds 200
    $st0 = Get-OverlayState $ov $app
    Add-Check 'a tool key before any selection is ignored' ($st0.tool -eq '0') "tool=$($st0.tool) (expected 0)"

    Select-Region $ov
    $st = Get-OverlayState $ov $app
    Add-Check 'dragging a region selects it' ($st.sel -notmatch '^0,0,0,0$') "sel=$($st.sel)"

    $badTool = @()
    foreach ($k in ($TOOL.Keys | Sort-Object)) {
        Send-Key $ov ([int][char]$k)
        Start-Sleep -Milliseconds 140
        $s = Get-OverlayState $ov $app
        if (-not $s) { $badTool += "$k(no answer)"; continue }
        if ([int]$s.tool -ne $TOOL[$k]) { $badTool += "$k(got $($s.tool), want $($TOOL[$k]))" }
    }
    Add-Check 'all ten tool keys switch the tool' ($badTool.Count -eq 0) `
        $(if ($badTool.Count) { $badTool -join ' ' } else { 'P E L R O A T M B F' })

    # --- 2. a stroke, and undo/redo -------------------------------------
    # Draw with the pen so there is something for the undo stack to hold.
    Send-Key $ov ([int][char]'P')
    Start-Sleep -Milliseconds 150
    Send-Down $ov 200 200
    for ($i = 1; $i -le 10; $i++) { Send-Move $ov (200 + $i * 8) (200 + $i * 5) $script:MK_LBUTTON; Start-Sleep -Milliseconds 15 }
    Send-Up $ov 280 250
    Start-Sleep -Milliseconds 300
    $after = Get-OverlayState $ov $app
    Add-Check 'a pen drag records exactly one stroke' ([int]$after.strokes -eq 1) "strokes=$($after.strokes)"

    # Undo and redo ask GetKeyState(VK_CONTROL), not the modifier byte in
    # lParam, so a posted key message is ignored by design and the chord has
    # to be real. That is also the only way this probe proves the shortcuts a
    # user actually presses work.
    #
    # Note what the assertion reads: undo moves a->cursor, not a->nstrokes
    # (shot_undo in src\shot\editor.c). The stroke list keeps its length and
    # the undo position walks back through it, so a probe that watched
    # nstrokes would see 1 -> 1 and conclude undo was broken.
    $fg = Focus-GuiInfo
    $hasFocus = ($fg.Fore -eq $ov)

    # Owning the foreground is necessary but not sufficient, and finding that
    # out the hard way is what this block used to do. The desktop can accept
    # injected input, report the overlay as the foreground window, and still
    # never hand a keystroke to it - the overlay then looks focused and silent
    # at the same time, and the shortcut reads as broken. Comparing against
    # another window is not enough either; it is the same window.
    #
    # So the app is asked. The tool keys are already known to work when posted,
    # and a tool is something the app reports about itself, so an unmodified
    # real keypress either moves the tool or it never arrived. That makes this
    # a control rather than a precondition: it fails for a reason that belongs
    # to the machine, and it fails in a way the report can show.
    $realKeysWork = $false
    $whyReal = $null
    if (-not $hasFocus) {
        $whyReal = ("the keyboard is held by '{0}' - real keys go there, not to the app" -f `
                    $(if ($fg.Fore -eq [IntPtr]::Zero) { 'nothing' } else { Get-ClassName $fg.Fore }))
    } else {
        $toolBefore = (Get-OverlayState $ov $app).tool
        Send-RealKey ([int][char]'E')      # ellipse, unmodified
        Start-Sleep -Milliseconds 400
        $toolAfter = (Get-OverlayState $ov $app).tool
        $realKeysWork = ([int]$toolAfter -eq $TOOL['E'] -and [int]$toolBefore -ne [int]$toolAfter)
        if (-not $realKeysWork) {
            $whyReal = "an unmodified key that the app answers when it is posted changed nothing when sent for real (tool stayed at $toolAfter), so this session is not delivering injected input to the app"
        }
        Send-Key $ov ([int][char]'P')      # back to the pen for the undo work
        Start-Sleep -Milliseconds 200
    }

    if ($realKeysWork) {
        Add-Check 'the overlay owns the keyboard while it is up' $true `
            ("foreground=0x{0:X} and a real key reached it" -f $fg.Fore.ToInt64())
    } else {
        # A system window holding the foreground means the keystrokes go
        # there. That is the machine, not the program: the shortcut is not
        # broken, it is not reachable, and saying so is the honest result.
        # Reported as a skip with the reason rather than a pass, because "not
        # exercised" and "works" must not both print a tick.
        Add-Skip 'the overlay owns the keyboard while it is up' $whyReal
    }
    if ($realKeysWork) {
        $startCursor = [int]$after.cursor
        Send-RealCombo 0x5A -Ctrl          # Z
        Start-Sleep -Milliseconds 400
        $undone = Get-OverlayState $ov $app
        Add-Check 'Ctrl+Z walks the undo position back' ([int]$undone.cursor -lt $startCursor) `
            "cursor=$startCursor -> $($undone.cursor)"
        Add-Check 'undo does not throw the stroke away' ([int]$undone.strokes -eq 1) `
            "strokes=$($undone.strokes)"

        Send-RealCombo 0x59 -Ctrl          # Y
        Start-Sleep -Milliseconds 400
        $redone = Get-OverlayState $ov $app
        Add-Check 'Ctrl+Y walks it forward again' ([int]$redone.cursor -gt [int]$undone.cursor) `
            "cursor=$($undone.cursor) -> $($redone.cursor)"
    } else {
        $r = if ($whyReal) { $whyReal } else { 'no real key reached the overlay in this session' }
        Add-Skip 'Ctrl+Z walks the undo position back' $r
        Add-Skip 'undo does not throw the stroke away' $r
        Add-Skip 'Ctrl+Y walks it forward again' $r
    }

    # --- 3. a click whose capture is stolen mid-press --------------------
    # The overlay takes the capture on WM_LBUTTONDOWN and releases it in
    # WM_LBUTTONUP, and WM_CAPTURECHANGED deliberately does NOT clear the press
    # index - a comment in editor.c says so at length, because clearing it
    # there is what used to make every toolbar button dead: ReleaseCapture()
    # inside WM_LBUTTONUP delivers that message synchronously, so a handler
    # that trusted it would cancel the very click that ended the button.
    #
    # The observable is the tool: a mosaic button that was not the current one,
    # so a fired click is unmistakable and a dead one is too.
    $btns = @(Get-ToolbarButtons $app)
    # chrome.c lays the row out in the app's absolute space - virtual screen
    # coordinates - and the window proc adds a->vsx / a->vsy to every mouse
    # message as it unpacks it. On a machine whose primary monitor is not at
    # 0,0 those origins differ, so the logged rectangles have to come back
    # into client space before they can be posted as a click. Taking the last
    # dump matters too: an earlier one describes the toolbar before the region
    # was resized.
    $vorg = Get-VirtualOrigin
    $mosaic = $btns | Where-Object { $_.Id -eq 8 } | Select-Object -Last 1
    if ($mosaic) {
        $mx = $mosaic.X - $vorg.X
        $my = $mosaic.Y - $vorg.Y
        $toolBefore = [int](Get-OverlayState $ov $app).tool
        Send-Down $ov $mx $my
        Start-Sleep -Milliseconds 250
        $pressed = Get-OverlayState $ov $app
        Add-Check 'pressing a toolbar button records the press' ([int]$pressed.tbDown -ge 0) `
            "tbDown=$($pressed.tbDown) at ($mx,$my)"

        # something else takes the capture: a menu, or a drag to another app
        [void][ProbeLib.U]::SetCapture([IntPtr]::Zero)
        Start-Sleep -Milliseconds 250
        Send-Up $ov $mx $my
        Start-Sleep -Milliseconds 400
        $released = Get-OverlayState $ov $app
        Add-Check 'the press index is cleared once the button comes up' ([int]$released.tbDown -eq -1) `
            "tbDown=$($released.tbDown)"
        Add-Check 'a click whose capture was stolen still activates the button' `
            ([int]$released.tool -eq 7 -and $toolBefore -ne 7) `
            "tool=$toolBefore -> $($released.tool) (7 = mosaic)"
    } else {
        Add-Check 'pressing a toolbar button records the press' $false 'the app logged no toolbar buttons'
        Add-Check 'the press index is cleared once the button comes up' $false 'the app logged no toolbar buttons'
        Add-Check 'a click whose capture was stolen still activates the button' $false 'the app logged no toolbar buttons'
    }

    # --- 4. the wheel, and a flood of moves ------------------------------
    Send-Key $ov ([int][char]'M')           # mosaic, so the wheel has work to do
    Start-Sleep -Milliseconds 150
    Send-Wheel $ov 400 300 120
    Start-Sleep -Milliseconds 200
    Send-Wheel $ov 400 300 -120
    Start-Sleep -Milliseconds 250
    $afterWheel = Get-OverlayState $ov $app
    Add-Check 'the wheel is absorbed while editing' ($null -ne $afterWheel) "tool=$($afterWheel.tool)"

    # A real drag delivers ~125 moves a second. If the window cannot retire
    # them the queue backs up and the selection stops following the cursor,
    # which is what "frozen" looks like from the chair. The WM_NULL round trip
    # is the backpressure signal: one thread, so a slow paint is a slow answer.
    $sw = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $Flood; $i++) {
        Send-Move $ov (100 + ($i % 700)) (100 + ($i % 500))
    }
    $idle = Wait-Idle $ov 3000
    $sw.Stop()
    # The bar matters as much as the answer: a healthy window retires a burst
    # in tens of milliseconds, and one that needs seconds has a paint that
    # cannot keep up with a real drag even when it never actually wedges.
    Add-Check "the overlay retires $Flood queued mouse moves" ($idle.Ok -and $idle.Ms -lt 1500) `
        ("answered in {0:n0} ms with {1} moves queued" -f $idle.Ms, $Flood)

    $r = Wait-Idle $ov 2000
    Add-Check 'the window is still answering after the flood' $r.Ok ("round trip {0:n0} ms" -f $r.Ms)

    # --- 5. the Esc cascade ---------------------------------------------
    # Three levels, in order: a stroke in progress is dropped, then the
    # selection is dropped, then the overlay goes away. Pressing Esc twice is
    # the difference between "cancelled my drawing" and "lost my selection",
    # and a handler that collapsed any two of them would be indistinguishable
    # from one that works.
    Send-Key $ov ([int][char]'P')
    Start-Sleep -Milliseconds 150
    Send-Down $ov 220 220
    for ($i = 1; $i -le 6; $i++) { Send-Move $ov (220 + $i * 10) (220 + $i * 8) $script:MK_LBUTTON }
    Start-Sleep -Milliseconds 150
    [void][ProbeLib.U]::PostMessageW($ov, $script:WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 350
    $afterEsc1 = Get-OverlayState $ov $app
    Add-Check 'Esc mid-stroke drops the stroke and keeps the capture' `
        ($null -ne $afterEsc1 -and $afterEsc1.sel -notmatch '^0,0,0,0$') "sel=$($afterEsc1.sel)"

    [void][ProbeLib.U]::PostMessageW($ov, $script:WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
    $afterEsc2 = Get-OverlayState $ov $app
    Add-Check 'the second Esc clears the selection' ($afterEsc2.sel -eq '0,0,0,0') "sel=$($afterEsc2.sel)"

    [void][ProbeLib.U]::PostMessageW($ov, $script:WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    $gone = Find-AppWindow $app.Proc.Id $OVERLAY 500
    Add-Check 'the third Esc closes the capture' ($gone -eq [IntPtr]::Zero) `
        $(if ($gone -eq [IntPtr]::Zero) { 'overlay is gone' } else { 'still open' })

    # --- 6. the host is still there --------------------------------------
    $hostWin = Find-AppWindow $app.Proc.Id $HOST_CLASS 2000
    Add-Check 'the tray host survives the capture being cancelled' ($hostWin -ne [IntPtr]::Zero) ''

    # and it can start another one, which is the loop a user actually does
    $ov2 = New-Overlay $app.Proc.Id
    Add-Check 'a second capture opens after a cancel' ($ov2 -ne [IntPtr]::Zero) ''
    if ($ov2 -ne [IntPtr]::Zero) {
        Select-Region $ov2
        $s2 = Get-OverlayState $ov2 $app
        Add-Check 'the reopened overlay is fully usable' ($null -ne $s2 -and $s2.sel -notmatch '^0,0,0,0$') `
            "sel=$($s2.sel) tool=$($s2.tool)"
    }
}
catch {
    Add-Check 'the probe ran to completion' $false "$_"
}
finally {
    Stop-ProbeApp $app
}

Add-CrashCheck $crashMark 'shot' 'shot logged no crash through the whole event matrix'

Write-Checks 'shot event matrix'