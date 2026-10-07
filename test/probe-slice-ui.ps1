# probe-slice-ui.ps1 - drives slice.exe's window the way a person does: by
# clicking its buttons.
#
#   .\test\probe-slice-ui.ps1 [-Exe .\bin\slice.exe] [-Work .\build\slice-ui-probe]
#
# probe-slice.ps1 opens the window with a file already on the command line, so
# it never touches a button. That gap was worth a whole class of bug: the panel
# looked right in a screenshot and every control in it was dead. This one starts
# the program with nothing, clicks Open, types a path into the dialog that
# actually appears, and works down the panel and out through Export - the only
# sequence that proves the buttons reach their handlers, that the shell dialog
# opens at all, and that what lands on disk is real.
#
# Controls are addressed by their numeric id from the tool's own enum rather
# than by caption, so a wording change in the UI cannot quietly rename a target
# out from under the probe.
#
# This is also the one probe in the suite that needs the real cursor, and that
# makes it the one that can measure the machine instead of the program. The
# first click is the tell: if it lands nothing and the desktop turns out not to
# be ours to click into, the rest is reported as skipped with the evidence
# rather than as a cascade of failures that look like button bugs. See
# Get-InputBlockReason for the distinction and why it is made from the outcome.
#
#   IDC_OPEN 100  IDC_EXPORT 102  IDC_EQ_ROWS 111  IDC_EQ_COLS 112
#   IDC_RESET 113  IDC_DIVN 114    IDC_DIV_H 115     IDC_DIV_V 116
#   IDC_DIV_ALL 117  IDC_DEL 118    IDC_MERGE 119    IDC_OUTDIR 123
[CmdletBinding()]
param(
    [string]$Exe,
    [string]$Work,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe)  { $Exe  = Join-Path $root '..\bin\slice.exe' }
if (-not $Work) { $Work = Join-Path $root '..\build\slice-ui-probe' }
$Exe  = (Resolve-Path $Exe).Path
$Work = [System.IO.Path]::GetFullPath($Work)

$script:failures = 0
$script:skipped  = 0
function Bad($msg) { Write-Host "FAIL: $msg" -ForegroundColor Red; $script:failures++ }
function Ok($msg)  { Write-Host "ok   $msg" }
function Skip($msg) { Write-Host "SKIP $msg" -ForegroundColor Yellow; $script:skipped++ }

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace SliceUI -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowTextW(IntPtr h, string s);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
[DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern IntPtr LoadCursorW(IntPtr inst, int name);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT ptScreenPos; }
public delegate bool EnumProc(IntPtr h, IntPtr p);
public struct RECT { public int L, T, R, B; }
public struct POINT { public int X, Y; }
'@
[void][SliceUI.U]::SetProcessDPIAware()

# --- window plumbing -------------------------------------------------------

