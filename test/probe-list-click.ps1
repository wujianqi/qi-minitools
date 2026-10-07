# probe-list-click.ps1 - black-box check that the env-var list can be selected
# with the mouse, and that clicking does not trigger a repaint storm.
#
#   .\test\probe-list-click.ps1 [-Exe .\bin\envtool.exe]
#
# Drives the real window with posted mouse messages (no cursor hijacking) and
# reads the ListView selection back with LVM_GETNEXTITEM, so it observes
# exactly what a user would see. Exits 0 when every click lands.

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is not yet bound while a param default is evaluated, so resolve
# the default from the script path instead - same idiom as build.ps1.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\envtool.exe' }
if (-not (Test-Path $Exe)) { throw "envtool.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace ETProbe -Name U -MemberDefinition @'
[DllImport("user32.dll", SetLastError=true)] public static extern IntPtr FindWindowW(string c, string n);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr x);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetDlgCtrlID(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr rgn, uint flags);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, System.Text.StringBuilder l);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
'@

$WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$LVM_GETITEMCOUNT      = 0x1004
$LVM_GETNEXTITEM       = 0x100C
$LVNI_SELECTED         = 0x0002
$MK_LBUTTON            = 0x0001

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][ETProbe.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}
function Get-CtrlId([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][ETProbe.U]::GetWindowTextW($h, $sb, 512)
    [pscustomobject]@{ Id = [ETProbe.U]::GetDlgCtrlID($h); Text = $sb.ToString() }
}

