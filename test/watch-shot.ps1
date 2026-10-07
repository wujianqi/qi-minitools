# watch-shot.ps1 - passive sampler for a shot.exe capture.
#
# Moves nothing. It only watches, so it can be left running while a human
# reproduces the freeze by hand.
#
# The one number that settles the argument is the SendMessageTimeout probe:
# a window that answers WM_NULL inside 2s is pumping messages no matter what
# it looks like on screen. A window that does not is genuinely hung, and the
# CPU column then says whether it is spinning or blocked on something.
#
# ASCII only (see probe-tray-stress.ps1).

[CmdletBinding()]
param([int]$Seconds = 120)

$ErrorActionPreference = 'Continue'
Add-Type -Namespace Watch -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr res);
[DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint a, bool i, uint pid);
[DllImport("kernel32.dll")] public static extern bool GetProcessTimes(IntPtr h, out long c, out long e, out long k, out long u);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$SMTO_ABORTIFHUNG = 0x0002
$WM_NULL = 0x0000
$PROC_QUERY_LIMITED = 0x1000

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][Watch.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
function Get-MyWindows([int]$procId) {
    $script:found = @()
    $cb = [Watch.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][Watch.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $procId) { $script:found += $h }
        return $true
    }
    [void][Watch.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

Write-Host "watching for shot.exe for $Seconds s - go ahead and start a capture."
Write-Host "(this moves nothing; press Ctrl+C to stop early)"
Write-Host ""

$handle = [IntPtr]::Zero
$lastCpu = 0.0
$end = (Get-Date).AddSeconds($Seconds)
$prev = Get-Date

while ((Get-Date) -lt $end) {
    $now = Get-Date
    $dt = ($now - $prev).TotalSeconds
    $prev = $now

    $p = Get-Process -Name 'shot' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) {
        Write-Host ("{0:HH:mm:ss}  (shot.exe not running)" -f $now)
        Start-Sleep -Milliseconds 500
        continue
    }

    # CPU%
    $cpu = $p.TotalProcessorTime.TotalSeconds
    $pct = if ($dt -gt 0) { [math]::Round((($cpu - $lastCpu) / $dt) * 100, 1) } else { 0 }
    $lastCpu = $cpu

    $line = "{0:HH:mm:ss}  cpu={1,5}%  threads={2,-3} handles={3,-5} ws={4,-7}MB" -f `
        $now, $pct, $p.Threads.Count, $p.HandleCount, [math]::Round($p.WorkingSet64 / 1MB, 1)

    foreach ($h in (Get-MyWindows $p.Id)) {
        $cls = Get-ClassName $h
        $res = [IntPtr]::Zero
        $ok = [Watch.U]::SendMessageTimeoutW($h, $WM_NULL, [IntPtr]::Zero, [IntPtr]::Zero,
                                            $SMTO_ABORTIFHUNG, 2000, [ref]$res)
        $state = if ($ok -eq [IntPtr]::Zero) { 'HUNG' } else { 'pumping' }
        $line += ("  [{0}] {1}" -f $cls, $state)
    }
    Write-Host $line

    Start-Sleep -Milliseconds 400
}
Write-Host ""
Write-Host "done."
