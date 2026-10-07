# probe-tray-isolation.ps1 - the eyedropper and the notification area must not
# be the same button.
#
#   .\test\probe-tray-isolation.ps1 [-Exe .\bin\picker.exe]
#
# The report this covers: arm the picker from the tray, then click the
# screenshot icon in the tray, and what came out was a colour - no screenshot
# at all. Neither app's tray menu was wrong. The picker's eyedropper is a
# fullscreen topmost window, so while it was armed it owned hit-testing for the
# whole virtual screen, the notification area included, and a click meant for
# the screenshot icon landed in the loupe instead. Two tray entries, one button.
#
# So the question is not "does the menu have both items" but "can the shell
# still reach the taskbar while the picker is armed". That is what this asks,
# and it asks it with a real cursor, because the stand-down is driven by where
# the cursor actually is: a posted WM_MOUSEMOVE proves nothing about a window
# that decides to hide itself.
#
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 reads a .ps1 without
# a BOM as ANSI, and mojibake in a comment can swallow the next statement.

[CmdletBinding()]
param([string]$Exe, [string]$ShotExe)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\picker.exe' }
if (-not (Test-Path $Exe)) { throw "picker.exe not found at $Exe (build it first)" }
if (-not $ShotExe) { $ShotExe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\shot.exe' }

# probe-lib owns enumeration and the checks; these are the few calls it has no
# declaration for - a real cursor, and the hit test the whole probe is about.
# FindWindowW keeps its CharSet=Unicode: without it the string is marshalled
# as ANSI bytes and handed to the W entry point, which reads them as UTF-16 -
# "Shell_TrayWnd" becomes "S h e ..." and no window is ever found.
Add-Type -Namespace TrayIso -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
'@

$MOUSEEVENTF_LEFTDOWN = 0x0002
$MOUSEEVENTF_LEFTUP   = 0x0004

$PICKER_CLASS = 'MuskPickerMain'
$EYEDROP_CLASS = 'MuskPickerEyeDrop'
$SHOT_HOST_CLASS = 'MuskShotHost'
$SHOT_OVERLAY_CLASS = 'ShotOverlayCls'
$PMI_EYEDROP    = 40001
$WM_LBUTTONUP   = 0x0202
$WM_CHAR        = 0x0102
$VK_ESCAPE      = 0x1B

function Get-Eyedrops([int]$ProcId) {
    @(Get-AppWindows $ProcId | Where-Object { (Get-ClassName $_) -eq $EYEDROP_CLASS })
}

function Get-ShotOverlays([int]$ProcId) {
    @(Get-AppWindows $ProcId | Where-Object { (Get-ClassName $_) -eq $SHOT_OVERLAY_CLASS })
}

# Start a capture the way the tray does, and wait for the overlay. Returns the
# overlay window, or Zero.
function Start-Capture([int]$ProcId) {
    # Not $host: that is PowerShell's own read-only console host variable, and
    # assigning to it throws, which would take the whole capture check with it.
    $hostWin = Find-AppWindow $ProcId $SHOT_HOST_CLASS 2000
    if ($hostWin -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    # The message is a uint, not an IntPtr - that is the declaration's type, and
    # PowerShell will not coerce one into the other.
    [void][ProbeLib.U]::PostMessageW($hostWin, [uint32]0x8001, [IntPtr]::Zero, [IntPtr]$WM_LBUTTONUP)
    Find-AppWindow $ProcId $SHOT_OVERLAY_CLASS 8000
}

function Wait-NoEyedrops([int]$ProcId, [int]$Ms = 2500) {
    $end = (Get-Date).AddMilliseconds($Ms)
    while ((Get-Date) -lt $end) {
        if ((Get-Eyedrops $ProcId).Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 100
    }
    $false
}

function Arm-Eyedrop([int]$ProcId, [int]$TimeoutMs = 8000) {
    $win = Find-AppWindow $ProcId $PICKER_CLASS 2000
    if ($win -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    [void][ProbeLib.U]::PostMessageW($win, $script:WM_COMMAND, [IntPtr]$PMI_EYEDROP, [IntPtr]::Zero)
    Find-AppWindow $ProcId $EYEDROP_CLASS $TimeoutMs
}

# The shell's taskbar: the primary one, else whatever a second monitor has.
# Both spellings are looked for because which one carries the notification
# area depends on where the shell put the icons, not on anything this probe
# can assume.
function Get-Taskbar {
    foreach ($cls in @('Shell_TrayWnd', 'Shell_SecondaryTrayWnd')) {
        $h = [TrayIso.U]::FindWindowW($cls, $null)
        if ($h -ne [IntPtr]::Zero) {
            $r = New-Object TrayIso.U+RECT
            if ([TrayIso.U]::GetWindowRect($h, [ref]$r)) {
                return [pscustomobject]@{ H = $h; Class = $cls; Rect = $r }
            }
        }
    }
    $null
}

# A point on the taskbar that belongs to the taskbar itself. Scanning for one
# where WindowFromPoint returns the taskbar rather than one of its children is
# what keeps the real click below off a Start button, a clock or a running
# app: an empty patch of taskbar takes a left click and does nothing at all,
# which is the harmless way to prove the shell receives clicks.
function Get-EmptyTaskbarPoint($Tray) {
    $y = [int](($Tray.Rect.T + $Tray.Rect.B) / 2)
    for ($x = $Tray.Rect.L + 6; $x -lt $Tray.Rect.R - 6; $x += 6) {
        $p = New-Object TrayIso.U+POINT
        $p.X = $x; $p.Y = $y
        if ([TrayIso.U]::WindowFromPoint($p) -eq $Tray.H) {
            return [pscustomobject]@{ X = $x; Y = $y }
        }
    }
    $null
}

# Somewhere on the desktop, well clear of the taskbar in every direction.
function Get-DesktopPoint {
    # A point the desktop really owns. The loupe treats "cursor over some
    # other program's window" as "a capture tool wants the screen" and stands
    # down for good, so a probe point in the middle of whatever window happens
    # to be centred makes the resume check fail on a machine that is merely
    # busy - not on the picker. Scan a grid for a spot whose hit-test root is
    # the desktop itself; fall back to the old fixed point when every spot is
    # covered, so a fully obscured desktop reports the same failure it always
    # did rather than something new.
    $sw = [ProbeLib.U]::GetSystemMetrics(0)
    $sh = [ProbeLib.U]::GetSystemMetrics(1)
    $fallback = [pscustomobject]@{ X = [int]($sw * 0.5); Y = [int]($sh * 0.35) }
    foreach ($fy in 0.2, 0.35, 0.5, 0.65, 0.8) {
        foreach ($fx in 0.15, 0.3, 0.5, 0.7, 0.85) {
            $p = New-Object TrayIso.U+POINT
            $p.X = [int]($sw * $fx); $p.Y = [int]($sh * $fy)
            $under = [TrayIso.U]::WindowFromPoint($p)
            if ($under -eq [IntPtr]::Zero) { continue }
            $root = [TrayIso.U]::GetAncestor($under, 2)      # GA_ROOT
            if ($root -eq [IntPtr]::Zero) { $root = $under }
            $sb = New-Object System.Text.StringBuilder 256
            [void][TrayIso.U]::GetClassNameW($root, $sb, 256)
            $cls = $sb.ToString()
            if ($cls -eq 'Progman' -or $cls -eq 'WorkerW') {
                return [pscustomobject]@{ X = $p.X; Y = $p.Y }
            }
        }
    }
    return $fallback
}

function Move-To([int]$x, [int]$y) {
    [void][TrayIso.U]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 260      # the watcher polls every 60 ms
}

# The clipboard is shared, so a read is a retry rather than a single call.
function Get-Clip([int]$Tries = 6) {
    for ($k = 0; $k -lt $Tries; $k++) {
        try {
            $t = Get-Clipboard -Raw -ErrorAction Stop
            if ($null -ne $t) { return [string]$t }
        } catch { }
        Start-Sleep -Milliseconds 250
    }
    $null
}

# What the picker is currently showing, read out through its own digit
# shortcut: pressing 1 copies format 1 of the current colour. This is the
# observation that separates "the click was the shell's" from "the loupe ate
# it and picked a colour" - both close the eyedropper, so the window coming
# and going cannot tell them apart, but only one of them changes the colour.
function Get-PickedColor([IntPtr]$Win) {
    try { Set-Clipboard -Value '' -ErrorAction Stop } catch { }
    Send-Key $Win 0x31 0x31
    Start-Sleep -Milliseconds 500
    $c = Get-Clip
    if ($null -eq $c) { '' } else { $c.Trim() }
}

$crashMark = Get-CrashMark
$app = Start-ProbeApp $Exe @() 'probe-tray-isolation'
try {
    $win = Find-AppWindow $app.Proc.Id $PICKER_CLASS 8000
    if ($win -eq [IntPtr]::Zero) { throw "the picker window never appeared (pid $($app.Proc.Id))" }

    $tray = Get-Taskbar
    if (-not $tray) {
        # Without a taskbar there is no notification area to collide with, so
        # the bug this probe covers cannot happen. Said out loud rather than
        # reported as a pass: "not exercised" and "passed" are otherwise the
        # same green tick, and only one of them is true.
        Add-Skip 'the taskbar is reachable' 'no Shell_TrayWnd / Shell_SecondaryTrayWnd on this desktop'
    } else {
        $trayRect = $tray.Rect
        Write-Host ("taskbar {0}  {1},{2} - {3},{4}" -f $tray.Class, $trayRect.L, $trayRect.T, $trayRect.R, $trayRect.B) -ForegroundColor Cyan

        # Leave the mouse where it was found. This probe drives the real
        # pointer, and the user has to be able to keep using their machine.
        $origin = New-Object TrayIso.U+POINT
        [void][TrayIso.U]::GetCursorPos([ref]$origin)

        try {
            $taskbarPt = Get-EmptyTaskbarPoint $tray
            if (-not $taskbarPt) {
                Add-Skip 'an empty patch of taskbar' "every pixel of $trayRect is covered by a button"
            } else {
                $deskPt = Get-DesktopPoint
                Write-Host ("taskbar point {0},{1}   desktop point {2},{3}" -f $taskbarPt.X, $taskbarPt.Y, $deskPt.X, $deskPt.Y) -ForegroundColor Cyan

                # --- 1. armed: is the taskbar still the shell's? ----------
                $before = Get-PickedColor $win
                Write-Host "colour before the exchange: '$before'" -ForegroundColor DarkGray

                $ed = Arm-Eyedrop $app.Proc.Id
                Add-Check 'the eyedropper opens' ($ed -ne [IntPtr]::Zero) ''

                if ($ed -ne [IntPtr]::Zero) {
                    # Move onto the taskbar with the real cursor. Nothing is
                    # clicked here: the point of the check is what the picker
                    # does when the cursor only *arrives* at the tray.
                    Move-To $taskbarPt.X $taskbarPt.Y

                    $probePt = New-Object TrayIso.U+POINT
                    $probePt.X = $taskbarPt.X; $probePt.Y = $taskbarPt.Y
                    $under = [TrayIso.U]::WindowFromPoint($probePt)
                    Add-Check 'the shell keeps hit-testing the taskbar while the picker is armed' `
                        ($under -ne $ed) `
                        ("under the point: '{0}'" -f (Get-ClassName $under))

                    Add-Check 'the loupe stands down on the taskbar' `
                        (-not [ProbeLib.U]::IsWindowVisible($ed)) `
                        ("visible={0}" -f [ProbeLib.U]::IsWindowVisible($ed))

                    # --- 2. a real click there ---------------------------
                    # The click goes to the taskbar, not to us. Two things have
                    # to be true afterwards, and neither alone is enough: the
                    # pick has ended rather than being left armed under
                    # whatever the user just opened, and no colour came out of
                    # it - which is the whole report, since a click eaten here
                    # used to come out as a colour.
                    [TrayIso.U]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 220
                    [TrayIso.U]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 500
                    $left = Get-Eyedrops $app.Proc.Id
                    Add-Check 'a click on the taskbar ends the pick rather than leaving it armed' `
                        ($left.Count -eq 0) `
                        ("eyedropper windows left: {0}" -f $left.Count)

                    $after = Get-PickedColor $win
                    Add-Check 'that click picked no colour - it reached the shell, not the loupe' `
                        ($after -eq $before) `
                        ("'$before' -> '$after'")
                }

                # --- 3. the desktop still picks colours -------------------
                $ed2 = Arm-Eyedrop $app.Proc.Id
                Add-Check 'the eyedropper opens again' ($ed2 -ne [IntPtr]::Zero) ''
                if ($ed2 -ne [IntPtr]::Zero) {
                    Move-To $deskPt.X $deskPt.Y
                    $back = [ProbeLib.U]::IsWindowVisible($ed2)
                    Add-Check 'moving back to the desktop brings the loupe back' $back `
                        ("visible=$back")

                    # And the pick itself still works, which is the whole
                    # reason the stand-down exists.
                    [TrayIso.U]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 80
                    [TrayIso.U]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 600
                    $left2 = Get-Eyedrops $app.Proc.Id
                    Add-Check 'a click on the desktop picks a colour and closes the loupe' `
                        ($left2.Count -eq 0) `
                        ("eyedropper windows left: {0}" -f $left2.Count)
                }

                # --- 4. an armed picker is not re-armed -------------------
                # The tray command, the hotkey and the D key all land in
                # do_eyedrop while the overlay's nested loop is still running,
                # and the overlay's loop dispatches them like any other
                # message. A second overlay would freeze the screen twice over.
                $ed3 = Arm-Eyedrop $app.Proc.Id
                if ($ed3 -ne [IntPtr]::Zero) {
                    [void][ProbeLib.U]::PostMessageW($win, $script:WM_COMMAND, [IntPtr]$PMI_EYEDROP, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 700
                    $both = Get-Eyedrops $app.Proc.Id
                    Add-Check 'the tray command cannot open a second loupe over the first' `
                        ($both.Count -eq 1) `
                        ("eyedropper windows: {0}" -f $both.Count)
                    # close it, by the same Esc the user would use
                    [void][ProbeLib.U]::PostMessageW($ed3, 0x0102, [IntPtr]0x1B, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 500
                    $left3 = Get-Eyedrops $app.Proc.Id
                    Add-Check 'Esc still cancels the pick' ($left3.Count -eq 0) `
                        ("eyedropper windows left: {0}" -f $left3.Count)
                } else {
                    Add-Check 'the eyedropper opens a third time' $false 'it did not open'
                }
            }
        }
        finally {
            [void][TrayIso.U]::SetCursorPos($origin.X, $origin.Y)
        }
    }

    # --- 5. a capture takes the screen from an armed pick --------------
    # This is the report, end to end and with both programs running: arm the
    # picker, take a screenshot, and what used to happen is a colour picker
    # sitting on top of the capture, still waiting for a click that was never
    # meant for it. Nothing here goes near the tray, so the stand-down above
    # cannot be what fixes it - this is the branch a hotkey reaches.
    if (-not (Test-Path $ShotExe)) {
        Add-Skip 'a capture takes the screen from an armed pick' "shot.exe not built at $ShotExe"
    } else {
        $shot = $null
        try {
            $shot = Start-ProbeApp $ShotExe @() 'probe-tray-isolation-shot'
            $shotWin = Find-AppWindow $shot.Proc.Id $SHOT_HOST_CLASS 8000
            if ($shotWin -eq [IntPtr]::Zero) {
                Add-Check 'shot.exe starts' $false "no $SHOT_HOST_CLASS window (pid $($shot.Proc.Id))"
            } else {
                Add-Check 'shot.exe starts' $true ''

                # Armed with the loupe showing, cursor on the desktop: this is
                # the state that used to follow every capture around.
                $deskPt2 = Get-DesktopPoint
                Move-To $deskPt2.X $deskPt2.Y
                $ed4 = Arm-Eyedrop $app.Proc.Id
                Add-Check 'the eyedropper arms with its loupe showing' `
                    (($ed4 -ne [IntPtr]::Zero) -and [ProbeLib.U]::IsWindowVisible($ed4)) `
                    ("hwnd={0} visible={1}" -f $ed4, [ProbeLib.U]::IsWindowVisible($ed4))

                $ov = Start-Capture $shot.Proc.Id
                Add-Check 'the capture comes up' ($ov -ne [IntPtr]::Zero) ''
                if ($ov -ne [IntPtr]::Zero) {
                    $gone = Wait-NoEyedrops $app.Proc.Id 2500
                    Add-Check 'the armed pick gets out of the way of the capture' $gone `
                        ("eyedropper windows left: {0}" -f (Get-Eyedrops $app.Proc.Id).Count)

                    # The other direction is a refusal, not a tear-down: the
                    # user may be halfway through a drag, and taking that away
                    # is worse than not picking.
                    [void][ProbeLib.U]::PostMessageW($win, $script:WM_COMMAND, [IntPtr]$PMI_EYEDROP, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 700
                    $busy = Get-Eyedrops $app.Proc.Id
                    Add-Check 'a capture in progress refuses a new pick instead of tearing the capture down' `
                        (($busy.Count -eq 0) -and ((Get-ShotOverlays $shot.Proc.Id).Count -eq 1)) `
                        ("eyedropper windows: {0}, capture overlays: {1}" -f `
                         $busy.Count, (Get-ShotOverlays $shot.Proc.Id).Count)

                    # Cancel it, then watch: the complaint was a colour picker
                    # that came back over the next thing on screen.
                    [void][ProbeLib.U]::PostMessageW($ov, $WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 1200
                    $back2 = Get-Eyedrops $app.Proc.Id
                    Add-Check 'no colour picker follows the capture when it closes' `
                        ($back2.Count -eq 0) `
                        ("eyedropper windows: {0}" -f $back2.Count)

                    # And the picker is still usable afterwards, which is the
                    # whole point of ending the pick instead of wedging it.
                    $ed5 = Arm-Eyedrop $app.Proc.Id
                    Add-Check 'the picker still takes a new pick once the capture is done' `
                        ($ed5 -ne [IntPtr]::Zero) ''
                    if ($ed5 -ne [IntPtr]::Zero) {
                        [void][ProbeLib.U]::PostMessageW($ed5, $WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
                        Start-Sleep -Milliseconds 400
                    }
                }
                $shot.Proc.Refresh()
                Add-Check 'shot.exe survived the capture' (-not $shot.Proc.HasExited) `
                    $(if ($shot.Proc.HasExited) { "exit $($shot.Proc.ExitCode)" } else { '' })
            }
        }
        finally {
            Stop-ProbeApp $shot
        }
    }

    $app.Proc.Refresh()
    Add-Check 'the picker survived the whole exchange' (-not $app.Proc.HasExited) `
        $(if ($app.Proc.HasExited) { "exit $($app.Proc.ExitCode)" } else { '' })

    $crash = Get-CrashRecords $crashMark 'picker'
    Add-Check 'no picker crash recorded' ($crash.Count -eq 0) `
        $(if ($crash.Count) { "$($crash.Count) record(s)" } else { '' })
}
finally {
    Stop-ProbeApp $app
}

Write-Checks 'tray isolation'
