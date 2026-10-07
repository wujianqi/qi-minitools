# probe-focus.ps1 - does the capture overlay actually own the keyboard?
#
# Every existing probe opens and drives the overlay with PostMessage, which
# delivers WM_KEYDOWN/WM_CHAR straight to the window regardless of focus. That
# is why "Esc cancels" and the tool letters pass in CI while a real user, who
# can only type at whatever window owns the focus, gets nothing.
#
# This probe asks the only question that matters: while the overlay is up, who
# has the focus, and does a REAL Esc keystroke actually tear the capture down?
#
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 reads a .ps1 without
# a BOM as ANSI, and mojibake in a comment can swallow the next statement.

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace FocusProbe -Name U -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)]
public struct GUITHREADINFO {
    public int cbSize; public int flags;
    public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
    public RECT rcCaret;
}
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO gti);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$KEYEVENTF_KEYUP = 0x0002
$VK_ESCAPE = 0x1B
$HOST_CLASS = 'MuskShotHost'; $OVERLAY_CLASS = 'ShotOverlayCls'

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][FocusProbe.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [FocusProbe.U+EnumProc] {
        param($h, $x)
        $pid2 = 0
        [void][FocusProbe.U]::GetWindowThreadProcessId($h, [ref]$pid2)
        if ($pid2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][FocusProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}
function Wait-Window([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Window $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 100
    }
    [IntPtr]::Zero
}

# Whose window is holding the keyboard right now?
function Show-Focus([IntPtr]$overlay) {
    $gti = New-Object FocusProbe.U+GUITHREADINFO
    $gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($gti)
    $ok = [FocusProbe.U]::GetGUIThreadInfo(0, [ref]$gti)
    $fg = [FocusProbe.U]::GetForegroundWindow()

    Write-Host ("  foreground window : 0x{0:X}  class='{1}'" -f $fg.ToInt64(), (Get-ClassName $fg))
    if (-not $ok) { Write-Host "  GetGUIThreadInfo failed"; return }
    $f = $gti.hwndFocus
    $act = $gti.hwndActive
    Write-Host ("  hwndActive        : 0x{0:X}  class='{1}'" -f $act.ToInt64(), (Get-ClassName $act))
    Write-Host ("  hwndFocus         : 0x{0:X}  class='{1}'" -f $f.ToInt64(), (Get-ClassName $f))
    Write-Host ("  hwndCapture       : 0x{0:X}  class='{1}'" -f $gti.hwndCapture.ToInt64(), (Get-ClassName $gti.hwndCapture))

    if ($f -eq $overlay) {
        Write-Host "  VERDICT: the overlay OWNS the keyboard - Esc will work" -ForegroundColor Green
    } else {
        Write-Host "  VERDICT: the overlay does NOT own the keyboard - Esc goes elsewhere" -ForegroundColor Red
    }
    if ($fg -eq $overlay) {
        Write-Host "  (overlay is the foreground window)"
    } else {
        Write-Host "  (overlay is NOT the foreground window)" -ForegroundColor Yellow
    }
}

$errLog = Join-Path $root '..\build\probe-focus-stderr.txt'
$proc = Start-Process -FilePath $Exe -PassThru `
        -RedirectStandardError $errLog -RedirectStandardOutput 'NUL'
Write-Host "pid: $($proc.Id)"

$script:verdict = 'UNKNOWN'
try {
    $hostWin = Wait-Window $proc.Id $HOST_CLASS 6000
    if ($hostWin -eq [IntPtr]::Zero) { throw "host window never appeared" }

    # open a capture the same way every other probe does
    [void][FocusProbe.U]::PostMessageW($hostWin, 0x8001, [IntPtr]::Zero, [IntPtr]0x0202)
    $ov = Wait-Window $proc.Id $OVERLAY_CLASS 8000
    if ($ov -eq [IntPtr]::Zero) { throw "overlay never appeared" }
    Write-Host "overlay = 0x$($ov.ToInt64().ToString('X'))  visible=$([FocusProbe.U]::IsWindowVisible($ov))"
    Write-Host "focus state while the overlay is up:"
    Show-Focus $ov

    Write-Host ""
    Write-Host "now pressing a REAL Esc (keybd_event, not PostMessage)..."
    [FocusProbe.U]::keybd_event($VK_ESCAPE, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [FocusProbe.U]::keybd_event($VK_ESCAPE, 0, $KEYEVENTF_KEYUP, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 900

    $still = Find-Window $proc.Id $OVERLAY_CLASS
    if ($still -eq [IntPtr]::Zero) {
        Write-Host "RESULT: real Esc DID close the capture" -ForegroundColor Green
        $script:verdict = 'OK'
    } else {
        Write-Host "RESULT: real Esc did NOTHING - the capture is still up and still eating the mouse" -ForegroundColor Red
        $script:verdict = 'TRAPPED'
    }
}
catch {
    Write-Host "probe error: $_" -ForegroundColor Red
    $script:verdict = 'ERROR'
}
finally {
    # tear the overlay down the only way that always works: a posted WM_CHAR,
    # which needs no focus, and failing that the process itself.
    $h = Find-Window $proc.Id $OVERLAY_CLASS
    if ($h -ne [IntPtr]::Zero) {
        [void][FocusProbe.U]::PostMessageW($h, 0x0102, [IntPtr]0x1B, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
    }
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host "VERDICT=$($script:verdict)"
if ($script:verdict -eq 'TRAPPED') { exit 2 }
exit 0
