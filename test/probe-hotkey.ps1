# probe-hotkey.ps1 - opens the shortcut dialog, records a combination in it,
# and checks what the app ends up using.
#
#   .\test\probe-hotkey.ps1 [-Exe .\bin\shot.exe] [-Out hotkey.png]
#
# The tray menu entry that opens it is driven through WM_COMMAND with lParam 0,
# the same way the real menu command arrives, so this covers the whole path
# rather than just the dialog window.
# ASCII only (see probe-tray-stress.ps1).
[CmdletBinding()]
param([string]$Exe, [string]$Out, [switch]$NoShot)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\shot.exe' }
if (-not $Out) { $Out = Join-Path $root '..\build\hotkey.png' }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace HkProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@
Add-Type -AssemblyName System.Drawing
[void][HkProbe.U]::SetProcessDPIAware()

$WM_COMMAND = 0x0111; $WM_KEYDOWN = 0x0100; $WM_KEYUP = 0x0101
$WM_GETTEXT = 0x000D; $WM_GETTEXTLENGTH = 0x000E
$MI_SETKEY = 40008
$HOST_CLASS = 'MuskShotHost'; $DLG_CLASS = 'MuskShotHotkeyDlg'
$VK_CONTROL = 0x11; $VK_SHIFT = 0x10; $VK_ESCAPE = 0x1B; $VK_RETURN = 0x0D

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][HkProbe.U]::GetClassNameW($h, $sb, 256); $sb.ToString()
}
function Get-Text([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][HkProbe.U]::GetWindowTextW($h, $sb, 512); $sb.ToString()
}
$script:found = [IntPtr]::Zero
function Find-Top([int]$procId, [string]$class) {
    $script:found = [IntPtr]::Zero
    $cb = [HkProbe.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][HkProbe.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $procId -and (Get-ClassName $h) -eq $class) { $script:found = $h; return $false }
        return $true
    }
    [void][HkProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    $script:found
}
function Wait-Top([int]$procId, [string]$class, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        $h = Find-Top $procId $class
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 80
    }
    [IntPtr]::Zero
}
function Children([IntPtr]$parent) {
    $list = New-Object System.Collections.ArrayList
    $cb = [HkProbe.U+EnumProc] {
        param($h, $x)
        [void]$list.Add($h)
        return $true
    }
    [void][HkProbe.U]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    $list
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

$proc = Start-Process -FilePath $Exe -PassThru -RedirectStandardOutput 'NUL' `
        -RedirectStandardError (Join-Path $root '..\build\probe-hotkey-stderr.txt')
$fail = 0

# The stored shortcut is cleared first, and put back at the end.
#
# This check used to read HKCU\Software\MuskTools\Shot\Capture and call it a
# pass when it said "Ctrl+Shift+F9" - which is what the *previous* run left
# there. A probe that only passes when it runs after itself is worse than no
# probe: it goes green on a run where the app never saved anything, and it
# goes red on a clean machine. The value the app writes is now observed as a
# change from a known starting point, and the machine is left as it was found.
$regPath = 'HKCU:\Software\MuskTools\Shot'
$regName = 'Capture'
$hadOld = $null
$oldVal = (Get-ItemProperty -Path $regPath -Name $regName -ErrorAction SilentlyContinue).$regName
if ($oldVal) { $hadOld = $oldVal }
try { Remove-ItemProperty -Path $regPath -Name $regName -ErrorAction SilentlyContinue } catch { }
try {
    $hostWin = Wait-Top $proc.Id $HOST_CLASS 6000
    if ($hostWin -eq [IntPtr]::Zero) { throw "no host window" }

    # The tray menu's "modify shortcut" path, driven the way a menu fires it.
    [void][HkProbe.U]::PostMessageW($hostWin, $WM_COMMAND, [IntPtr]$MI_SETKEY, [IntPtr]0)
    # Wait-Top, not Find-Top: a posted command is processed asynchronously, so
    # a single enumeration races the dialog's CreateWindow and loses almost
    # every time now that it is not being rescued by scheduling luck. The
    # stray 6000 on the old Find-Top call was being discarded - Find-Top takes
    # no timeout - which is why this read as an app bug.
    $dlg = Wait-Top $proc.Id $DLG_CLASS 6000
    if ($dlg -eq [IntPtr]::Zero) { throw "the shortcut dialog never opened" }
    "dialog: '$((Get-Text $dlg))'"
    if (-not $NoShot) { Save-Screen $Out; "saved $Out" }

    # the capture box is the first BUTTON child
    $cap = [IntPtr]::Zero
    foreach ($c in (Children $dlg)) {
        if ((Get-ClassName $c) -eq 'Button') { $cap = $c; break }
    }
    if ($cap -eq [IntPtr]::Zero) { throw "the dialog has no capture box" }
    "capture box: '$((Get-Text $cap))'"

    [void][HkProbe.U]::SetForegroundWindow($dlg)
    [void][HkProbe.U]::SetFocus($cap)
    Start-Sleep -Milliseconds 200

    # Send Ctrl+Shift+F9. A key message carries the modifier state in the high
    # byte of lParam (Shift, Ctrl, Alt, in that order), and the app reads that
    # as its fallback - so this does not depend on the real keyboard having
    # focus, which an automated session cannot guarantee.
    $VK_F9 = 0x78
    $state = (0x01 -bor 0x02)          # Shift + Ctrl
    $lp = [IntPtr]((1 -shl 30) -bor ($state -shl 16) -bor $VK_F9)
    [void][HkProbe.U]::PostMessageW($dlg, $WM_KEYDOWN, [IntPtr]$VK_F9, $lp)
    Start-Sleep -Milliseconds 400
    [void][HkProbe.U]::PostMessageW($dlg, $WM_KEYUP, [IntPtr]$VK_F9, [IntPtr]0)
    Start-Sleep -Milliseconds 300
    if (-not $NoShot) { Save-Screen $Out; "saved $Out (after the key press)" }

    # commit it through the OK button, exactly as a click does
    [void][HkProbe.U]::PostMessageW($dlg, $WM_COMMAND, [IntPtr]102, [IntPtr]0)
    Start-Sleep -Milliseconds 600
    $still = Find-Top $proc.Id $DLG_CLASS
    if ($still -ne [IntPtr]::Zero) {
        Write-Host "FAIL: the dialog stayed open - the combination was rejected" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK: the dialog accepted the combination and closed" -ForegroundColor Green
    }

    # whatever was chosen has to have been written to the registry. The value
    # was removed above, so anything here was written by this run.
    $val = (Get-ItemProperty -Path $regPath -Name $regName -ErrorAction SilentlyContinue).$regName
    "registry now says: '$val' (it held '$hadOld' before this run)"
    if (-not $val) {
        Write-Host "FAIL: the shortcut was not written" -ForegroundColor Red
        $fail++
    } elseif ($val -ne 'Ctrl+Shift+F9') {
        Write-Host "FAIL: expected 'Ctrl+Shift+F9', got '$val'" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK: the shortcut was written as Ctrl+Shift+F9" -ForegroundColor Green
    }
}
catch {
    Write-Host "FAIL: $_" -ForegroundColor Red
    $fail++
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    # put the machine's shortcut back the way it was found
    try {
        if ($hadOld) {
            New-Item -Path $regPath -Force | Out-Null
            Set-ItemProperty -Path $regPath -Name $regName -Value $hadOld
        } else {
            Remove-ItemProperty -Path $regPath -Name $regName -ErrorAction SilentlyContinue
        }
    } catch { }
}
if ($fail -eq 0) { Write-Host "RESULT: shortcut conflict handling works end to end" -ForegroundColor Green }
else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red }
exit $fail
