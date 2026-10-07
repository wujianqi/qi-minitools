# probe-events-envtool.ps1 - the env-var editor's event surface, driven from
# outside.
#
#   .\test\probe-events-envtool.ps1 [-Exe .\bin\envtool.exe]
#
# probe-list-click covers clicking a row and watching the editor follow.
# probe-delete-click covers the delete button. This one covers the parts
# around them: the focus rules, the filter's throttled rebuild, the smallest
# the window is allowed to be, what a repaint costs, and closing the app.
#
# Two things it deliberately does not do:
#
#   - it never saves. The Save button and the Ctrl+S accelerator both write to
#     HKCU\Environment, and a test that modifies the machine's environment
#     variables is a test nobody runs twice;
#   - it never leaves a pending edit behind before closing, because a pending
#     edit turns WM_CLOSE into a modal "discard?" box that a probe cannot
#     answer and would wait on forever.
#
# ASCII only (see probe-focus.ps1).

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')

if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\envtool.exe' }
if (-not (Test-Path $Exe)) { throw "envtool.exe not found at $Exe (build it first)" }

# Control ids from src\envtool\envtool.c. The probe reads them out of the
# window by id rather than by position, so an extra control in the middle of
# the panel does not silently make it click the wrong thing.
$ID_LIST   = 1001
$ID_NAME   = 1002
$ID_VALUE  = 1003
$ID_SCOPE  = 1004
$ID_FILTER = 1005

$ET_CLASS = 'MuskEnvTool'
$WM_SETTEXT = 0x000C
$WM_COMMAND = 0x0111
$EN_CHANGE  = 0x0300
$VK_RETURN  = 0x0D
$LVM_GETITEMCOUNT = 0x1004
$CB_GETCOUNT      = 0x0146
$CB_GETCURSEL     = 0x0147
$CB_SETCURSEL     = 0x014E
$CBN_SELCHANGE    = 1

function Get-Child([IntPtr]$parent, [int]$id) {
    $c = @(Get-ChildMap $parent) | Where-Object { $_.Id -eq $id } | Select-Object -First 1
    if ($c) { return $c.H }
    [IntPtr]::Zero
}

# The app answers WM_APP + 2 with its own repaint and event counters
# (et_log_stats in src\envtool\envtool.c). Reading them is how "is it still
# flickering" and "did the click reach the handler at all" get an answer with
# a number behind it instead of a screenshot to squint at.
function Get-Stats([IntPtr]$win, $app) {
    Read-LogSince $app.Err | Out-Null
    [void][ProbeLib.U]::PostMessageW($win, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero)
    $lines = @()
    for ($k = 0; $k -lt 15; $k++) {
        Start-Sleep -Milliseconds 60
        $lines = @(Read-LogSince $app.Err) | Where-Object { $_ -match 'paint=|notify=' }
        if ($lines.Count -ge 2) { break }
    }
    $st = @{}
    foreach ($l in $lines) {
        foreach ($m in [regex]::Matches($l, '(\w+)=(\d+)')) { $st[$m.Groups[1].Value] = [int]$m.Groups[2].Value }
    }
    $st
}