function Get-Windows([int]$ProcId, [switch]$VisibleOnly) {
    $script:found = @()
    $cb = [SliceUI.U+EnumProc] {
        param($h, $x)
        $q = 0
        [void][SliceUI.U]::GetWindowThreadProcessId($h, [ref]$q)
        if ($q -eq $ProcId) {
            $vis = [SliceUI.U]::IsWindowVisible($h)
            if (-not $VisibleOnly -or $vis) {
                $cn = New-Object System.Text.StringBuilder 256
                [void][SliceUI.U]::GetClassNameW($h, $cn, 256)
                $script:found += [pscustomobject]@{
                    H = $h; Class = $cn.ToString(); Vis = $vis
                    En = [SliceUI.U]::IsWindowEnabled($h)
                    Id = [SliceUI.U]::GetWindowLongW($h, -12)
                }
            }
        }
        return $true
    }
    [void][SliceUI.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Get-Children([IntPtr]$Parent) {
    $script:kids = @()
    $cb = [SliceUI.U+EnumProc] {
        param($h, $x)
        $cn = New-Object System.Text.StringBuilder 256
        [void][SliceUI.U]::GetClassNameW($h, $cn, 256)
        $script:kids += [pscustomobject]@{ H = $h; Class = $cn.ToString()
            Id = [SliceUI.U]::GetWindowLongW($h, -12) }
        return $true
    }
    [void][SliceUI.U]::EnumChildWindows($Parent, $cb, [IntPtr]::Zero)
    return $script:kids
}

function Get-ChildById([IntPtr]$Parent, [int]$Id) {
    return @(Get-Children $Parent | Where-Object { $_.Id -eq $Id })[0]
}

# A real click, not BM_CLICK. A posted BM_CLICK works for most controls but it
# never exercises the focus and activation path, and the bug this probe exists
# to catch was a button that was visible and enabled and still did nothing.
function Invoke-Click([IntPtr]$Handle) {
    $rc = New-Object SliceUI.U+RECT
    [void][SliceUI.U]::GetWindowRect($Handle, [ref]$rc)
    $cx = [int](($rc.L + $rc.R) / 2)
    $cy = [int](($rc.T + $rc.B) / 2)
    [void][SliceUI.U]::SetCursorPos($cx, $cy)
    Start-Sleep -Milliseconds 120
    [SliceUI.U]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
    [SliceUI.U]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
}

# Everything below this point moves the real cursor and waits for the real thing
# a person would see, so all of it depends on the desktop actually handing input
# to this program. Sometimes it does not, and the reason is never slice.exe: a
# shell-hosted surface can park on the desktop and stop the session having any
# foreground window at all, and injected mouse input has nowhere to go when
# there is none. mouse_event and SendInput behave identically there - both
# report success and neither reaches the app.
#
# So the probe does not try to predict that. It clicks, and then asks what the
# desktop looked like. If the click missed AND the desktop was not ours to click
# into, that is a skip with the evidence attached; if the click missed while the
# desktop was fine, that is a real defect in the button and it fails. Deciding
# from the outcome rather than from a precondition is what keeps this honest -
# a precondition check races the window manager and flickers, and a flickering
# precondition is how a blocked session turns into four unrelated-looking
# failures instead of one honest skip.

# Describes whoever is holding the foreground, so a skip says who to close.
function Get-ForegroundOwner {
    $fg = [SliceUI.U]::GetForegroundWindow()
    $q = 0
    [void][SliceUI.U]::GetWindowThreadProcessId($fg, [ref]$q)
    $cn = New-Object System.Text.StringBuilder 256
    [void][SliceUI.U]::GetClassNameW($fg, $cn, 256)
    $tx = New-Object System.Text.StringBuilder 256
    [void][SliceUI.U]::GetWindowTextW($fg, $tx, 256)
    $t = $cn.ToString(); if (-not $t) { $t = '(no class)' }
    $u = $tx.ToString(); if ($u) { $u = " '$u'" }
    return "pid $q, class '$t'$u"
}

# Describes the topmost window sitting over a control, if it is not ours.
function Get-BlockerAt([IntPtr]$Hwnd, [int]$ProcId) {
    $rc = New-Object SliceUI.U+RECT
    [void][SliceUI.U]::GetWindowRect($Hwnd, [ref]$rc)
    $pt = New-Object SliceUI.U+POINT
    $pt.X = [int](($rc.L + $rc.R) / 2)
    $pt.Y = [int](($rc.T + $rc.B) / 2)
    $under = [SliceUI.U]::WindowFromPoint($pt)
    if ($under -eq [IntPtr]::Zero) { return "nothing at all is at $($pt.X),$($pt.Y)" }
    if ($under -eq $Hwnd) { return $null }
    $q = 0
    [void][SliceUI.U]::GetWindowThreadProcessId($under, [ref]$q)
    $cn = New-Object System.Text.StringBuilder 256
    [void][SliceUI.U]::GetClassNameW($under, $cn, 256)
    $wr = New-Object SliceUI.U+RECT
    [void][SliceUI.U]::GetWindowRect($under, [ref]$wr)
    $t = $cn.ToString(); if (-not $t) { $t = '(no class)' }
    $own = if ($q -eq $ProcId) { 'a window of the program under test' } else { "pid $q" }
    $en = if ([SliceUI.U]::IsWindowEnabled($under)) { 'enabled' } else { 'disabled' }
    return "$t ($own), $en, covering ($($wr.L),$($wr.T))-($($wr.R),$($wr.B))"
}

# Returns a human-readable reason when a missed click is the desktop's fault, or
# $null when the desktop was normal and the app really did drop the click.
function Get-InputBlockReason([IntPtr]$Hwnd, [int]$ProcId) {
    $fg = [SliceUI.U]::GetForegroundWindow()
    $why = @()
    if ($fg -eq [IntPtr]::Zero) {
        $why += "the session has no foreground window at all (GetForegroundWindow returned NULL), so injected mouse input has nothing to be routed to"
    } else {
        $q = 0
        [void][SliceUI.U]::GetWindowThreadProcessId($fg, [ref]$q)
        if ($q -ne $ProcId) { $why += "the foreground belongs to $(Get-ForegroundOwner)" }
    }
    $cur = New-Object SliceUI.U+POINT
    if (-not [SliceUI.U]::GetCursorPos([ref]$cur)) {
        $why += 'GetCursorPos failed, so the cursor never got where it was asked to go'
    } else {
        $blk = Get-BlockerAt $Hwnd $ProcId
        if ($blk) { $why += "the window under the cursor is $blk" }
    }
    if (-not $why.Count) { return $null }
    return ($why -join '; ')
}

# Waits for a visible #32770 owned by the process. A modal dialog disables its
# owner, which is the other half of the signal: a dialog that is on screen but
# has not taken over yet has not actually opened.
function Wait-Dialog([IntPtr]$Owner, [int]$Seconds) {
    for ($i = 0; $i -lt $Seconds * 4; $i++) {
        $d = @(Get-Windows -ProcId $script:appId -VisibleOnly |
               Where-Object { $_.Class -eq '#32770' })
        if ($d.Count -and -not [SliceUI.U]::IsWindowEnabled($Owner)) { return $d[0].H }
        Start-Sleep -Milliseconds 250
    }
    return [IntPtr]::Zero
}

function Close-Dialogs([IntPtr]$Owner) {
    for ($i = 0; $i -lt 16; $i++) {
        $d = @(Get-Windows -ProcId $script:appId -VisibleOnly |
               Where-Object { $_.Class -eq '#32770' })
        if (-not $d.Count) { return }
        # A message box here is the export confirmation, which offers to open
        # the output folder in Explorer. Answering no keeps the probe from
        # leaving a file manager behind.
        [void][SliceUI.U]::PostMessageW($d[0].H, 0x0111, [IntPtr]7, [IntPtr]::Zero)  # WM_COMMAND IDNO
        Start-Sleep -Milliseconds 250
        $still = @(Get-Windows -ProcId $script:appId -VisibleOnly |
                   Where-Object { $_.Class -eq '#32770' })
        if ($still.Count) { [void][SliceUI.U]::PostMessageW($d[0].H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
        Start-Sleep -Milliseconds 250
    }
}

# The file dialog's file-name box is a ComboBoxEx32 > ComboBox > Edit chain.
# Typing into anything else puts the text nowhere near where the dialog reads
# it, and setting the text with WM_SETTEXT does not work either: the dialog
# keeps its own copy of the name and only refreshes it from the edit's change
# notification, which WM_SETTEXT does not raise. Per-character WM_CHAR is what
# real typing produces, so that is what this sends - then VK_RETURN to accept.
function Set-DialogPath([IntPtr]$Dialog, [string]$Path) {
    $box = Get-Children $Dialog | Where-Object { $_.Class -eq 'ComboBoxEx32' } | Select-Object -First 1
    if (-not $box) { return $false }
    $combo = Get-Children $box.H | Where-Object { $_.Class -eq 'ComboBox' } | Select-Object -First 1
    if (-not $combo) { return $false }
    $edit = Get-Children $combo.H | Where-Object { $_.Class -eq 'Edit' } | Select-Object -First 1
    if (-not $edit) { return $false }
    foreach ($ch in $Path.ToCharArray()) {
        [void][SliceUI.U]::SendMessageW($edit.H, 0x0102, [IntPtr][int][char]$ch, [IntPtr]1)
    }
    Start-Sleep -Milliseconds 300
    [void][SliceUI.U]::PostMessageW($edit.H, 0x0100, [IntPtr]0x0D, [IntPtr]0)  # VK_RETURN down
    [void][SliceUI.U]::PostMessageW($edit.H, 0x0101, [IntPtr]0x0D, [IntPtr]0)  # up
    return $true
}

# --- source image ----------------------------------------------------------

$W = 240; $H = 120
function New-Source([string]$Path, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Red)
    $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Lime)
    $bb = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Blue)
    $g.FillRectangle($br, 0, 0, [int]($w/2), [int]($h/2))
    $g.FillRectangle($bg, [int]($w/2), 0, [int]($w/2), [int]($h/2))
    $g.FillRectangle($bb, 0, [int]($h/2), [int]($w/2), [int]($h/2))
    $br.Dispose(); $bg.Dispose(); $bb.Dispose(); $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Counts pixels of one colour inside a captured window. Used to prove the
# canvas really repainted, which is the difference between "the click did
# something" and "the click did the right thing".
# The wanted-colour parameters are WantR/WantG/WantB rather than R/G/B for a
# reason worth keeping: PowerShell variable names ignore case, so a plain $r
# local for the RECT below would be the same variable as a parameter $R and
# PowerShell would try to stuff a RECT into an [int] and throw.
function Count-Colour([IntPtr]$Hwnd, [int]$WantR, [int]$WantG, [int]$WantB) {
    $rect = New-Object SliceUI.U+RECT
    [void][SliceUI.U]::GetClientRect($Hwnd, [ref]$rect)
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max($rect.R,1)), ([Math]::Max($rect.B,1))
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    # PW_RENDERFULLCONTENT: the canvas is a GDI+ back buffer blitted in
    # WM_PAINT, and without the flag a DWM window comes back blank - which
    # would make every colour count below read zero for the wrong reason.
    $ok = [SliceUI.U]::PrintWindow($Hwnd, $hdc, 2)
    $gfx.ReleaseHdc($hdc); $gfx.Dispose()
    if (-not $ok) { $bmp.Dispose(); return -1 }
    $n = 0
    for ($y = 0; $y -lt $bmp.Height; $y += 2) {
        for ($x = 0; $x -lt $bmp.Width; $x += 2) {
            $c = $bmp.GetPixel($x, $y)
            if ([Math]::Abs($c.R - $WantR) -le 24 -and [Math]::Abs($c.G - $WantG) -le 24 -and
                [Math]::Abs($c.B - $WantB) -le 24) { $n++ }
        }
    }
    $bmp.Dispose()
    return $n
}

# Client coordinates -> screen. The real cursor is aimed through this rather
# than through a remembered window position, so a resize or a DPI change moves
# the pointer with the app instead of missing by however much it moved.
function Client-XY([IntPtr]$Hwnd, [int]$x, [int]$y) {
    $p = New-Object SliceUI.U+POINT
    $p.X = $x; $p.Y = $y
    [void][SliceUI.U]::ClientToScreen($Hwnd, [ref]$p)
    return $p
}

# The cursor the desktop is actually showing, at a client point. GetCursorInfo is
# the only outside view of what a WM_SETCURSOR handler decided; asking the window
# what cursor it "would" use just asks the handler again and agrees with it either
# way. The pointer is nudged off and back on first, because a cursor that only
# teleports does not always make the desktop deliver a move - and a reading with
# CURSOR_SHOWING clear describes the desktop, not the app, so it is polled for
# rather than taken.
function Showing-CursorAt([IntPtr]$Hwnd, [int]$cx, [int]$cy) {
    $p = New-Object SliceUI.U+POINT
    $p.X = $cx; $p.Y = $cy
    [void][SliceUI.U]::ClientToScreen($Hwnd, [ref]$p)
    [void][SliceUI.U]::SetCursorPos($p.X - 8, $p.Y)
    Start-Sleep -Milliseconds 60
    [void][SliceUI.U]::SetCursorPos($p.X, $p.Y)
    $last = [IntPtr](-2)
    for ($k = 0; $k -lt 20; $k++) {
        $ci = New-Object SliceUI.U+CURSORINFO
        $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ci)
        if ([SliceUI.U]::GetCursorInfo([ref]$ci) -and ($ci.flags -band 1)) {
            # Two readings that agree, so a cursor caught mid-change - or a
            # reading taken while the window was still activating and nothing
            # had repainted it - cannot be mistaken for a verdict.
            if ($ci.hCursor -eq $last) { return $ci.hCursor }
            $last = $ci.hCursor
        }
        Start-Sleep -Milliseconds 60
    }
    return [IntPtr](-1)
}

