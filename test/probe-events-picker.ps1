# probe-events-picker.ps1 - the picker's event surface, driven from outside.
#
#   .\test\probe-events-picker.ps1 [-Exe .\bin\picker.exe]
#
# probe-picker-wheel covers the wheel, the harmony triangle and the palette.
# What nothing covered was the rest of the window: the keyboard shortcuts, the
# close-to-tray cycle, resizing, and the tray commands - the parts a user
# touches between picking a colour and leaving.
#
# The chain this probe follows end to end is the one that matters most: press a
# digit, and the formatted colour for that digit has to arrive on the
# clipboard. That is a keyboard event, a formatting decision and a clipboard
# write in one observable, so a break anywhere in it shows up as a wrong
# clipboard rather than as "something did not happen".
#
# ASCII only (see probe-focus.ps1).

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\picker.exe' }
if (-not (Test-Path $Exe)) { throw "picker.exe not found at $Exe (build it first)" }

# PowerShell variable names are case-insensitive, so a class constant called
# $MAIN and a window handle called $main are one variable - and the handle is
# initialised to zero a line or two before the lookup. That silently turns the
# search into "find a window whose class is 0", which never matches, so the
# probe reports the program as missing when it is running in plain sight. The
# constants below are suffixed _CLASS and the handles they are looked up
# through are named $win, so no two of them can ever be the same name.
$PICKER_CLASS = 'MuskPickerMain'
$EYEDROP_CLASS = 'MuskPickerEyeDrop'
$PMI_EYEDROP = 40001
$PMI_SHOW    = 40002

# The clipboard is shared and another process can hold it open, so every read
# is a retry rather than a single call. A read that never succeeds is reported
# as a failure of the chain, not skipped - a shortcut that cannot be observed
# is not a shortcut that works.
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

function Clear-Clip {
    for ($k = 0; $k -lt 6; $k++) {
        try { Set-Clipboard -Value '' -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 200 }
    }
}

