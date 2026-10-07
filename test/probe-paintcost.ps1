# probe-paintcost.ps1 - how fast can the overlay actually repaint?
#
# Moves nothing. It drives the overlay entirely with PostMessage, the same way
# diag-shot does, and then measures the one thing that decides whether dragging
# feels like a freeze: how many mouse-move updates per second the window can
# retire.
#
# A real drag delivers up to ~125 WM_MOUSEMOVE per second. If the window can
# only retire a handful, the queue backs up and the selection rectangle stops
# following the cursor - which is what "frozen" looks like from the chair.
#
# The WM_NULL round trip between batches is the backpressure signal: the thread
# is single, so a slow paint shows up directly as a slow answer.
#
# ASCII only (see probe-tray-stress.ps1).

[CmdletBinding()]
param([string]$Exe, [int]$Moves = 120, [int]$PauseMs = 0)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace PaintProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr res);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$WM_NULL = 0x0000; $WM_MOUSEMOVE = 0x0200
$WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$SMTO_ABORTIFHUNG = 0x0002; $SMTO_BLOCK = 0x0001
$HOST_CLASS = 'MuskShotHost'; $OVERLAY_CLASS = 'ShotOverlayCls'
$MK_LBUTTON = 1

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][PaintProbe.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [PaintProbe.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][PaintProbe.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][PaintProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}
function Wait-Window([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Window $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 50
    }
    [IntPtr]::Zero
}
function Post-Mouse([IntPtr]$h, [uint32]$m, [int]$x, [int]$y, [int]$w) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [void][PaintProbe.U]::PostMessageW($h, $m, [IntPtr]$w, $lp)
}
function RoundTrip([IntPtr]$h, [int]$ms) {
    $res = [IntPtr]::Zero
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [void][PaintProbe.U]::SendMessageTimeoutW($h, $WM_NULL, [IntPtr]::Zero, [IntPtr]::Zero,
                                             ($SMTO_ABORTIFHUNG -bor $SMTO_BLOCK), $ms, [ref]$res)
    $sw.Stop()
    $sw.Elapsed.TotalMilliseconds
}

$sw = [PaintProbe.U]::GetSystemMetrics(0)
$sh = [PaintProbe.U]::GetSystemMetrics(1)
$proc = Start-Process -FilePath $Exe -PassThru -RedirectStandardOutput 'NUL' `
        -RedirectStandardError (Join-Path $root '..\build\probe-paintcost-stderr.txt')
try {
    $hostWin = Wait-Window $proc.Id $HOST_CLASS 6000
    if ($hostWin -eq [IntPtr]::Zero) { throw "no host window" }
    [void][PaintProbe.U]::PostMessageW($hostWin, 0x8001, [IntPtr]::Zero, [IntPtr]0x0202)
    $ov = Wait-Window $proc.Id $OVERLAY_CLASS 8000
    if ($ov -eq [IntPtr]::Zero) { throw "overlay never appeared" }
    "virtual screen: ${sw}x${sh}   overlay=0x$($ov.ToInt64().ToString('X'))"
    "baseline WM_NULL round trip: $([math]::Round((RoundTrip $ov 3000),1)) ms"
    ""

    # --- phase 1: selecting, which paints the frozen capture + magnifier ----
    $x0 = [int]($sw * 0.15); $y0 = [int]($sh * 0.15)
    $x1 = [int]($sw * 0.85); $y1 = [int]($sh * 0.85)
    Post-Mouse $ov $WM_LBUTTONDOWN $x0 $y0 $MK_LBUTTON
    $t = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 1; $i -le $Moves; $i++) {
        $f = $i / $Moves
        Post-Mouse $ov $WM_MOUSEMOVE ($x0 + [int](($x1-$x0)*$f)) ($y0 + [int](($y1-$y0)*$f)) $MK_LBUTTON
        if ($PauseMs -gt 0) { Start-Sleep -Milliseconds $PauseMs }
    }
    $drain = RoundTrip $ov 10000
    $t.Stop()
    "SELECTING  $Moves moves: posted in $([math]::Round($t.Elapsed.TotalMilliseconds,0)) ms, " +
    "drained in $([math]::Round($drain,1)) ms  -> $([math]::Round($Moves/($drain/1000),0)) updates/s"
    Post-Mouse $ov $WM_LBUTTONUP $x1 $y1 0
    Start-Sleep -Milliseconds 400
    "after mouse-up, WM_NULL round trip: $([math]::Round((RoundTrip $ov 10000),1)) ms"
    ""

    # --- phase 2: annotating, which repaints base + layer every move --------
    $t = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 1; $i -le $Moves; $i++) {
        $f = $i / $Moves
        Post-Mouse $ov $WM_MOUSEMOVE ($x0 + [int](($x1-$x0)*$f)) ($y0 + [int](($y1-$y0)*$f)) 0
    }
    $drain2 = RoundTrip $ov 20000
    $t.Stop()
    "HOVERING   $Moves moves: posted in $([math]::Round($t.Elapsed.TotalMilliseconds,0)) ms, " +
    "drained in $([math]::Round($drain2,1)) ms  -> $([math]::Round($Moves/($drain2/1000),0)) updates/s"
    ""
    "a real mouse delivers up to ~125 moves/s; anything under ~60 feels like lag"
}
catch {
    "probe error: $_"
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}
