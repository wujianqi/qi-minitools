# probe-hotkey-conflict.ps1 - the default shortcut is Ctrl+Shift+A, and when
# something else already owns it the app asks instead of substituting.
#
#   .\test\probe-hotkey-conflict.ps1 [-Exe .\bin\shot.exe]
#
# The old build walked a list of six alternatives and quietly landed on the
# first free one, reporting success. That is the worst possible answer to "I
# pressed Ctrl+Shift+A and nothing happened": the shortcut the user has been
# pressing in QQ's screenshot tool for years silently became something else,
# and the tray tip was the only place that said so. So this asks the question
# directly - take Ctrl+Shift+A away, start the app, and see whether the shortcut
# dialog comes up.
#
# Holding a global hot key needs a live process that registers it, so the probe
# takes the key itself and gives it back afterwards. The stored shortcut is
# saved and restored around the run, for the same reason: a probe that leaves
# the machine's settings different from how it found them is a probe that
# changes the next test's answer.
#
# ASCII only (see probe-hotkey.ps1).

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace HkConflict -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint vk);
[DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h, int id);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$WM_CLOSE = 0x0010
$HOST_CLASS = 'MuskShotHost'; $DLG_CLASS = 'MuskShotHotkeyDlg'
$MOD_CONTROL = 0x0002; $MOD_ALT = 0x0001; $MOD_SHIFT = 0x0004; $MOD_NOREPEAT = 0x4000
$VK_A = 0x41
$regPath = 'HKCU:\Software\MuskTools\Shot'
$regName = 'Capture'

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][HkConflict.U]::GetClassNameW($h, $sb, 256); $sb.ToString()
}
function Get-Text([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][HkConflict.U]::GetWindowTextW($h, $sb, 512); $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Top([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [HkConflict.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][HkConflict.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][HkConflict.U]::EnumWindows($cb, [IntPtr]::Zero)
    $script:found
}
function Wait-Top([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Top $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 100
    }
    [IntPtr]::Zero
}

$fail = 0
$hadOld = (Get-ItemProperty -Path $regPath -Name $regName -ErrorAction SilentlyContinue).$regName
$proc = $null
$held = $false
try {
    # The default has to be reachable for the test to mean anything: whatever
    # the user chose last time is tried first and would never get as far as
    # Ctrl+Shift+A.
    try { Remove-ItemProperty -Path $regPath -Name $regName -ErrorAction SilentlyContinue } catch { }

    $held = [HkConflict.U]::RegisterHotKey([IntPtr]::Zero, 1,
             [uint32]($MOD_CONTROL -bor $MOD_SHIFT -bor $MOD_NOREPEAT), [uint32]$VK_A)
    if (-not $held) {
        # Ctrl+Shift+A is already owned by something on this machine - QQ's
        # screenshot tool, most likely. That is the same situation the test is
        # about, so the run continues and says so, rather than reporting a
        # failure of the app.
        Write-Host "note: Ctrl+Shift+A was already taken here; using that conflict instead" -ForegroundColor Yellow
    } else {
        Write-Host "took Ctrl+Shift+A for the duration of this probe" -ForegroundColor DarkGray
    }

    $proc = Start-Process -FilePath $Exe -PassThru -RedirectStandardOutput 'NUL' `
            -RedirectStandardError (Join-Path $root '..\build\probe-hotkey-conflict-stderr.txt')

    $hostWin = Wait-Top $proc.Id $HOST_CLASS 6000
    if ($hostWin -eq [IntPtr]::Zero) { throw "no host window (pid $($proc.Id))" }

    $dlg = Wait-Top $proc.Id $DLG_CLASS 6000
    if ($dlg -eq [IntPtr]::Zero) {
        Write-Host "FAIL: Ctrl+Shift+A was unavailable and the app did not ask for a shortcut" -ForegroundColor Red
        $fail++
    } else {
        # The class name is the whole identity check: the dialog the menu's
        # "modify shortcut" entry opens has this class and nothing else does.
        Write-Host "OK: the shortcut dialog opened by itself (class $DLG_CLASS)" -ForegroundColor Green
        # Closing it without choosing is a supported answer: the app has to come
        # back to life with no shortcut rather than sitting on a dialog forever.
        [void][HkConflict.U]::PostMessageW($dlg, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 900
        # Parenthesised on purpose. Written as
        #     if (Find-Top $proc.Id $DLG_CLASS -ne [IntPtr]::Zero)
        # PowerShell hands "-ne" to the function as another argument, the call
        # returns the class name as a non-empty string, and the test is true for
        # ever - so a dialog that never closed would still report as closed.
        if ((Find-Top $proc.Id $DLG_CLASS) -ne [IntPtr]::Zero) {
            Write-Host "FAIL: the dialog ignored the close" -ForegroundColor Red
            $fail++
        } else {
            Write-Host "OK: it closed on cancel and the app carried on" -ForegroundColor Green
        }
    }

    $proc.Refresh()
    if ($proc.HasExited) {
        Write-Host ("FAIL: the app exited instead of waiting for a shortcut (exit {0})" -f $proc.ExitCode) -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK: still running with no shortcut, tray menu can still capture" -ForegroundColor Green
    }
}
catch {
    Write-Host "FAIL: $_" -ForegroundColor Red
    $fail++
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    if ($held) { [void][HkConflict.U]::UnregisterHotKey([IntPtr]::Zero, 1) }
    try {
        if ($hadOld) {
            New-Item -Path $regPath -Force | Out-Null
            Set-ItemProperty -Path $regPath -Name $regName -Value $hadOld
        }
    } catch { }
}

if ($fail -eq 0) { Write-Host "RESULT: a taken default shortcut makes the app ask, not substitute" -ForegroundColor Green }
else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red }
exit $fail