# Sweeps left to right along one row and returns the first client x where the
# canvas offers a left-right resize handle, or -1 if it never does. Returns a
# point within the grab reach of a line rather than the line itself, which is all
# a drag needs and is what a 6px step can guarantee: the reach is 8 logical
# pixels before scaling, so no line can fall between two samples.
function Find-LineX([IntPtr]$Hwnd, [int]$x0, [int]$x1, [int]$y, [IntPtr]$SizeWE) {
    for ($x = $x0; $x -le $x1; $x += 6) {
        if ((Showing-CursorAt $Hwnd $x $y) -eq $SizeWE) { return $x }
    }
    return -1
}

# A press-drag-release with the real cursor, in steps. The steps are the point:
# a drag delivered as one jump can leave the app showing a preview it never got
# a chance to tear down, and a drag it never saw the middle of is a drag that
# ends somewhere the user did not aim at.
function Real-Drag([IntPtr]$Hwnd, [int]$x0, [int]$y0, [int]$x1, [int]$y1, [switch]$Right) {
    $down = if ($Right) { 0x0008 } else { 0x0002 }   # RIGHTDOWN / LEFTDOWN
    $up   = if ($Right) { 0x0010 } else { 0x0004 }   # RIGHTUP   / LEFTUP
    $a = Client-XY $Hwnd $x0 $y0
    $b = Client-XY $Hwnd $x1 $y1
    [void][SliceUI.U]::SetCursorPos($a.X, $a.Y)
    Start-Sleep -Milliseconds 250
    [SliceUI.U]::mouse_event($down, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    $steps = 6
    for ($i = 1; $i -le $steps; $i++) {
        [void][SliceUI.U]::SetCursorPos([int]($a.X + ($b.X - $a.X) * $i / $steps),
                                       [int]($a.Y + ($b.Y - $a.Y) * $i / $steps))
        Start-Sleep -Milliseconds 120
    }
    Start-Sleep -Milliseconds 200
    [SliceUI.U]::mouse_event($up, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
}

# Clicks Export, answers the completion box with "no" so no Explorer is left
# behind, and hands back what is on disk. The folder is emptied first: the app
# numbers slices by index and never clears the folder, so one that held four
# slices and now holds three keeps the fourth, and counting files in it would
# measure a leftover rather than what this export actually wrote.
function Export-Cells([IntPtr]$Hwnd, [IntPtr]$Btn, [string]$Dir) {
    Get-ChildItem $Dir -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Invoke-Click $Btn
    Start-Sleep -Milliseconds 2500
    Close-Dialogs $Hwnd
    return @(Get-ChildItem $Dir -Filter '*.png' -ErrorAction SilentlyContinue)
}

# One line per exported file, in name order, as "WxH" - which is how every
# layout assertion below is phrased, because dimensions are the one thing about
# a slice that survives the trip to disk without being reinterpreted.
function Get-Sizes($files) {
    return @($files | Sort-Object Name | ForEach-Object {
        $im = [System.Drawing.Image]::FromFile($_.FullName)
        $t = "$($im.Width)x$($im.Height)"; $im.Dispose(); $t })
}

# --- the run ---------------------------------------------------------------

Write-Host "probe-slice-ui: $Exe"
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$src    = Join-Path $Work 'source.png'
$outDir = Join-Path $Work 'out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
New-Source $src $W $H

# Started with no arguments on purpose: the whole point is that the user has
# no file yet and has to pick one.
$script:app = Start-Process -FilePath $Exe -PassThru
$script:appId = $script:app.Id
$main = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $main -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 150
    if ($script:app.HasExited) { break }
    $m = @(Get-Windows -ProcId $script:appId -VisibleOnly |
           Where-Object { $_.Class -eq 'MuskSliceMain' })
    if ($m.Count) { $main = $m[0].H }
}
if ($main -eq [IntPtr]::Zero) {
    Bad 'the window never appeared'
    if (-not $script:app.HasExited) { $script:app.Kill() }
} else {
    Ok 'the window opened with no file and no arguments'

    $script:blocked = $false
    $openBtn = Get-ChildById $main 100
    if (-not $openBtn) {
        Bad 'the Open button is not in the window'
    } else {
        [void][SliceUI.U]::SetForegroundWindow($main)
        Start-Sleep -Milliseconds 300
        Invoke-Click $openBtn.H

        $dlg = Wait-Dialog $main 8
        if ($dlg -eq [IntPtr]::Zero) {
            # The click is the one thing this probe cannot fake, so this is
            # where a blocked desktop gets recognised - and it is recognised
            # from what the desktop looked like after the miss, not from a guess
            # made beforehand. Missing while the desktop is ours is a defect in
            # the button; missing while it is not ours is a skip.
            $why = Get-InputBlockReason $openBtn.H $script:appId
            if ($why) {
                Skip "the click on Open never reached the button - $why"
                Skip 'subdivide, browse and export checks did not run'
                $script:blocked = $true
            } else {
                Bad 'clicking Open put no file dialog on screen'
            }
        } else {
            Ok 'the Open button opens a file dialog'
            if (Set-DialogPath $dlg $src) {
                Start-Sleep -Milliseconds 1200
                Close-Dialogs $main
                $left = @(Get-Windows -ProcId $script:appId -VisibleOnly |
                          Where-Object { $_.Class -eq '#32770' })
                if ($left.Count) {
                    Bad 'the file dialog did not close when a file was chosen'
                } else {
                    $red = Count-Colour $main 255 0 0
                    $green = Count-Colour $main 0 255 0
                    if ($red -gt 20 -and $green -gt 20) {
                        Ok "the chosen file loaded and painted ($red red, $green green px)"
                    } else {
                        Bad "the canvas shows no source image (red $red, green $green)"
                    }
                }
            } else {
                Bad 'could not type a path into the file dialog'
                Close-Dialogs $main
            }
        }
    }

    # The crosshair over the picture. The canvas says "you can pick and draw
    # here" by switching to a cross cursor; a WM_SETCURSOR handler that takes
    # its position out of lParam (whose words are a hit-test code and a mouse
    # message id, never coordinates) shows the arrow everywhere instead, or a
    # crosshair in places that depend on the window size. GetCursorInfo is the
    # one honest outside view of that: it reports the cursor the desktop is
    # actually showing, which only the handler under test decided.
    if (-not $script:blocked -and [SliceUI.U]::IsWindowEnabled($main)) {
        [void][SliceUI.U]::SetForegroundWindow($main)
        Start-Sleep -Milliseconds 250
        $dpi   = [SliceUI.U]::GetDpiForWindow($main)
        $scale = $dpi / 96.0
        $rc = New-Object SliceUI.U+RECT
        [void][SliceUI.U]::GetClientRect($main, [ref]$rc)
        $cw = $rc.R - $rc.L; $ch = $rc.B - $rc.T
        $panelW  = [int](282 * $scale)      # build_layout's fixed panel width
        $statusH = [int](28 * $scale)

        # centre of the canvas: the fitted picture is centred in it, so this
        # point is on the image
        $pt = New-Object SliceUI.U+POINT
        $pt.X = [int](($cw - $panelW) / 2)
        $pt.Y = [int](($ch - $statusH) / 2)
        [void][SliceUI.U]::ClientToScreen($main, [ref]$pt)
        [void][SliceUI.U]::SetCursorPos($pt.X, $pt.Y)
        Start-Sleep -Milliseconds 400
        $ci = New-Object SliceUI.U+CURSORINFO
        $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ci)
        $cross = [SliceUI.U]::LoadCursorW([IntPtr]::Zero, 32515)   # IDC_CROSS
        $arrow = [SliceUI.U]::LoadCursorW([IntPtr]::Zero, 32512)   # IDC_ARROW
        if ([SliceUI.U]::GetCursorInfo([ref]$ci) -and ($ci.flags -band 1)) {
            if ($ci.hCursor -eq $cross) { Ok 'the cursor is a crosshair over the picture' }
            elseif ($ci.hCursor -eq $arrow) { Bad 'the cursor stays a plain arrow over the picture (WM_SETCURSOR never switches it)' }
            else { Bad "the cursor over the picture is an unexpected handle $($ci.hCursor)" }
        } else {
            Skip 'GetCursorInfo would not report the showing cursor'
        }

        # ...and back to the arrow over the control panel
        $pt = New-Object SliceUI.U+POINT
        $pt.X = $cw - [int]($panelW / 2)
        $pt.Y = [int]($ch * 0.08)
        [void][SliceUI.U]::ClientToScreen($main, [ref]$pt)
        [void][SliceUI.U]::SetCursorPos($pt.X, $pt.Y)
        Start-Sleep -Milliseconds 400
        $ci.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ci)
        if ([SliceUI.U]::GetCursorInfo([ref]$ci) -and ($ci.flags -band 1) -and
            $ci.hCursor -eq $arrow) {
            Ok 'the cursor goes back to the arrow over the panel'
        }
    }

    # The layout buttons. Each one is a WM_COMMAND carrying a real control id,
    # and every one of them was dead for the same reason the Open button was,
    # so a run that only ever opens a file on the command line proves nothing
    # about them. subdivide-all with divN = 2 turns the single slice into 2x2,
    # which is four exported files - one click, checkable on screen and on disk.
    if (-not $script:blocked -and [SliceUI.U]::IsWindowEnabled($main)) {
        # 按行均分 twice on a rows box reading 1 is a no-op, so the meaningful
        # press is the one that changes the tree: 细分全部 with divN = 2 turns
        # the single slice into 2x2, which is four exported files.
        $divAll = Get-ChildById $main 117
        if (-not $divAll) {
            Bad 'the subdivide-all button is not in the window'
        } else {
            Invoke-Click $divAll.H
            Start-Sleep -Milliseconds 400
            $blue = Count-Colour $main 0 0 255
            if ($blue -gt 20) { Ok 'a panel button changed the canvas (subdivide-all)' }
            else { Bad "subdivide-all left the canvas unchanged (blue $blue px)" }
        }

        # Export. The output folder is the default the app fills in beside the
        # source, which is the ordinary path and needs nothing driven from
        # outside. Note what is deliberately NOT done here: the folder box is
        # not written with WM_SETTEXT and read back with WM_GETTEXT. Setting it
        # half-works and reading it back across a process boundary does not work
        # at all, so that combination reports success while the app goes on
        # using the old path - a check like that is worse than no check.
        $browse = Get-ChildById $main 101
        $expBtn = Get-ChildById $main 102
        if (-not $browse -or -not $expBtn) {
            Bad 'the Browse or Export button is missing'
        } else {
            Invoke-Click $browse.H
            $fdlg = Wait-Dialog $main 8
            if ($fdlg -eq [IntPtr]::Zero) {
                Bad 'clicking Browse put no folder dialog on screen'
            } else {
                Ok 'the Browse button opens a folder dialog'
                [void][SliceUI.U]::PostMessageW($fdlg, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
                Start-Sleep -Milliseconds 600
            }

            Invoke-Click $expBtn.H
            Start-Sleep -Milliseconds 2500
            Close-Dialogs $main

            $made = Join-Path $Work 'source_slices'
            $files = @(Get-ChildItem $made -Filter '*.png' -ErrorAction SilentlyContinue)
            if ($files.Count -ne 4) {
                Bad "the Export button wrote $($files.Count) files, expected 4"
            } else {
                $sizes = @($files | Sort-Object Name | ForEach-Object {
                    $im = [System.Drawing.Image]::FromFile($_.FullName)
                    $t = "$($im.Width)x$($im.Height)"; $im.Dispose(); $t })
                if (($sizes -join ',') -eq '120x60,120x60,120x60,120x60') {
                    Ok 'the Export button wrote the 2x2 grid as four 120x60 files'
                } else {
                    Bad "the exported sizes are $($sizes -join ', '), expected four 120x60"
                }
                $c = New-Object System.Drawing.Bitmap $files[3].FullName
                $px = $c.GetPixel(5, 5); $c.Dispose()
                if ($px.R -gt 200 -and $px.G -gt 200 -and $px.B -gt 200) {
                    Ok 'the fourth file is the bottom-right quadrant, not a copy of the first'
                } else {
                    Bad "file 4 is R$($px.R) G$($px.G) B$($px.B), expected white"
                }
            }
        }
    }

    # The two canvas gestures: dragging a grid line, and box-selecting cells to
    # merge. Both are recent, and both fail silently when they fail - a press
    # that misses the line does not complain, it draws a cut instead, and the
    # user is left dragging something that does nothing while their picture
    # quietly gains a slice. Nothing on screen says "wrong gesture", so the only
    # honest check is what comes out the other end.
    if (-not $script:blocked -and [SliceUI.U]::IsWindowEnabled($main)) {
        $expBtn2  = Get-ChildById $main 102
        $mergeBtn = Get-ChildById $main 119
        if (-not $expBtn2 -or -not $mergeBtn) {
            Bad 'the Export or Merge button is missing'
        } else {
            $sizeWE = [SliceUI.U]::LoadCursorW([IntPtr]::Zero, 32644)   # IDC_SIZEWE
            $sizeNS = [SliceUI.U]::LoadCursorW([IntPtr]::Zero, 32646)   # IDC_SIZENS
            $cross  = [SliceUI.U]::LoadCursorW([IntPtr]::Zero, 32515)   # IDC_CROSS
            $rc2 = New-Object SliceUI.U+RECT
            [void][SliceUI.U]::GetClientRect($main, [ref]$rc2)
            $canvasW = $rc2.R - $rc2.L
            $canvasH = $rc2.B - $rc2.T

            [void][SliceUI.U]::SetForegroundWindow($main)
            Start-Sleep -Milliseconds 300

            # Where the lines are is looked for rather than calculated. Working
            # it out would mean re-implementing build_layout and the canvas fit
            # in PowerShell, and build_layout scales off app_scale_px(1) - which
            # rounds 1.25 down to 1, so at 125% the panel is 282px and not 352.
            # Getting that wrong points the cursor 35px away from the line, which
            # from out here is indistinguishable from the line being ungrabbable.
            # Stepping 6px cannot skip a line, because the grab reach is 8
            # logical pixels before scaling anything.
            $lineX = Find-LineX $main 4 ($canvasW - 4) ([int]($canvasH / 2)) $sizeWE
            if ($lineX -lt 0) {
                Bad 'no vertical grid line anywhere in the picture offers a resize cursor'
            } else {
                $lineY = [int]($canvasH / 2)
                Ok "a vertical grid line offers a resize cursor (client x=$lineX)"

                # The middle of a cell must still offer a plain cut. Without
                # this, a reach wide enough to grab everything would pass the
                # checks below by grabbing everything. 60px out is several times
                # the 8-logical-pixel reach even at 200% scaling.
                $midX = $lineX - 60
                $h = Showing-CursorAt $main $midX $lineY
                if ($h -eq $sizeWE) {
                    Bad 'a point well inside a cell still offers a line - the grab reach has swallowed the cell'
                } else {
                    # Any cursor other than the resize one satisfies the point
                    # being made here, so this does not have to recognise every
                    # cursor the desktop might be showing to judge it.
                    Ok "a point well inside a cell offers no line (cursor $h)"
                }

                # Drag the line to the right. A drag that drew a cut instead would
                # not leave four files behind, it would leave six, because the
                # cut would split two more cells - and it would leave four equal
                # ones. So the count and the widths together say which gesture
                # actually ran, which is the whole point of checking here.
                $dragPx = 40
                Real-Drag $main $lineX $lineY ($lineX + $dragPx) $lineY

                $made  = Join-Path $Work 'source_slices'
                $files = Export-Cells $main $expBtn2.H $made
                if ($files.Count -ne 4) {
                    Bad "after dragging a grid line there are $($files.Count) files, expected 4 - a drag that cuts would leave more"
                } else {
                    $sizes = Get-Sizes $files
                    $ws = @($sizes | ForEach-Object { [int](($_ -split 'x')[0]) } | Sort-Object -Unique)
                    $hs = @($sizes | ForEach-Object { [int](($_ -split 'x')[1]) } | Sort-Object -Unique)
                    if ($ws.Count -eq 2 -and ($ws[0] + $ws[1]) -eq $W -and $hs.Count -eq 1 -and $hs[0] -eq 60) {
                        Ok "the dragged line moved instead of cutting ($($sizes -join ', '))"
                    } else {
                        Bad "the drag left the columns at $($sizes -join ', '), expected two widths summing to $W, all 60 tall"
                    }
                }

                # Now the merge. The band goes down the left column with the
                # right button - the left one is already spoken for by picking a
                # cell and drawing a cut. It is drawn a long way off the row line
                # on both sides, because what matters is which cells it touches:
                # a band that starts below the row line selects one cell, and one
                # cell cannot be merged with anything. It is also nowhere near the
                # column edge, because a box a person drags never lands on a cell
                # edge and the app has to snap it.
                $colX = $lineX + $dragPx - 40
                $quarter = [int]($canvasH / 4)
                Real-Drag $main $colX ($lineY - $quarter) $colX ($lineY + $quarter) -Right
                Invoke-Click $mergeBtn.H
                Start-Sleep -Milliseconds 500
                $files = Export-Cells $main $expBtn2.H $made
                if ($files.Count -ne 3) {
                    Bad "after merging two cells there are $($files.Count) files, expected 3"
                } else {
                    $sizes = Get-Sizes $files
                    $tall = @($sizes | Where-Object { [int](($_ -split 'x')[1]) -eq 120 })
                    if ($tall.Count -eq 1) {
                        Ok "the box-selected cells merged into one ($($sizes -join ', '))"
                    } else {
                        Bad "no single full-height slice after the merge ($($sizes -join ', '))"
                    }
                }
            }
        }
    }

    if (-not $script:app.HasExited) { $script:app.Kill() }
    $script:app.WaitForExit(3000) | Out-Null
}

if ($script:failures) {
    Write-Host "RESULT: slice UI probe FAILED ($script:failures problem(s), $script:skipped skipped)" -ForegroundColor Red
    exit 1
}
if (-not $Keep) { Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue }
if ($script:skipped) {
    Write-Host "RESULT: slice UI probe passed - real-input checks skipped" -ForegroundColor Green
} else {
    Write-Host "RESULT: slice UI probe passed" -ForegroundColor Green
}
exit 0