# --- launch -------------------------------------------------------------
# Run verbose with stderr captured: the app's [dbg] trace is what tells us
# whether a click reached WM_NOTIFY/LVN_ITEMCHANGED at all.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$logDir = Join-Path $root '..\build'
$errLog = Join-Path $logDir 'probe-envtool-stderr.txt'
$outLog = Join-Path $logDir 'probe-envtool-stdout.txt'
$proc = Start-Process -FilePath $Exe -ArgumentList '--verbose' -PassThru `
        -RedirectStandardError $errLog -RedirectStandardOutput $outLog
$main = [IntPtr]::Zero
try {
    for ($i = 0; $i -lt 60 -and $main -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        $cb = [ETProbe.U+EnumProc] {
            param($h, $x)
            $pid2 = 0
            [void][ETProbe.U]::GetWindowThreadProcessId($h, [ref]$pid2)
            if ($pid2 -eq $proc.Id -and (Get-ClassName $h) -eq 'MuskEnvTool') {
                $script:main = $h
                return $false
            }
            return $true
        }
        [void][ETProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    }
    if ($main -eq [IntPtr]::Zero) { throw "main window never appeared (pid $($proc.Id))" }

    [void][ETProbe.U]::SetForegroundWindow($main)
    [void][ETProbe.U]::UpdateWindow($main)

    # --- find the list view and the value edit -------------------------
    $list = [IntPtr]::Zero
    $kids = @()
    $ccb = [ETProbe.U+EnumProc] {
        param($h, $x)
        $c = Get-ClassName $h
        $script:kids += [pscustomobject]@{ H = $h; Class = $c; Info = (Get-CtrlId $h) }
        if ($c -like 'SysListView32*') { $script:list = $h }
        return $true
    }
    [void][ETProbe.U]::EnumChildWindows($main, $ccb, [IntPtr]::Zero)

    Write-Host "main=$main list=$list"
    Write-Host "children:"
    $kids | ForEach-Object {
        Write-Host ("  id={0,-5} class={1,-18} vis={2} text='{3}'" -f `
            $_.Info.Id, $_.Class, [ETProbe.U]::IsWindowVisible($_.H), $_.Info.Text)
    }
    if ($list -eq [IntPtr]::Zero) { throw "SysListView32 child not found" }

    $count = [int][ETProbe.U]::SendMessageW($list, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Host "list item count = $count"
    if ($count -lt 1) { throw "list is empty, nothing to select" }

    $lr = New-Object ETProbe.U+RECT
    [void][ETProbe.U]::GetWindowRect($list, [ref]$lr)
    Write-Host "list rect L=$($lr.L) T=$($lr.T) R=$($lr.R) B=$($lr.B)"

    # --- click rows and read the selection back ------------------------
    function Get-Selected([IntPtr]$lv) {
        $sel = [ETProbe.U]::SendMessageW($lv, $LVM_GETNEXTITEM, [IntPtr](-1), [IntPtr]$LVNI_SELECTED)
        if ($sel -eq [IntPtr](-1)) { -1 } else { [int]$sel }
    }
    # Force the app down its normal repaint path (parent WM_PAINT, which also
    # re-places the child controls) without waiting for user input. RDW_ALLCHILDREN
    # matters: the flicker bug lives in the parent's paint -> child re-place path.
    $RDW_INVALIDATE = 0x0001; $RDW_UPDATENOW = 0x0100; $RDW_ALLCHILDREN = 0x0080
    function Pump-Repaint([IntPtr]$h, [int]$times = 3) {
        for ($k = 0; $k -lt $times; $k++) {
            [void][ETProbe.U]::RedrawWindow($h, [IntPtr]::Zero, [IntPtr]::Zero,
                ($RDW_INVALIDATE -bor $RDW_UPDATENOW -bor $RDW_ALLCHILDREN))
            Start-Sleep -Milliseconds 150
        }
    }
    # NOTE: GetWindowTextW() returns "" for a caption-less control in another
    # process *by design* (so callers cannot hang on a busy app). WM_GETTEXT is
    # one of the few messages USER32 marshals cross-process, so an Edit's real
    # contents have to be read that way.
    $WM_GETTEXT = 0x000D
    function Get-EditText([int]$id, [int]$max = 60) {
        $e = $kids | Where-Object { $_.Info.Id -eq $id } | Select-Object -First 1
        if (-not $e) { return '<missing control>' }
        $sb = New-Object System.Text.StringBuilder 8192
        [void][ETProbe.U]::SendMessageW($e.H, $WM_GETTEXT, [IntPtr]8192, $sb)
        $t = $sb.ToString()
        if ($t.Length -gt $max) { $t.Substring(0, $max) + "..." } else { $t }
    }
    function Get-EditorState() {
        "name='$(Get-EditText 1002)' value='$(Get-EditText 1003 40)'"
    }
    function Click-ListRow([IntPtr]$lv, [int]$x, [int]$y) {
        $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
        # let the app finish any pending layout/paint work between the two halves
        [void][ETProbe.U]::PostMessageW($lv, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
        [void][ETProbe.U]::UpdateWindow($main)
        Start-Sleep -Milliseconds 120
        [void][ETProbe.U]::PostMessageW($lv, $WM_LBUTTONUP, [IntPtr]::Zero, $lp)
        [void][ETProbe.U]::UpdateWindow($main)
        Start-Sleep -Milliseconds 120
    }

    $fail = 0
    # the list has a header row; items start under it. Probe a few offsets so
    # the test does not depend on an exact row height.
    foreach ($row in 0, 1, 2) {
        $y = 20 + $row * 20
        Click-ListRow $list 60 $y
        $got = Get-Selected $list
        $ok = ($got -ge 0)
        if (-not $ok) { $fail++ }
        Write-Host ("click y={0,-4} -> selected {1,-3} {2}" -f $y, $got, $(if ($ok) { 'OK' } else { 'FAIL (click selected nothing)' }))
    }

    Write-Host ""
    Write-Host "--- selection survival across the app's own repaint ---"
    Click-ListRow $list 60 40
    $before = Get-Selected $list
    Write-Host "  selected before repaint = $before ; $(Get-EditorState)"
    Pump-Repaint $main
    $after = Get-Selected $list
    Write-Host "  selected after  repaint = $after ; $(Get-EditorState)"
    if ($before -lt 0) {
        Write-Host "  FAIL: could not select an item to begin with"
        $fail++
    }
    elseif ($after -ne $before) {
        Write-Host "  FAIL: repaint cleared/moved the selection ($before -> $after)" -ForegroundColor Red
        $fail++
    }
    else {
        Write-Host "  OK: selection survives repaint" -ForegroundColor Green
    }

    # --- app-level selection must follow the list selection ------------
    Write-Host ""
    Write-Host "--- editor follows the list ---"
    $edEmpty = $false
    foreach ($row in 0, 1, 2, 3) {
        Click-ListRow $list 60 (20 + $row * 20)
        $sel = Get-Selected $list
        $name = Get-EditText 1002
        $val  = Get-EditText 1003 30
        $empty = ($name -eq '' -and $val -eq '')
        if ($empty) { $edEmpty = $true }
        Write-Host ("  row {0}: listSel={1,-3} name='{2}' value='{3}' {4}" -f $row, $sel, $name, $val, $(if ($empty) { '<- EDITOR EMPTY' } else { '' }))
    }
    if ($edEmpty) {
        Write-Host "  FAIL: the list highlights a row but the editor stays empty - clicks do not reach the app" -ForegroundColor Red
        $fail++
    }
    else {
        Write-Host "  OK: every click loaded the variable into the editor" -ForegroundColor Green
    }

    Write-Host ""
    if ($fail -eq 0) { Write-Host "RESULT: list is selectable by mouse" -ForegroundColor Green }
    else { Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red }
    exit $(if ($fail -eq 0) { 0 } else { 1 })
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}
