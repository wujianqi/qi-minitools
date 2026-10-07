# probe-events-slice.ps1 - the slicer's event surface, driven from outside.
#
#   .\test\probe-events-slice.ps1 [-Exe .\bin\slice.exe]
#
# probe-slice drives the command line and the file it cuts; probe-slice-ui
# drives the window buttons and the file dialogs. This one is about the
# keyboard and the window frame: the keys that change the division count, the
# clamp at each end of its range, the focus rule that decides whether Delete
# deletes a selection or a character somebody is typing, the smallest the
# window may be, and whether any of it wedges the message loop.
#
# It never exports and never opens a file dialog: both are covered elsewhere,
# and both would put a modal box between the probe and the thing it is
# measuring.
#
# ASCII only (see probe-focus.ps1).

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\slice.exe' }
if (-not (Test-Path $Exe)) { throw "slice.exe not found at $Exe (build it first)" }

# Ids from the enum at the top of src\slice\slice.c. The probe looks controls
# up by id rather than counting children, so the panel can grow a button
# without making this aim at the wrong one.
$IDC_DIVN   = 114
$IDC_PREFIX = 121
$SL_CLASS = 'MuskSliceMain'
$WM_SETTEXT = 0x000C
$VK_SUBTRACT = 0x6D
$VK_ADD      = 0x6B
$VK_OEM_PLUS  = 0xBB
$VK_OEM_MINUS = 0xBD
$VK_DELETE   = 0x2E
$VK_BACK     = 0x08
$VK_ESCAPE   = 0x1B

function Get-Kid([IntPtr]$parent, [int]$id) {
    $c = @(Get-ChildMap $parent) | Where-Object { $_.Id -eq $id } | Select-Object -First 1
    if ($c) { return $c.H }
    [IntPtr]::Zero
}