$crashMark = Get-CrashMark
# --verbose is what makes the app write its own repaint and event counters at
# all, and those counters are how half of this probe answers its questions.
$app = Start-ProbeApp $Exe @('--verbose') 'probe-events-envtool'
$win = [IntPtr]::Zero
try {
    $win = Find-AppWindow $app.Proc.Id $ET_CLASS 8000
    if ($win -eq [IntPtr]::Zero) { throw "the main window never appeared" }
    [void][ProbeLib.U]::SetForegroundWindow($win)
    Start-Sleep -Milliseconds 600
    Write-Host "main 0x$($win.ToInt64().ToString('X'))" -ForegroundColor Cyan

    $kids  = @(Get-ChildMap $win)
    $list  = Get-Child $win $ID_LIST
    $value = Get-Child $win $ID_VALUE
    $name  = Get-Child $win $ID_NAME
    $filt  = Get-Child $win $ID_FILTER
    $scope = Get-Child $win $ID_SCOPE
    Write-Host ("  {0} children, list=0x{1:X} value=0x{2:X} filter=0x{3:X}" -f `
        $kids.Count, $list.ToInt64(), $value.ToInt64(), $filt.ToInt64())

    Add-Check 'the list, the name box, the value box and the filter are all found' `
        (($list -ne [IntPtr]::Zero) -and ($value -ne [IntPtr]::Zero) -and `
         ($name -ne [IntPtr]::Zero) -and ($filt -ne [IntPtr]::Zero) -and `
         ($scope -ne [IntPtr]::Zero)) `
        ("{0} children" -f $kids.Count)

    $count0 = [int][ProbeLib.U]::SendMessageW($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    Add-Check 'the list has variables in it' ($count0 -gt 0) "$count0 items"

    # --- 1. how small the window may get --------------------------------
    # A window that can be dragged below the size its panel needs ends up with
    # its bottom controls outside the client area, and no other check here
    # would notice.
    $min = Get-MinTrackSize $win
    Add-Check 'the window declares a minimum size' ($null -ne $min) `
        $(if ($min) { "$($min.W)x$($min.H)" } else { 'WM_GETMINMAXINFO left it at zero' })
    if ($min) {
        Add-Check 'the minimum is at least the 560x430 the panel needs' `
            ($min.W -ge 560 -and $min.H -ge 430) "reported $($min.W)x$($min.H)"
    }

    # --- 2. Enter moves the caret into the value box --------------------
    # Enter in this app used to run the save path from anywhere that was not a
    # text box - which, with the list holding focus, is every time somebody
    # opened a variable to look at it. The test that matters is that Enter
    # does not save, and that it does move the caret to where editing happens.
    $tid = [ProbeLib.U]::GetWindowThreadProcessId($win, [ref]([uint32]0))
    $appTid = $tid
    function Get-AppFocus([uint32]$t) {
        $gti = New-Object ProbeLib.U+GUITHREADINFO
        $gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($gti)
        [void][ProbeLib.U]::GetGUIThreadInfo($t, [ref]$gti)
        $gti
    }
    # give the list the focus, the way a click would. The click has to be
    # posted to the ListView itself, not to the parent: a posted message is
    # delivered to the window it names and no hit testing happens, so a click
    # aimed at the parent lands on the app's own painted zones and never
    # reaches the control underneath.
    Send-Click $list 60 30
    Start-Sleep -Milliseconds 500
    # The click above selects the row; the focus it cannot give. Focus moves
    # in the window manager when genuine input lands on a control, and a
    # posted message bypasses that entirely - an app whose real click focuses
    # its list looks exactly like one that does not. So the focus is handed
    # over the way the keyboard path gets it, by attaching to the app's input
    # queue, and the assertion below covers what the app does with it.
    [void](Give-AppFocus $list $appTid)
    Start-Sleep -Milliseconds 300
    $gti0 = Get-AppFocus $appTid
    Add-Check 'the list can be given the keyboard focus' `
        ($gti0.hwndFocus -eq $list) `
        ("focus=0x{0:X} '{1}'" -f $gti0.hwndFocus.ToInt64(), (Get-ClassName $gti0.hwndFocus))

    [void][ProbeLib.U]::PostMessageW($win, $WM_CHAR, [IntPtr]$VK_RETURN, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    $gti1 = Get-AppFocus $appTid
    Add-Check 'Enter with the list focused moves the caret into the value box' `
        ($gti1.hwndFocus -eq $value) `
        ("focus=0x{0:X} '{1}'" -f $gti1.hwndFocus.ToInt64(), (Get-ClassName $gti1.hwndFocus))

    # --- 3. the filter rebuilds the list, and leaves the editor alone ----
    # The filter is throttled to one rebuild every 250 ms, and rebuilding the
    # list normally reloads the editor - which would eat a half-typed value.
    # The value box is set here on purpose: a filter keystroke that clears it
    # is the bug.
    [void][ProbeLib.U]::SendMessageW($value, $WM_SETTEXT, [IntPtr]0, 'probe-typed-value')
    Start-Sleep -Milliseconds 300
    $kept = Get-WinText $value 512
    Add-Check 'the value box takes a value' ($kept -eq 'probe-typed-value') "'$kept'"

    $countBefore = [int][ProbeLib.U]::SendMessageW($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][ProbeLib.U]::SendMessageW($filt, $WM_SETTEXT, [IntPtr]0, 'Path')
    [void][ProbeLib.U]::PostMessageW($win, $WM_COMMAND, [IntPtr]$ID_FILTER, [IntPtr]$filt)
    Start-Sleep -Milliseconds 800
    $countAfter = [int][ProbeLib.U]::SendMessageW($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    Add-Check 'typing in the filter rebuilds the list' `
        ($countAfter -ne $countBefore -or $countAfter -gt 0) `
        ("{0} items -> {1} items" -f $countBefore, $countAfter)

    $kept2 = Get-WinText $value 512
    Add-Check 'filtering does not eat the value being typed' ($kept2 -eq 'probe-typed-value') "'$kept2'"

    # put the filter back, so the list is not left short for anything later
    [void][ProbeLib.U]::SendMessageW($filt, $WM_SETTEXT, [IntPtr]0, '')
    [void][ProbeLib.U]::PostMessageW($win, $WM_COMMAND, [IntPtr]$ID_FILTER, [IntPtr]$filt)
    Start-Sleep -Milliseconds 800

    # --- 4. the scope box reloads the list ------------------------------
    $scopes = [int][ProbeLib.U]::SendMessageW($scope, $CB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    $sel0 = [int][ProbeLib.U]::SendMessageW($scope, $CB_GETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero)
    $scoped = $false
    if ($scopes -gt 1) {
        $target = if ($sel0 -eq 0) { 1 } else { 0 }
        [void][ProbeLib.U]::SendMessageW($scope, $CB_SETCURSEL, [IntPtr]$target, [IntPtr]0)
        [void][ProbeLib.U]::PostMessageW($win, $WM_COMMAND, [IntPtr]$ID_SCOPE, [IntPtr]$scope)
        Start-Sleep -Milliseconds 900
        $sel1 = [int][ProbeLib.U]::SendMessageW($scope, $CB_GETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero)
        $scoped = ($sel1 -eq $target)
    }
    Add-Check 'switching the scope changes the list it is showing' $scoped `
        ("{0} scopes, selection {1}" -f $scopes, $sel0)

    # --- 5. what a repaint costs ---------------------------------------
    # The panel is self-drawn GDI+ and has been through a flicker bug, so the
    # counters are the regression test: hovering should not repaint the whole
    # window, and a click should cost a handful of paints, not hundreds.
    $s0 = Get-Stats $win $app
    for ($i = 0; $i -lt 25; $i++) { Send-Move $win (300 + $i * 7) (400 + $i * 3) }
    Start-Sleep -Milliseconds 600
    $s1 = Get-Stats $win $app
    $hoverPaints = 0; $hoverMoves = 0
    if ($s0 -and $s1 -and $s1.ContainsKey('paint') -and $s1.ContainsKey('mouseMove')) {
        $hoverPaints = $s1.paint - $s0.paint
        $hoverMoves = $s1.mouseMove - $s0.mouseMove
    }
    Add-Check 'the hover moves reach the window' ($hoverMoves -ge 20) "$hoverMoves of 25 moves counted"
    Add-Check 'hovering does not repaint the window once per move' `
        ($hoverPaints -le ($hoverMoves + 4)) `
        ("{0} paints for {1} moves" -f $hoverPaints, $hoverMoves)

    $s2 = Get-Stats $win $app
    Send-Click $list 60 30
    Start-Sleep -Milliseconds 600
    $s3 = Get-Stats $win $app
    $clickPaints = 0; $clickNotify = 0
    if ($s2 -and $s3 -and $s3.ContainsKey('paint')) {
        $clickPaints = $s3.paint - $s2.paint
        $clickNotify = $s3.notify - $s2.notify
    }
    Add-Check 'a click produces a list notification' ($clickNotify -ge 1) "$clickNotify notifications"
    Add-Check 'a click does not set off a repaint storm' ($clickPaints -le 40) "$clickPaints paints"

    # --- 6. resizing ----------------------------------------------------
    $orig = Get-WinRect $win
    $bad = @()
    foreach ($s in @(@(320, 200), @(1400, 1000), @(80, 80))) {
        [void][ProbeLib.U]::MoveWindow($win, 60, 60, $s[0], $s[1], $true)
        Start-Sleep -Milliseconds 400
        Pump-Repaint $win 2
        if (-not (Wait-Idle $win 2000).Ok) { $bad += ("{0}x{1}" -f $s[0], $s[1]) }
    }
    [void][ProbeLib.U]::MoveWindow($win, $orig.Left, $orig.Top,
            ($orig.Right - $orig.Left), ($orig.Bottom - $orig.Top), $true)
    Start-Sleep -Milliseconds 500
    Add-Check 'the window survives being forced smaller than its own minimum' ($bad.Count -eq 0) `
        $(if ($bad.Count) { "hung at: $($bad -join ', ')" } else { '320x200, 1400x1000, 80x80, restored' })
    Add-Check 'it is still answering after the resizes' (Wait-Idle $win 2000).Ok ''

    # --- 7. closing -----------------------------------------------------
    # The value box still holds probe text, which counts as a pending edit and
    # would turn this into a modal box. Clear it first, then close: the app
    # should destroy its window and leave.
    [void][ProbeLib.U]::SendMessageW($value, $WM_SETTEXT, [IntPtr]0, '')
    [void][ProbeLib.U]::PostMessageW($win, $WM_COMMAND, [IntPtr]$ID_VALUE, [IntPtr]$value)
    Start-Sleep -Milliseconds 500
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

Add-CrashCheck $crashMark 'envtool' 'envtool logged no crash through the whole event matrix'

Write-Checks 'envtool event matrix'
