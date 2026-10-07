# probe-toolbar-tip.ps1 - drives a capture, hovers a toolbar button, and checks
# that the explanation bubble actually comes up. Optionally saves a PNG so the
# toolbar can be looked at.
#
#   .\test\probe-toolbar-tip.ps1 [-Exe .\bin\shot.exe] [-Hover 7] [-Out shot.png]
#
# Hover defaults to the mosaic button. The toolbar geometry comes from the app's
# own --verbose dump, and the result is checked through the app's WM_APP+2 state
# query rather than by looking at pixels - that way the probe never has to
# agree with the app about DPI, which is a losing game when one side is
# per-monitor aware and the other is not.
# ASCII only (see probe-tray-stress.ps1).
[CmdletBinding()]
param([string]$Exe, [int]$Hover = 7, [string]$Out, [switch]$NoShot)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not $Out) { $Out = Join-Path $root '..\build\toolbar-tip.png' }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace TipProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@
Add-Type -AssemblyName System.Drawing
# The app is per-monitor DPI aware, so the screen has to be grabbed the same
# way or the PNG comes back in virtualised coordinates and nothing lines up.
[void][TipProbe.U]::SetProcessDPIAware()

$WM_APP=0x8000; $WM_LBUTTONUP=0x0202; $WM_LBUTTONDOWN=0x0201; $WM_MOUSEMOVE=0x0200
$MK=1; $HOST_CLASS='MuskShotHost'; $OVL='ShotOverlayCls'

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][TipProbe.U]::GetClassNameW($h, $sb, 256); $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [TipProbe.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][TipProbe.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][TipProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    $script:found
}
function Wait-Window([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Window $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 80
    }
    [IntPtr]::Zero
}
function Post-Mouse([IntPtr]$h, [uint32]$m, [int]$x, [int]$y, [int]$w) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    if (-not [TipProbe.U]::PostMessageW($h, $m, [IntPtr]$w, $lp)) {
        throw "PostMessageW(0x$($m.ToString('X'))) failed on 0x$($h.ToInt64().ToString('X'))"
    }
}
function Save-Screen([string]$path) {
    $bmp = New-Object System.Drawing.Bitmap(
        [System.Windows.Forms.SystemInformation]::VirtualScreen.Width,
        [System.Windows.Forms.SystemInformation]::VirtualScreen.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(
        [System.Windows.Forms.SystemInformation]::VirtualScreen.Left,
        [System.Windows.Forms.SystemInformation]::VirtualScreen.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
function Read-Query([string]$errLog) {
    $line = Get-Content $errLog | Where-Object { $_ -match '^query:' } | Select-Object -Last 1
    if ($line -notmatch 'tbHot=(-?\d+) tbDown=(-?\d+) tipShown=(-?\d+) tool=(\d+)') { return $null }
    @{ hot = [int]$Matches[1]; down = [int]$Matches[2]; shown = [int]$Matches[3]; tool = [int]$Matches[4] }
}
function Query-Count([string]$errLog) {
    if (-not (Test-Path $errLog)) { return 0 }
    return @(Get-Content $errLog | Where-Object { $_ -match '^query:' }).Count
}

# The app answers WM_APP+2 by logging its state. Sleeping a fixed time and then
# reading the answer is a race: when the machine is busy the hover has not been
# processed yet, and the probe reports a failure that does not exist. This polls
# instead, and only accepts an answer that arrived after the question was asked.
function Wait-Query([IntPtr]$ov, [string]$errLog, [scriptblock]$want, [int]$ms = 3000) {
    $end = (Get-Date).AddMilliseconds($ms)
    $q = $null
    while ((Get-Date) -lt $end) {
        $before = Query-Count $errLog
        Post-Mouse $ov ($WM_APP + 2) 0 0
        Start-Sleep -Milliseconds 120
        if ((Query-Count $errLog) -le $before) { continue }
        $q = Read-Query $errLog
        if ($q -and (& $want $q)) { return $q }
    }
    return $q
}

$errLog = Join-Path $root '..\build\probe-tip-stderr.txt'
$proc = Start-Process -FilePath $Exe -PassThru -ArgumentList '--verbose' `
        -RedirectStandardError $errLog -RedirectStandardOutput 'NUL'
$fail = 0
try {
    $hostWin = Wait-Window $proc.Id $HOST_CLASS 6000
    if ($hostWin -eq [IntPtr]::Zero) { throw "no host window" }

    [void][TipProbe.U]::PostMessageW($hostWin, ($WM_APP+1), [IntPtr]::Zero, [IntPtr]$WM_LBUTTONUP)
    $ov = Wait-Window $proc.Id $OVL 8000
    if ($ov -eq [IntPtr]::Zero) { throw "no overlay after tray click" }

    # a selection in the middle of the screen, well clear of the edges so the
    # toolbar has somewhere to go
    $sx = 200; $sy = 150
    Post-Mouse $ov $WM_LBUTTONDOWN $sx $sy $MK
    Post-Mouse $ov $WM_MOUSEMOVE ($sx+700) ($sy+420) $MK
    Post-Mouse $ov $WM_LBUTTONUP   ($sx+700) ($sy+420) 0
    Start-Sleep -Milliseconds 400

    $tb = @()
    if (Test-Path $errLog) { $tb = @(Get-Content $errLog | Where-Object { $_ -match '^\s+tb\[' }) }
    if ($tb.Count -eq 0) { throw "no toolbar dump in stderr (is this a --verbose build?)" }

    $sel = $tb[$Hover]
    if ($sel -notmatch 'tb\[\s*\d+\] id=(-?\d+).*?(-?\d+),(-?\d+) (\d+)x(\d+)\s+(\S.*)$') {
        throw "could not parse toolbar item: $sel"
    }
    $id  = [int]$Matches[1]
    $cx  = [int]$Matches[2] + [int]([int]$Matches[4] / 2)
    $cy  = [int]$Matches[3] + [int]([int]$Matches[5] / 2)
    $tip = $Matches[6].Trim()
    "item $Hover -> id=$id centre=$cx,$cy tip='$tip'"

    # aim the real cursor (the app is DPI aware, so physical coords are right)
    [void][TipProbe.U]::SetCursorPos($cx, $cy)
    Post-Mouse $ov $WM_MOUSEMOVE $cx $cy 0
    $q = Wait-Query $ov $errLog { param($s) $s.hot -eq $Hover }
    if (-not $q) { throw "no state query came back" }
    "after the move: hot=$($q.hot) shown=$($q.shown)"
    if ($q.hot -ne $Hover) {
        Write-Host "FAIL: hover landed on item $($q.hot), expected $Hover" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK: cursor over item $Hover is tracked" -ForegroundColor Green
    }

    # the bubble appears after the hover delay plus a timer tick to repaint
    $q = Wait-Query $ov $errLog { param($s) $s.shown -eq $Hover }
    "after the delay: hot=$($q.hot) shown=$($q.shown)"
    if (-not $q -or $q.shown -ne $Hover) {
        Write-Host "FAIL: no bubble for item $Hover (tipShown=$($q.shown))" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK: bubble for item $Hover is up" -ForegroundColor Green
    }

    # and it must go away again when the pointer leaves
    Post-Mouse $ov $WM_MOUSEMOVE ($cx + 400) ($cy + 300) 0
    $q = Wait-Query $ov $errLog { param($s) $s.shown -eq -1 }
    "after leaving: hot=$($q.hot) shown=$($q.shown)"
    if ($q.shown -ne -1) {
        Write-Host "FAIL: bubble stayed up after the pointer left" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK: bubble clears when the pointer leaves" -ForegroundColor Green
    }

    # put it back for the screenshot
    [void][TipProbe.U]::SetCursorPos($cx, $cy)
    Post-Mouse $ov $WM_MOUSEMOVE $cx $cy 0
    Start-Sleep -Milliseconds 1200
    if (-not $NoShot) { Save-Screen $Out; "saved $Out" }
}
catch {
    Write-Host "FAIL: $_" -ForegroundColor Red
    $fail++
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}
if ($fail -eq 0) { Write-Host "RESULT: toolbar hover bubble works" -ForegroundColor Green }
else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red }
exit $fail