$crashMark = Get-CrashMark
$app = Start-ProbeApp $Exe @() 'probe-events-slice'
$win = [IntPtr]::Zero
try {
    $win = Find-AppWindow $app.Proc.Id $SL_CLASS 8000
    if ($win -eq [IntPtr]::Zero) { throw "the main window never appeared" }
    [void][ProbeLib.U]::SetForegroundWindow($win)
    Start-Sleep -Milliseconds 800
    Write-Host "main 0x$($win.ToInt64().ToString('X'))" -ForegroundColor Cyan

    $kids   = @(Get-ChildMap $win)
    $divN   = Get-Kid $win $IDC_DIVN
    $prefix = Get-Kid $win $IDC_PREFIX
    Add-Check 'the division box and the prefix box are found by id' `
        (($divN -ne [IntPtr]::Zero) -and ($prefix -ne [IntPtr]::Zero)) `
        ("{0} children, IDC_DIVN=0x{1:X}, IDC_PREFIX=0x{2:X}" -f $kids.Count, $divN.ToInt64(), $prefix.ToInt64())

    # --- 1. how small the window may get --------------------------------
    $min = Get-MinTrackSize $win
    Add-Check 'the window declares a minimum size' ($null -ne $min) `
        $(if ($min) { "$($min.W)x$($min.H)" } else { 'WM_GETMINMAXINFO left it at zero' })
    if ($min) {
        Add-Check 'the minimum is at least the 560x520 the panel needs' `
            ($min.W -ge 560 -and $min.H -ge 520) "reported $($min.W)x$($min.H)"
    }

    # --- 2. the division count, and the clamp at each end ---------------
    # These keys are the only part of slice a power user touches hundreds of
    # times, and both ends of the range are where an off-by-one shows up as a
    # divide that never stops or a count that goes to zero and divides by it.
    $start = Get-WinText $divN 64
    Add-Check 'the division box starts with a number in it' `
        ($start -match '^\d+$') "'$start'"

    # Raise it first: the count starts at its floor of 2, so asking "does minus
    # lower it" against a starting value of 2 cannot tell a working key from
    # a dead one.
    [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK_ADD, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    $up1 = Get-WinText $divN 64
    Add-Check 'the plus key raises the division count' `
        ($up1 -match '^\d+$' -and [int]$up1 -gt [int]$start) "$start -> $up1"

    [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK_SUBTRACT, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    $down1 = Get-WinText $divN 64
    Add-Check 'the minus key lowers it again' `
        ($down1 -match '^\d+$' -and [int]$down1 -lt [int]$up1) "$up1 -> $down1"

    # the low clamp: the source refuses to go below 2
    for ($i = 0; $i -lt 12; $i++) {
        [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK_SUBTRACT, [IntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 700
    $low = Get-WinText $divN 64
    Add-Check 'holding minus stops at 2 and does not run off the end' `
        ($low -eq '2') "after 12 presses: '$low'"

    # the high clamp: 64
    for ($i = 0; $i -lt 80; $i++) {
        [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK_ADD, [IntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 1200
    $high = Get-WinText $divN 64
    Add-Check 'holding plus stops at 64' ($high -eq '64') "after 80 presses: '$high'"

    # the numpad and the main keyboard row are the same key to a user
    [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK_OEM_MINUS, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    $rowKey = Get-WinText $divN 64
    Add-Check 'the minus key on the main keyboard row works too' `
        ($rowKey -match '^\d+$' -and [int]$rowKey -lt [int]$high) "$high -> $rowKey"

    $idle = Wait-Idle $win 3000
    Add-Check 'the window is still answering after 100 key presses' $idle.Ok `
        ("round trip {0:n0} ms" -f $idle.Ms)

    # --- 3. Delete is about the focus ----------------------------------
    # Delete removes the selected slice, but only when the canvas has the
    # focus - otherwise it has to stay a plain Backspace so a caret in the
    # prefix field can eat a character. A handler that dropped the focus test
    # would delete a slice every time somebody pressed Delete with the cursor
    # in a text box.
    #
    # What can be checked from out here is that half, and it is the half that
    # belongs to this program: with the focus sitting in the prefix box, a
    # Delete must leave both the text and the app's own state alone. The
    # character the user sees disappear is the Edit control's own doing - the
    # keyboard goes straight to the focused control and never reaches the
    # parent's window procedure at all - so that half is not observable by
    # posting a message, and asserting it here would only ever be asserting
    # that the Edit control works.
    [void][ProbeLib.U]::SendMessageW($prefix, $WM_SETTEXT, [IntPtr]0, 'cell')
    Start-Sleep -Milliseconds 300
    $g = Activate-Window $win
    Send-Click $prefix 20 10
    Start-Sleep -Milliseconds 500
    $pid2 = 0
    $tid = [ProbeLib.U]::GetWindowThreadProcessId($win, [ref]$pid2)
    $gti = New-Object ProbeLib.U+GUITHREADINFO
    $gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($gti)
    [void][ProbeLib.U]::GetGUIThreadInfo($tid, [ref]$gti)
    $prefixHasFocus = ($gti.hwndFocus -eq $prefix)
    Add-Check 'the prefix box can take the keyboard focus' $prefixHasFocus `
        ("focus=0x{0:X} '{1}'" -f $gti.hwndFocus.ToInt64(), (Get-ClassName $gti.hwndFocus))

    if ($prefixHasFocus) {
        $divBefore = Get-WinText $divN 64
        [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK_DELETE, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
        Add-Check 'Delete with the focus in a text box leaves the app alone' `
            ((Get-WinText $divN 64) -eq $divBefore -and (Get-WinText $prefix 64) -eq 'cell') `
            ("division '$divBefore', prefix 'cell' unchanged")

        [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr]$VK_BACK, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
        Add-Check 'Backspace with the focus in a text box leaves the app alone too' `
            ((Get-WinText $divN 64) -eq $divBefore -and (Get-WinText $prefix 64) -eq 'cell') `
            ("division '$divBefore', prefix 'cell' unchanged")
    } else {
        Add-Skip 'Delete with the focus in a text box leaves the app alone' `
            'the prefix box could not take the focus in this session'
        Add-Skip 'Backspace with the focus in a text box leaves the app alone too' `
            'the prefix box could not take the focus in this session'
    }

    # a click on the canvas itself must not disturb anything either
    $divBefore2 = Get-WinText $divN 64
    Send-Click $win 900 500
    Start-Sleep -Milliseconds 500
    Add-Check 'clicking the canvas changes neither the count nor the text' `
        ((Get-WinText $divN 64) -eq $divBefore2 -and (Wait-Idle $win 2000).Ok) `
        ("division still '$divBefore2'")

    # --- 4. Escape, and keys with nothing selected ---------------------
    [void][ProbeLib.U]::PostMessageW($win, $WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
    [void][ProbeLib.U]::PostMessageW($win, $script:WM_KEYDOWN, [IntPtr][int][char]'S', [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    Add-Check 'Escape and the divide key leave the window answering' (Wait-Idle $win 2000).Ok ''

    # --- 5. resizing ----------------------------------------------------
    $orig = Get-WinRect $win
    $bad = @()
    foreach ($s in @(@(320, 240), @(1500, 1000), @(80, 80))) {
        [void][ProbeLib.U]::MoveWindow($win, 60, 60, $s[0], $s[1], $true)
        Start-Sleep -Milliseconds 400
        Pump-Repaint $win 2
        if (-not (Wait-Idle $win 2000).Ok) { $bad += ("{0}x{1}" -f $s[0], $s[1]) }
    }
    [void][ProbeLib.U]::MoveWindow($win, $orig.Left, $orig.Top,
            ($orig.Right - $orig.Left), ($orig.Bottom - $orig.Top), $true)
    Start-Sleep -Milliseconds 500
    Add-Check 'the window survives being forced smaller than its own minimum' ($bad.Count -eq 0) `
        $(if ($bad.Count) { "hung at: $($bad -join ', ')" } else { '320x240, 1500x1000, 80x80, restored' })

    # --- 6. closing -----------------------------------------------------
    [void][ProbeLib.U]::PostMessageW($win, $script:WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    $exited = $app.Proc.WaitForExit(6000)
    Add-Check 'closing the window quits the app' $exited `
        $(if ($exited) { "exit code $($app.Proc.ExitCode)" } else { 'still running after 6s' })
    if ($exited) {
        Add-Check 'and it quits cleanly' ($app.Proc.ExitCode -eq 0) "exit code $($app.Proc.ExitCode)"
    }
}
catch {
    Add-Check 'the probe ran to completion' $false "$_"
}
finally {
    Stop-ProbeApp $app
}

Add-CrashCheck $crashMark 'slice' 'slice logged no crash through the whole event matrix'

Write-Checks 'slice event matrix'
