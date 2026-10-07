# probe-tray-menu.ps1 - drives the tray context-menu commands the way the
# menu handler does: post the WM_COMMAND the menu returns, straight to the
# hidden host window.
#
#   .\test\probe-tray-menu.ps1 [-Exe .\bin\shot.exe]
#
# host_proc is re-entered by SendMessage from inside its own WM_CONTEXTMENU
# handler, so each command here also exercises that re-entrancy.
#
# NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads a .ps1 without
# a BOM as ANSI, and mojibake in a comment can swallow the next statement.

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace MenuProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$WM_COMMAND = 0x0111; $WM_CHAR = 0x0102
$VK_ESCAPE = 0x1B
$HOST_CLASS = 'MuskShotHost'; $OVERLAY_CLASS = 'ShotOverlayCls'
$MI_CAPTURE = 40001; $MI_DELAY3 = 40002; $MI_DELAY5 = 40003
$MI_HOTKEYS = 40005; $MI_ABOUT = 40006; $MI_EXIT = 40007

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][MenuProbe.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
# The delegate callback can only reach a script-scoped variable.
$script:found = [IntPtr]::Zero
function Find-Window([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [MenuProbe.U+EnumProc] {
        param($h, $x)
        $pid2 = 0
        [void][MenuProbe.U]::GetWindowThreadProcessId($h, [ref]$pid2)
        if ($pid2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][MenuProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}
function Wait-Window([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Window $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 200
    }
    [IntPtr]::Zero
}

$logDir = Join-Path $root '..\build'
$errLog = Join-Path $logDir 'probe-shotmenu-stderr.txt'
$proc = $null
function Start-App {
    $p = Start-Process -FilePath $Exe -ArgumentList '--verbose' -PassThru `
            -RedirectStandardError $errLog -RedirectStandardOutput 'NUL'
    $h = Wait-Window $p.Id $HOST_CLASS 5000
    if ($h -eq [IntPtr]::Zero) { throw "host window never appeared (pid $($p.Id))" }
    return [pscustomobject]@{ Proc = $p; Host = $h }
}

$script:fail = 0
$app = Start-App
$proc = $app.Proc
$host1 = $app.Host
Write-Host "host = $host1"

try {
    # Each of these opens a capture, so the caller cancels it again and checks
    # the app is still there afterwards.
    function Test-MenuCmd([int]$id, [string]$name, [int]$settleMs) {
        [void][MenuProbe.U]::PostMessageW($host1, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
        Start-Sleep -Milliseconds $settleMs
        $proc.Refresh()
        if ($proc.HasExited) {
            Write-Host ("FAIL: menu '{0}' (id {1}) killed the app (exit {2})" -f $name, $id, $proc.ExitCode) -ForegroundColor Red
            $script:fail++
            return
        }
        $ov = Find-Window $proc.Id $OVERLAY_CLASS
        if ($ov -eq [IntPtr]::Zero) {
            Write-Host ("FAIL: menu '{0}' (id {1}) produced no overlay" -f $name, $id) -ForegroundColor Red
            $script:fail++
            return
        }
        [void][MenuProbe.U]::PostMessageW($ov, $WM_CHAR, [IntPtr]$VK_ESCAPE, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 700
        $proc.Refresh()
        if ($proc.HasExited) {
            Write-Host ("FAIL: app exited after cancelling '{0}' (exit {1})" -f $name, $proc.ExitCode) -ForegroundColor Red
            $script:fail++
            return
        }
        Write-Host ("OK: menu '{0}' (id {1})" -f $name, $id) -ForegroundColor Green
    }

    # capture - no delay, overlay immediately
    Test-MenuCmd $MI_CAPTURE 'capture' 1500
    # delay 3s - runs the delay pump first
    Test-MenuCmd $MI_DELAY3 'delay 3s' 5000
    # delay 5s
    Test-MenuCmd $MI_DELAY5 'delay 5s' 7000

    # The two message-box commands are modal, so a running instance would block
    # the next step: open one, confirm the app survived, restart, move on.
    foreach ($m in @(@($MI_HOTKEYS, 'hotkeys'), @($MI_ABOUT, 'about'))) {
        [void][MenuProbe.U]::PostMessageW($host1, $WM_COMMAND, [IntPtr]$m[0], [IntPtr]::Zero)
        Start-Sleep -Milliseconds 1200
        $proc.Refresh()
        if ($proc.HasExited) {
            Write-Host ("FAIL: menu '{0}' killed the app (exit {1})" -f $m[1], $proc.ExitCode) -ForegroundColor Red
            $script:fail++
        } else {
            Write-Host ("OK: menu '{0}' opened its dialog" -f $m[1]) -ForegroundColor Green
        }
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 400
        $app = Start-App
        $proc = $app.Proc
        $host1 = $app.Host
    }
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
if ($script:fail -eq 0) { Write-Host "RESULT: every tray-menu command survived" -ForegroundColor Green; exit 0 }
else { Write-Host ("RESULT: {0} check(s) failed" -f $script:fail) -ForegroundColor Red; exit 1 }