# Open the eyedropper the way the tray menu and the D key both do, hover a
# pixel and click it. The eyedrop takes whatever it last sampled, so the move
# has to land before the click. Returns whatever is still open afterwards, so
# the caller can tell a closed eyedropper from one that ignored the click.
function Pick-Color([int]$ProcId) {
    $win = Find-AppWindow $ProcId $PICKER_CLASS 2000
    if ($win -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    [void][ProbeLib.U]::PostMessageW($win, $script:WM_COMMAND, [IntPtr]$PMI_EYEDROP, [IntPtr]::Zero)
    $ed = Find-AppWindow $ProcId $EYEDROP_CLASS 8000
    if ($ed -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    Start-Sleep -Milliseconds 600
    Send-Move $ed 400 300
    Start-Sleep -Milliseconds 400
    Send-Down $ed 400 300
    Start-Sleep -Milliseconds 800
    Find-AppWindow $ProcId $EYEDROP_CLASS 500
}

$crashMark = Get-CrashMark
$app = Start-ProbeApp $Exe @() 'probe-events-picker'
$win = [IntPtr]::Zero
try {
    $win = Find-AppWindow $app.Proc.Id $PICKER_CLASS 8000
    if ($win -eq [IntPtr]::Zero) {
        # Say what was there, so a failure to find the window is a report and
        # not a shrug. A probe that throws "not found" leaves the reader
        # guessing between "the program did not start", "it started and
        # crashed", and "it started under a different class name".
        $app.Proc.Refresh()
        $seen = @(Get-AppWindows $app.Proc.Id | ForEach-Object { "'" + (Get-ClassName $_) + "'" })
        throw ("the main window never appeared (pid {0}, exited={1}, exit={2}, windows seen: {3})" -f `
               $app.Proc.Id, $app.Proc.HasExited, `
               $(if ($app.Proc.HasExited) { $app.Proc.ExitCode } else { '-' }), `
               $(if ($seen.Count) { $seen -join ', ' } else { 'none' }))
    }
    Write-Host "main 0x$($win.ToInt64().ToString('X'))" -ForegroundColor Cyan

    # The tray command that shows the window, so the probe does not depend on
    # whether the program happened to start visible.
    if (-not [ProbeLib.U]::IsWindowVisible($win)) {
        [void][ProbeLib.U]::PostMessageW($win, $script:WM_COMMAND, [IntPtr]$PMI_SHOW, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 700
    }
    Add-Check 'the picker window can be shown' ([ProbeLib.U]::IsWindowVisible($win)) ''

    # --- 1. a colour, and the keyboard that copies it -------------------
    $stillOpen = Pick-Color $app.Proc.Id
    Add-Check 'the eyedropper opens and a click in it closes it' `
        ($stillOpen -eq [IntPtr]::Zero) `
        $(if ($stillOpen -eq [IntPtr]::Zero) { 'eyedropper is gone' } else { 'still open' })

    Clear-Clip
    Send-Key $win 0x31 0x31        # '1'
    Start-Sleep -Milliseconds 600
    $c1 = Get-Clip
    Add-Check 'pressing 1 puts that format on the clipboard' ($c1 -and $c1.Trim().Length -gt 0) `
        ("'{0}'" -f (($c1 -as [string]) -replace "`r?`n", ' '))

    Send-Key $win 0x34 0x34        # '4'
    Start-Sleep -Milliseconds 600
    $c4 = Get-Clip
    Add-Check 'pressing 4 puts a different format on the clipboard' `
        ($c4 -and $c4.Trim().Length -gt 0 -and $c4 -ne $c1) `
        ("1='{0}'  4='{1}'" -f (($c1 -as [string]) -replace "`r?`n", ' '), (($c4 -as [string]) -replace "`r?`n", ' '))

    Send-Key $win 0x31 0x31        # '1' again
    Start-Sleep -Milliseconds 600
    $c1b = Get-Clip
    Add-Check 'the same key gives the same result twice' ($c1b -eq $c1) `
        ("'{0}'" -f (($c1b -as [string]) -replace "`r?`n", ' '))

    # every digit is wired to a format, not just the first two
    $noCopy = @()
    foreach ($d in 2, 5, 7, 9) {
        Clear-Clip
        $vk = 0x30 + $d
        Send-Key $win $vk $vk
        Start-Sleep -Milliseconds 500
        $t = Get-Clip
        if (-not $t -or $t.Trim().Length -eq 0) { $noCopy += $d }
    }
    Add-Check 'digits 2, 5, 7 and 9 all reach the clipboard' ($noCopy.Count -eq 0) `
        $(if ($noCopy.Count) { "no copy for: $($noCopy -join ',')" } else { '2 5 7 9' })

    # Ctrl+Shift+C is the "give me everything" shortcut. The app reads it as
    # ctrl AND shift on the C inside WM_CHAR, so the chord has to be real - a
    # posted key message carries no key state and the shortcut would look dead.
    #
    # Real input is the one thing a probe cannot make happen, so before calling
    # this shortcut broken, the probe checks whether the session is delivering
    # real input at all. It can ask the app itself: digit 3 is already known to
    # copy a format when posted, so the same digit sent for real is a control.
    # A control that fails means the keys are going somewhere else - the desktop
    # can sit behind a shell-hosted surface that accepts injected input and
    # never delivers it, while both SendInput and keybd_event cheerfully report
    # success - and a shortcut that cannot be reached must be reported as
    # unreached, not as broken. Skipping needs the evidence, so the control is
    # the evidence.
    $realKeysWork = $false
    $whyReal = $null
    Clear-Clip
    $g = Activate-Window $win
    if ($g.Fore -ne $win -or $g.Focus -ne $win) {
        $whyReal = "the app does not hold the keyboard - foreground is $(if ($g.Fore -eq [IntPtr]::Zero) { 'nothing at all' } else { Get-ClassName $g.Fore })"
    } else {
        Start-Sleep -Milliseconds 250
        Send-RealKey 0x33        # '3', unmodified
        Start-Sleep -Milliseconds 600
        $ctrl = Get-Clip
        $realKeysWork = ($ctrl -and $ctrl.Trim().Length -gt 0)
        if (-not $realKeysWork) {
            $whyReal = 'an unmodified key that the app answers when it is posted produced nothing when it was sent for real, so this session is not delivering injected input to the app'
        }
    }

    if ($realKeysWork) {
        Clear-Clip
        Send-RealCombo 0x43 -Ctrl -Shift
        Start-Sleep -Milliseconds 700
        $all = Get-Clip
        Add-Check 'Ctrl+Shift+C copies every format at once' `
            ($all -and (($all -as [string]).Length -gt ($c1 -as [string]).Length)) `
            ("{0} chars, against {1} for a single format" -f ($all -as [string]).Length, ($c1 -as [string]).Length)
    } else {
        Add-Skip 'Ctrl+Shift+C copies every format at once' $whyReal
    }

    # --- 2. the navigation keys -----------------------------------------
    # None of these change anything a probe can read back, so the check is the
    # one that still means something: the window keeps answering afterwards.
    $VK = @{ LEFT = 0x25; RIGHT = 0x27; UP = 0x26; DOWN = 0x28; PRIOR = 0x21; NEXT = 0x22;
             HOME = 0x24; END = 0x23; ADD = 0x6B; SUBTRACT = 0x6D; OEM_PLUS = 0xBB; OEM_MINUS = 0xBD }
    foreach ($name in $VK.Keys) {
        [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK[$name], [IntPtr]::Zero)
        Start-Sleep -Milliseconds 60
    }
    $idle = Wait-Idle $win 2000
    Add-Check 'every navigation key leaves the window answering' $idle.Ok `
        ("round trip {0:n0} ms after {1} keys" -f $idle.Ms, $VK.Count)

    # --- 3. resizing ----------------------------------------------------
    # The window is a fixed-layout panel, so resizing is where a paint that
    # assumes its own geometry falls over.
    #
    # The original *window* rect is what gets restored, not the client size:
    # MoveWindow takes the frame and border into account, so handing it the
    # client width back would land a frame's worth short and leave the probe
    # reporting a window it had itself changed.
    $origWin = Get-WinRect $win
    $before = Get-ClientSize $win
    $badSize = @()
    foreach ($s in @(@(320, 240), @(900, 700), @(60, 60))) {
        [void][ProbeLib.U]::MoveWindow($win, 60, 60, $s[0], $s[1], $true)
        Start-Sleep -Milliseconds 400
        Pump-Repaint $win 2
        $r = Wait-Idle $win 2000
        if (-not $r.Ok) { $badSize += ("{0}x{1}" -f $s[0], $s[1]) }
    }
    [void][ProbeLib.U]::MoveWindow($win, $origWin.Left, $origWin.Top,
            ($origWin.Right - $origWin.Left), ($origWin.Bottom - $origWin.Top), $true)
    Start-Sleep -Milliseconds 500
    Add-Check 'the window survives a resize down to 60x60 and back' ($badSize.Count -eq 0) `
        $(if ($badSize.Count) { "hung at: $($badSize -join ', ')" } else { '320x240, 900x700, 60x60, restored' })
    $now = Get-ClientSize $win
    Add-Check 'the window is back to its original size' `
        ($now.W -eq $before.W -and $now.H -eq $before.H) `
        ("now {0}x{1}, started {2}x{3}" -f $now.W, $now.H, $before.W, $before.H)

    # --- 4. close to the tray, and back ---------------------------------
    # WM_CLOSE here hides the window rather than quitting: the program lives on
    # in the tray, and a user who clicks the tray icon expects it back.
    [void][ProbeLib.U]::PostMessageW($win, $script:WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    $hidden = -not [ProbeLib.U]::IsWindowVisible($win)
    Add-Check 'closing the window hides it instead of quitting' $hidden `
        $(if ($hidden) { 'hidden, process still up' } else { 'still visible' })
    Add-Check 'the process survives being closed' (-not $app.Proc.HasExited) ''

    [void][ProbeLib.U]::PostMessageW($win, $script:WM_COMMAND, [IntPtr]$PMI_SHOW, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    Add-Check 'the tray command brings it back' ([ProbeLib.U]::IsWindowVisible($win)) ''

    # --- 5. still alive, still painting ---------------------------------
    Pump-Repaint $win 2
    $r = Wait-Idle $win 2000
    Add-Check 'the window is still answering at the end' $r.Ok ("round trip {0:n0} ms" -f $r.Ms)
    Clear-Clip
}
catch {
    Add-Check 'the probe ran to completion' $false "$_"
}
finally {
    Stop-ProbeApp $app
}

Add-CrashCheck $crashMark 'picker' 'picker logged no crash through the whole event matrix'

Write-Checks 'picker event matrix'
