# probe-delete-click.ps1 - black-box check that the self-drawn buttons on the
# right-hand side actually fire when clicked.
#
#   .\test\probe-delete-click.ps1 [-Exe .\bin\envtool.exe]
#
# The buttons are not controls: the parent draws them and hit-tests the mouse
# by hand, so the self-test can only prove the rectangles exist. This drives
# the real window with posted WM_LBUTTONDOWN/WM_LBUTTONUP at the coordinates
# build_layout() would have produced, and watches for something observable.
#
# 复制值 is the control case and 删除 is the case under test. They travel the
# exact same dispatch path - pressed index on DOWN, same index on UP - but
# 复制值 leaves proof on the clipboard while 删除 leaves proof in a dialog.
# Comparing them separates "every button on this strip is dead" from "the
# delete button specifically is dead", which no amount of rectangle checking
# can tell apart.
#
# The delete confirmation is never answered: the dialog is detected, the
# process is killed, and the registry is only touched on IDYES.

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'
trap { Write-Host ("AT LINE {0}: {1}" -f $_.InvocationInfo.ScriptLineNumber, $_.InvocationInfo.Line.Trim()); Write-Host ("MSG: {0}" -f $_.Exception.Message); break }

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\envtool.exe' }
if (-not (Test-Path $Exe)) { throw "envtool.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace ETClick -Name U -MemberDefinition @'
[DllImport("user32.dll", SetLastError=true)] public static extern IntPtr FindWindowW(string c, string n);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr x);
[DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, System.Text.StringBuilder l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr rgn, uint flags);
[DllImport("user32.dll")] public static extern bool OpenClipboard(IntPtr h);
[DllImport("user32.dll")] public static extern bool CloseClipboard();
[DllImport("user32.dll")] public static extern IntPtr GetClipboardData(uint f);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
public struct RECT { public int L,T,R,B; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

# Read the window the same way the app draws it. Without this the probe is
# DPI-*un*aware, GetClientRect hands back virtualised numbers, and every
# coordinate derived from them below is a fraction off - by 25% on a 125%
# display, which is enough to put a click in the gap between two buttons and
# make a working one look dead.
[void][ETClick.U]::SetProcessDPIAware()

$WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$MK_LBUTTON     = 0x0001
$CF_UNICODETEXT = 13
$RDW_INVALIDATE = 0x0001; $RDW_UPDATENOW = 0x0100; $RDW_ALLCHILDREN = 0x0080
# The app's layout scale. build_layout() does `int S = app_scale_px(1)`, and
# the truncation is the point: app_scale_px(1) is 1.25 on a 125% display and
# that lands as 1, so the panel is laid out in unscaled units. Computing it the
# same way here - truncate the ratio rather than using it - is what keeps the
# arithmetic below in step with the app's. Using 1.25 instead puts every
# button a quarter of the way off and lands the clicks in the gaps between
# them, which reads as a dead button.
$S = 1

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][ETClick.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}

function Get-ClipboardText {
    if (-not [ETClick.U]::OpenClipboard([IntPtr]::Zero)) { return $null }
    try {
        $h = [ETClick.U]::GetClipboardData($CF_UNICODETEXT)
        if ($h -eq [IntPtr]::Zero) { return $null }
        $p = [Runtime.InteropServices.Marshal]::PtrToStringUni($h)
        return $p
    } finally { [void][ETClick.U]::CloseClipboard() }
}

