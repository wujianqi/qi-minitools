# probe-mosaic.ps1 - picks the mosaic button on the toolbar, drags it across the
# captured content, and saves a PNG so the result can be looked at.
#
#   .\test\probe-mosaic.ps1 [-Exe .\bin\shot.exe] [-Out mosaic.png]
#
# Clicks the button rather than posting a key, because that is the path the
# user described as missing - the button has to be there, findable and working,
# not just reachable from the keyboard.
# ASCII only (see probe-tray-stress.ps1).
[CmdletBinding()]
param([string]$Exe, [string]$Out)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not $Out) { $Out = Join-Path $root '..\build\mosaic.png' }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace MosProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@
Add-Type -AssemblyName System.Drawing
[void][MosProbe.U]::SetProcessDPIAware()

$WM_APP=0x8000; $WM_LBUTTONUP=0x0202; $WM_LBUTTONDOWN=0x0201; $WM_MOUSEMOVE=0x0200
$MK=1; $HOST_CLASS='MuskShotHost'; $OVL='ShotOverlayCls'
$TB_MOSAIC = 8   # toolbar id, must match TB_MOSAIC in shot.h

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][MosProbe.U]::GetClassNameW($h, $sb, 256); $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [MosProbe.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][MosProbe.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][MosProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
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
    if (-not [MosProbe.U]::PostMessageW($h, $m, [IntPtr]$w, $lp)) {
        throw "PostMessageW(0x$($m.ToString('X'))) failed"
    }
}
function Save-Screen([string]$path) {
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($vs.Left, $vs.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
function Read-Query([string]$errLog) {
    $line = Get-Content $errLog | Where-Object { $_ -match '^query:' } | Select-Object -Last 1
    if ($line -notmatch 'tbHot=(-?\d+) tbDown=(-?\d+) tipShown=(-?\d+) tool=(\d+) strokes=(\d+) cursor=(\d+) base=(\d+)x(\d+) layer=(\d+)') { return $null }
    @{ hot = [int]$Matches[1]; down = [int]$Matches[2]; shown = [int]$Matches[3]
      tool = [int]$Matches[4]; strokes = [int]$Matches[5]; cursor = [int]$Matches[6]
      baseW = [int]$Matches[7]; baseH = [int]$Matches[8]; layer = [int]$Matches[9]
      raw = $line.Trim() }
}

$errLog = Join-Path $root '..\build\probe-mosaic-stderr.txt'
$proc = Start-Process -FilePath $Exe -PassThru -ArgumentList '--verbose' `
        -RedirectStandardError $errLog -RedirectStandardOutput 'NUL'
$fail = 0
try {
    $hostWin = Wait-Window $proc.Id $HOST_CLASS 6000
    if ($hostWin -eq [IntPtr]::Zero) { throw "no host window" }
    [void][MosProbe.U]::PostMessageW($hostWin, ($WM_APP+1), [IntPtr]::Zero, [IntPtr]$WM_LBUTTONUP)
    $ov = Wait-Window $proc.Id $OVL 8000
    if ($ov -eq [IntPtr]::Zero) { throw "no overlay after tray click" }

    # the light sidebar on the left, which has text on a pale background -
    # a mosaic over a black terminal would be impossible to judge
    $sx = 20; $sy = 200
    Post-Mouse $ov $WM_LBUTTONDOWN $sx $sy $MK
    Post-Mouse $ov $WM_MOUSEMOVE ($sx+290) ($sy+700) $MK
    Post-Mouse $ov $WM_LBUTTONUP   ($sx+290) ($sy+700) 0
    Start-Sleep -Milliseconds 500

    $tb = @(Get-Content $errLog | Where-Object { $_ -match '^\s+tb\[' })
    if ($tb.Count -eq 0) { throw "no toolbar dump" }
    $mos = $tb | Where-Object { $_ -match "id=$TB_MOSAIC\s" } | Select-Object -First 1
    if (-not $mos) { throw "TB_MOSAIC (id=$TB_MOSAIC) is not on the toolbar at all" }
    if ($mos -notmatch 'id=\d+.*?(-?\d+),(-?\d+) (\d+)x(\d+)') { throw "cannot parse: $mos" }
    $mx = [int]$Matches[1] + [int]([int]$Matches[3] / 2)
    $my = [int]$Matches[2] + [int]([int]$Matches[4] / 2)
    "mosaic button at $mx,$my  ($($mos.Trim()))"

    # click it the way a user would: press and release on the same button
    Post-Mouse $ov $WM_LBUTTONDOWN $mx $my $MK
    Post-Mouse $ov $WM_MOUSEMOVE $mx $my $MK
    Post-Mouse $ov $WM_LBUTTONUP   $mx $my 0
    Start-Sleep -Milliseconds 400
    Post-Mouse $ov ($WM_APP + 2) 0 0
    Start-Sleep -Milliseconds 300
    $q = Read-Query $errLog
    "tool after the click: $($q.tool)  (T_MOSAIC is 7)"
    if ($q.tool -ne 7) {
        Write-Host "FAIL: clicking the mosaic button did not select the mosaic tool" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK: mosaic is the active tool" -ForegroundColor Green
    }

    # drag two mosaic strokes across the captured text
    foreach ($r in @(@(30,60,260,110), @(30,250,260,300), @(30,430,260,480))) {
        Post-Mouse $ov $WM_LBUTTONDOWN ($sx + $r[0]) ($sy + $r[1]) $MK
        Post-Mouse $ov $WM_MOUSEMOVE  ($sx + $r[2]) ($sy + $r[3]) $MK
        Post-Mouse $ov $WM_LBUTTONUP   ($sx + $r[2]) ($sy + $r[3]) 0
        Start-Sleep -Milliseconds 400
    }
    Post-Mouse $ov ($WM_APP + 2) 0 0
    Start-Sleep -Milliseconds 300
    $q2 = Read-Query $errLog
    Start-Sleep -Milliseconds 500
    Post-Mouse $ov ($WM_APP + 2) 0 0
    Start-Sleep -Milliseconds 300
    "state: $($q2)"
    Save-Screen $Out
    "saved $Out"
}
catch {
    Write-Host "FAIL: $_" -ForegroundColor Red
    $fail++
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}
if ($fail -eq 0) { Write-Host "RESULT: mosaic is on the toolbar and works" -ForegroundColor Green }
else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red }
exit $fail