$logDir = Join-Path $root '..\build'
$errLog = Join-Path $logDir 'probe-delete-stderr.txt'
$proc = Start-Process -FilePath $Exe -ArgumentList '--verbose' -PassThru `
        -RedirectStandardError $errLog -RedirectStandardOutput (Join-Path $logDir 'probe-delete-stdout.txt')
$main = [IntPtr]::Zero
try {
    for ($i = 0; $i -lt 60 -and $main -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        $cb = [ETClick.U+EnumProc] {
            param($h, $x)
            $pid2 = 0
            [void][ETClick.U]::GetWindowThreadProcessId($h, [ref]$pid2)
            if ($pid2 -eq $proc.Id -and (Get-ClassName $h) -eq 'MuskEnvTool') {
                $script:main = $h; return $false
            }
            return $true
        }
        [void][ETClick.U]::EnumWindows($cb, [IntPtr]::Zero)
    }
    if ($main -eq [IntPtr]::Zero) { throw "main window never appeared (pid $($proc.Id))" }
    [void][ETClick.U]::SetForegroundWindow($main)
    [void][ETClick.U]::UpdateWindow($main)

    $list = [IntPtr]::Zero
    $ccb = [ETClick.U+EnumProc] {
        param($h, $x)
        if ((Get-ClassName $h) -like 'SysListView32*') { $script:list = $h; return $false }
        return $true
    }
    [void][ETClick.U]::EnumChildWindows($main, $ccb, [IntPtr]::Zero)
    if ($list -eq [IntPtr]::Zero) { throw "SysListView32 child not found" }

    $cr = New-Object ETClick.U+RECT
    [void][ETClick.U]::GetClientRect($main, [ref]$cr)
    $W = $cr.R - $cr.L; $H = $cr.B - $cr.T
    $dpi = [ETClick.U]::GetDpiForWindow($main)
    $S = [int]($dpi / 96.0)
    Write-Host ("main={0} client={1}x{2} dpi={3} layoutScale={4}" -f $main, $W, $H, $dpi, $S)

    # --- build_layout(), transcribed ------------------------------------
    # Same arithmetic as src\envtool\envtool.c, so the probe clicks where the
    # app actually drew rather than at a guessed offset.
    $pad = 10*$S; $bh = 30*$S; $gap = 6*$S
    # The banner band (the administrator notice) and the gap under it are part
    # of the vertical budget: leaving them out pushed every y below the top of
    # the form down by banH+gap, which put the strip's clicks 34px under the
    # real buttons and made a working 删除 read as dead. The band disappears on
    # a short window, exactly as build_layout decides it.
    $banH = 28*$S
    if ($H -lt 380*$S) { $banH = 0 }
    $listW = [int](($W - $pad*3) * 44 / 100)
    $rx  = $pad + $listW + $pad
    $rw  = $W - $rx - $pad
    $colH = $H - $pad - 30*$S - $banH - $gap
    $actTop = ($pad + $colH) - 36*$S
    $abw = [int](($rw - $gap*2) / 3)
    if ($abw -gt 104*$S) { $abw = 104*$S }
    if ($abw -lt 40*$S)  { $abw = 40*$S }
    # Each element is parenthesised on purpose. PowerShell binds `,` tighter
    # than `+`, so an unbracketed `$a + $b + $c, $d` is parsed as
    # `$a + $b + ($c, $d)` and fails on the array.
    $actModify = @(($rx), ($actTop), ($rx + $abw), ($actTop + 36*$S))
    $actDelete = @(($rx + $abw + $gap), ($actTop), ($rx + $abw*2 + $gap), ($actTop + 36*$S))
    $actCopy   = @(($rx + $abw*2 + $gap*2), ($actTop), ($rx + $abw*3 + $gap*2), ($actTop + 36*$S))
    Write-Host ("strip y={0}..{1}" -f $actTop, ($actTop + 36*$S))
    Write-Host ("  修改  x={0}..{1}" -f $actModify[0], $actModify[2])
    Write-Host ("  删除  x={0}..{1}" -f $actDelete[0], $actDelete[2])
    Write-Host ("  复制值 x={0}..{1}" -f $actCopy[0],   $actCopy[2])

    function Click-Parent([int]$x, [int]$y) {
        $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
        [void][ETClick.U]::PostMessageW($main, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
        [void][ETClick.U]::UpdateWindow($main)
        Start-Sleep -Milliseconds 150
        [void][ETClick.U]::PostMessageW($main, $WM_LBUTTONUP, [IntPtr]::Zero, $lp)
        [void][ETClick.U]::UpdateWindow($main)
        Start-Sleep -Milliseconds 250
    }
    function Click-ListRow([int]$x, [int]$y) {
        $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
        [void][ETClick.U]::PostMessageW($list, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
        [void][ETClick.U]::UpdateWindow($main)
        Start-Sleep -Milliseconds 120
        [void][ETClick.U]::PostMessageW($list, $WM_LBUTTONUP, [IntPtr]::Zero, $lp)
        [void][ETClick.U]::UpdateWindow($main)
        Start-Sleep -Milliseconds 150
    }
    function Find-Dialog {
        # Retried, not sampled once. A confirmation box is created by the app
        # some way behind the click that asked for it - the click is posted,
        # the app has to wake up, run its hit test, and MessageBox has to build
        # the window - so a single enumeration taken straight afterwards reads
        # the desktop as it was before the box existed and reports "no dialog".
        # That is not a rare race either: it is the normal case, and a probe
        # that misses every dialog it asked for concludes the button is dead.
        for ($try = 1; $try -le 12; $try++) {
            $script:found = [IntPtr]::Zero
            $cb = [ETClick.U+EnumProc] {
                param($h, $x)
                $pid2 = 0
                [void][ETClick.U]::GetWindowThreadProcessId($h, [ref]$pid2)
                if ($pid2 -eq $proc.Id -and (Get-ClassName $h) -eq '#32770') {
                    $script:found = $h; return $false
                }
                return $true
            }
            [void][ETClick.U]::EnumWindows($cb, [IntPtr]::Zero)
            if ($script:found -ne [IntPtr]::Zero) { return $script:found }
            Start-Sleep -Milliseconds 150
        }
        return [IntPtr]::Zero
    }
    function Pump-Repaint {
        for ($k = 0; $k -lt 3; $k++) {
            [void][ETClick.U]::RedrawWindow($main, [IntPtr]::Zero, [IntPtr]::Zero,
                ($RDW_INVALIDATE -bor $RDW_UPDATENOW -bor $RDW_ALLCHILDREN))
            Start-Sleep -Milliseconds 150
        }
    }

    $fail = 0

    # --- nothing selected: the strip must be inert --------------------
    Write-Host ""
    Write-Host "--- with nothing selected the strip is inert ---"
    Click-Parent ($actDelete[0] + 10) ($actDelete[1] + 10)
    $dlg = Find-Dialog
    if ($dlg -ne [IntPtr]::Zero) { Write-Host "  FAIL: a dialog appeared with no row selected" -ForegroundColor Red; $fail++ }
    else { Write-Host "  OK: no dialog" -ForegroundColor Green }

    # --- select a row -------------------------------------------------
    Click-ListRow 60 20
    Pump-Repaint
    Write-Host "  (row 0 selected)"

    # --- control case: 复制值 -----------------------------------------
    Write-Host ""
    Write-Host "--- 复制值 (control: same dispatch path, observable on the clipboard) ---"
    $SENTINEL = 'PROBE-SENTINEL-NOT-THE-VARIABLE'
    Set-Clipboard -Value $SENTINEL
    Start-Sleep -Milliseconds 200
    $cx = $actCopy[0] + [int](($actCopy[2] - $actCopy[0]) / 2)
    $cy = $actCopy[1] + [int](($actCopy[3] - $actCopy[1]) / 2)
    Click-Parent $cx $cy
    $clip = Get-ClipboardText
    $copyOk = ($clip -and $clip.Trim().Length -gt 0 -and $clip -ne $SENTINEL)
    Write-Host ("  clipboard after 复制值: '{0}'  {1}" -f ($(if ($clip) { $clip.Substring(0, [Math]::Min(50, $clip.Length)) } else { '<empty>' })), $(if ($copyOk) { 'OK' } else { 'FAIL - button did not fire' }))
    if (-not $copyOk) { $fail++ }

    # --- case under test: 删除 ---------------------------------------
    # The list is dominated by system variables - Path, TEMP, APPDATA and the
    # rest of the protected set - and for those 删除 deliberately shows a status
    # line instead of a confirmation. "No dialog" is therefore the correct
    # answer on those rows, so the probe has to read the variable name and step
    # over them before it can call a missing dialog a failure.
    $PROTECTED = @(
        'path','pathext','systemroot','windir','systemdrive','comspec','temp','tmp',
        'os','processor_architecture','processor_identifier','processor_level',
        'processor_revision','number_of_processors','homedrive','homepath',
        'userprofile','username','userdomain','userdomain_roamingprofile',
        'username_domain','appdata','localappdata','programdata',
        'allusersprofile','programfiles','programfiles(x86)','programw6432',
        'commonprogramfiles','commonprogramfiles(x86)','psmodulepath',
        'sessionname','driverdata','class_path','public','logonserver','onedrive',
        '__compat_layer','__derived_data_base'
    )
    function Get-NameEdit {
        # Collect the children first and pick the id out of the result, rather
        # than stashing it in a variable from inside the enumeration callback.
        # The callback form depends on $script: resolving to the scope the
        # scriptblock was compiled in, and when it does not the lookup quietly
        # returns no handle - which reads as "the app has no name box" and
        # turns every row below into a false failure.
        $list = New-Object System.Collections.ArrayList
        $cb2 = [ETClick.U+EnumProc] {
            param($h, $x)
            [void]$list.Add($h)
            return $true
        }
        [void][ETClick.U]::EnumChildWindows($main, $cb2, [IntPtr]::Zero)
        foreach ($c in $list) {
            if ([ETClick.U]::GetDlgCtrlID($c) -eq 1002) { return $c }
        }
        return [IntPtr]::Zero
    }
    function Get-NameText([IntPtr]$h) {
        if ($h -eq [IntPtr]::Zero) { return '<no name edit>' }
        $sb = New-Object System.Text.StringBuilder 4096
        [void][ETClick.U]::SendMessageW($h, 0x000D, [IntPtr]4096, $sb)   # WM_GETTEXT
        return $sb.ToString()
    }
    $nameEdit = Get-NameEdit
    $dx = $actDelete[0] + [int](($actDelete[2] - $actDelete[0]) / 2)
    $dy = $actDelete[1] + [int](($actDelete[3] - $actDelete[1]) / 2)

    Write-Host ""
    Write-Host "--- 删除 (the case under test) ---"
    $tested = 0; $sawPlain = $false; $fired = $false
    foreach ($row in 0..12) {
        Click-ListRow 60 (20 + $row * 20)
        Pump-Repaint
        $nm = Get-NameText $nameEdit
        if (-not $nm) { break }
        $prot = $PROTECTED -contains $nm.ToLowerInvariant()
        Write-Host ("  row {0,-2} {1,-28} {2}" -f $row, $nm, $(if ($prot) { '(protected - correctly no dialog)' } else { '(ordinary)' }))
        if ($prot) { continue }
        $sawPlain = $true
        $tested++
        Click-Parent $dx $dy
        $dlg = Find-Dialog
        if ($dlg -ne [IntPtr]::Zero) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][ETClick.U]::GetWindowTextW($dlg, $sb, 256)
            Write-Host ("    -> OK: 删除 fired, dialog title='{0}'" -f $sb.ToString()) -ForegroundColor Green
            $fired = $true
            break
        } else {
            Write-Host "    -> no dialog" -ForegroundColor Yellow
        }
        if ($tested -ge 3) { break }
    }
    if ($fired) {
        Write-Host "  OK: 删除 reaches its handler on an ordinary variable" -ForegroundColor Green
    } elseif (-not $sawPlain) {
        Write-Host "  SKIP: no ordinary (unprotected) variable in the first 13 rows" -ForegroundColor Yellow
    } else {
        Write-Host "  FAIL: 删除 reached no handler on $tested ordinary variable(s) while 复制值 on the same strip works" -ForegroundColor Red
        $fail++
    }

    Write-Host ""
    if ($fail -eq 0) { Write-Host "RESULT: the right-hand buttons respond to clicks" -ForegroundColor Green }
    else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red }
    exit $(if ($fail -eq 0) { 0 } else { 1 })
}
finally {
    # A modal confirmation may still be up; killing the process is what closes
    # it. Nothing has written to the registry - that needs IDYES.
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}
