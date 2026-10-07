# probe-picker-wheel.ps1 - black-box check that the picker's colour wheel, the
# harmony panel, the palette tabs and the wheel key bindings actually respond to
# input, and that doing so does not leak or crash the process.
#
#   .\test\probe-picker-wheel.ps1 [-Exe .\bin\picker.exe]
#
# The picker is a single self-painted window: no child controls, nothing that
# another process can read back. So every assertion here is made the way a user
# would see it - a screenshot, a click, another screenshot. The big swatch in
# the header is filled with the current colour, which makes it a one-pixel
# readout of the app's whole state, and the harmony row doubles as an oracle:
# slot 0 is defined to be the current colour, so clicking it must leave the
# header showing exactly that swatch.
#
# Clicks are posted straight to the window, so the probe never steals the
# cursor and never depends on what is under it.
#
# ASCII-only like the rest of the repo (see extract-article-themes.ps1).

[CmdletBinding()]
param([string]$Exe)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class PK {
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
 [DllImport("user32.dll")] public static extern bool GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr rgn, uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
 public delegate bool EnumProc(IntPtr h, IntPtr p);
}
"@ -ReferencedAssemblies System.Drawing, System.Windows.Forms

# Windows PowerShell is DPI-unaware by default, so every window coordinate it
# reads back is virtualised (divided by the scale) while PrintWindow still
# draws the window at its true physical size. Posting a click built from those
# numbers would land ~20% away from the intended widget, so claim awareness
# before touching a single window API.
[void][PK]::SetProcessDpiAwarenessContext([IntPtr](-4))
[void][PK]::SetProcessDPIAware()

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $root '..\bin\picker.exe' }
if (-not (Test-Path $Exe)) { throw "picker.exe not found at $Exe (build it first)" }
$Exe = (Resolve-Path $Exe).Path

# --- the app's own layout constants (src/picker/picker.c) -------------------
# S is app_scale_px(1) at 120 DPI, i.e. MulDiv(1,120,96) = 1.
$S       = 1
$WHEEL_BOX  = 144
$WHEEL_RING = 16
$SW_CELL_W  = 22
$SW_CELL_H  = 18
$SW_GAP     = 3
$PAD        = 10 * $S
$GAP        = 6 * $S

$WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
$WM_MOUSEWHEEL  = 0x020A; $WM_KEYDOWN = 0x0100
$MK_LBUTTON     = 0x0001
$RDW_INVALIDATE = 0x0001; $RDW_UPDATENOW = 0x0100; $RDW_ALLCHILDREN = 0x0080
$VK_RIGHT = 0x27; $VK_LEFT = 0x25; $VK_ADD = 0x6B

function Get-ClassName([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][PK]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}

# --- launch -----------------------------------------------------------------
$proc = Start-Process -FilePath $Exe -PassThru
$main = [IntPtr]::Zero
$fail = 0
$skip = 0
try {
    for ($i = 0; $i -lt 60 -and $main -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        $cb = [PK+EnumProc] {
            param($hw, $x)
            $pid2 = 0
            [void][PK]::GetWindowThreadProcessId($hw, [ref]$pid2)
            if ($pid2 -eq $proc.Id -and (Get-ClassName $hw) -eq 'MuskPickerMain') {
                $script:main = $hw
                return $false
            }
            return $true
        }
        [void][PK]::EnumWindows($cb, [IntPtr]::Zero)
    }
    if ($main -eq [IntPtr]::Zero) { throw "main window never appeared (pid $($proc.Id))" }
    [void][PK]::SetForegroundWindow($main)
    Start-Sleep -Milliseconds 400

    # client origin inside the captured window bitmap
    $wr = New-Object PK+RECT; $cr = New-Object PK+RECT
    [void][PK]::GetWindowRect($main, [ref]$wr)
    [void][PK]::GetClientRect($main, [ref]$cr)
    $org = New-Object PK+POINT
    [void][PK]::ClientToScreen($main, [ref]$org)
    $offX = $org.X - $wr.L
    $offY = $org.Y - $wr.T
    $cw = $cr.R - $cr.L
    $ch = $cr.B - $cr.T
    Write-Host ("main=0x{0:X} dpi={1} window={2}x{3} client={4}x{5} clientOriginInShot={6},{7}" -f `
        $main.ToInt64(), [PK]::GetDpiForWindow($main), ($wr.R-$wr.L), ($wr.B-$wr.T), $cw, $ch, $offX, $offY)

    function Get-Shot {
        $bmp = New-Object System.Drawing.Bitmap ($wr.R - $wr.L), ($wr.B - $wr.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][PK]::PrintWindow($main, $hdc, 2)
        $g.ReleaseHdc($hdc)
        $g.Dispose()
        return $bmp
    }
    function Px($bmp, [int]$cx, [int]$cy) {
        $p = $bmp.GetPixel($cx + $offX, $cy + $offY)
        return @($p.R, $p.G, $p.B)
    }
    # The header swatch has its hex drawn across it and the swatch cells carry
    # a selection ring, so a single pixel can land on a glyph or a border. The
    # fill covers most of the rectangle, so its modal colour is the one the app
    # actually painted - that is the value under test.
    function Get-Dominant($bmp, [int]$x0, [int]$y0, [int]$x1, [int]$y1) {
        $tally = @{}
        $best = $null; $bestN = 0
        for ($y = $y0; $y -le $y1; $y++) {
            for ($x = $x0; $x -le $x1; $x++) {
                $p = $bmp.GetPixel($x + $offX, $y + $offY)
                $k = '{0},{1},{2}' -f $p.R, $p.G, $p.B
                if ($tally.ContainsKey($k)) { $tally[$k]++ } else { $tally[$k] = 1 }
                if ($tally[$k] -gt $bestN) { $bestN = $tally[$k]; $best = @($p.R, $p.G, $p.B) }
            }
        }
        return $best
    }
    function Same-Rgb($a, $b, [int]$tol = 4) {
        ([Math]::Abs($a[0] - $b[0]) -le $tol) -and
        ([Math]::Abs($a[1] - $b[1]) -le $tol) -and
        ([Math]::Abs($a[2] - $b[2]) -le $tol)
    }
    function Hex($c) { return ('#{0:X2}{1:X2}{2:X2}' -f $c[0], $c[1], $c[2]) }
    function Click([int]$cx, [int]$cy) {
        $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
        [void][PK]::PostMessageW($main, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp)
        Start-Sleep -Milliseconds 60
        [void][PK]::PostMessageW($main, $WM_LBUTTONUP, [IntPtr]::Zero, $lp)
        [void][PK]::RedrawWindow($main, [IntPtr]::Zero, [IntPtr]::Zero,
            ($RDW_INVALIDATE -bor $RDW_UPDATENOW -bor $RDW_ALLCHILDREN))
        Start-Sleep -Milliseconds 120
    }
    function Key([int]$vk) {
        [void][PK]::PostMessageW($main, $WM_KEYDOWN, [IntPtr]$vk, [IntPtr]::Zero)
        [void][PK]::RedrawWindow($main, [IntPtr]::Zero, [IntPtr]::Zero,
            ($RDW_INVALIDATE -bor $RDW_UPDATENOW -bor $RDW_ALLCHILDREN))
        Start-Sleep -Milliseconds 120
    }
    function Scroll([int]$cx, [int]$cy) {
        $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
        [void][PK]::PostMessageW($main, $WM_MOUSEWHEEL, [IntPtr](-120), $lp)
        [void][PK]::RedrawWindow($main, [IntPtr]::Zero, [IntPtr]::Zero,
            ($RDW_INVALIDATE -bor $RDW_UPDATENOW -bor $RDW_ALLCHILDREN))
        Start-Sleep -Milliseconds 150
    }
    function Region-Hash($bmp, [int]$x0, [int]$y0, [int]$x1, [int]$y1) {
        $acc = 0
        for ($y = $y0; $y -le $y1; $y += 2) {
            for ($x = $x0; $x -le $x1; $x += 2) {
                $p = $bmp.GetPixel($x + $offX, $y + $offY)
                $acc = ($acc * 31 + $p.R * 7 + $p.G * 13 + $p.B * 17) -band 0x7FFFFFFF
            }
        }
        return $acc
    }

    # header swatch: g_lay.big = (PAD, PAD) size 110x62, filled with the colour
    $bigX = $PAD; $bigY = $PAD; $bigR = $PAD + 110; $bigB = $PAD + 62
    function Read-Big($bmp) { return Get-Dominant $bmp $bigX $bigY $bigR $bigB }
    # colour wheel box: (PAD, PAD + 62 + 34 + GAP), 144x144. The 34 is the
    # readout band under the swatch (2px margin + two 16px text lines) - the
    # RGB values and the nearest-name line live there, and the wheel starts
     # below them since the backdrop stopped wiping that band.
    $wbX = $PAD; $wbY = $PAD + 62 + 34 + $GAP
    $cx0 = $wbX + [int]($WHEEL_BOX / 2)
    $cy0 = $wbY + [int]($WHEEL_BOX / 2)
    $rOut = $WHEEL_BOX / 2.0 - 1.0
    $rIn  = $rOut - $WHEEL_RING
    $rMid = ($rOut + $rIn) / 2.0

    $shot = Get-Shot
    Write-Host ""
    Write-Host "--- the hue ring drives the current colour ---"
    # hue 0 sits at 12 o'clock and grows clockwise, so angle a is
    # x = c + r*sin(a), y = c - r*cos(a).
    $seen = @{}
    foreach ($ang in 0, 90, 210, 300) {
        $a = $ang * [Math]::PI / 180.0
        $px = [int][Math]::Round($cx0 + $rMid * [Math]::Sin($a))
        $py = [int][Math]::Round($cy0 - $rMid * [Math]::Cos($a))
        Click $px $py
        $c = Read-Big (Get-Shot)
        $seen[$ang] = (Hex $c)
        Write-Host ("  hue {0,3} deg at client {1},{2} -> {3}" -f $ang, $px, $py, (Hex $c))
    }
    $distinct = ($seen.Values | Sort-Object -Unique).Count
    if ($distinct -ne 4) {
        Write-Host "  FAIL: four ring positions produced $distinct distinct colours" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  OK: four ring positions give four distinct colours" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "--- the same ring position always gives the same colour ---"
    $first = $seen[0]
    $a = 0.0
    Click ([int][Math]::Round($cx0 + $rMid * [Math]::Sin($a))) ([int][Math]::Round($cy0 - $rMid * [Math]::Cos($a)))
    $again = Hex (Read-Big (Get-Shot))
    if ($again -ne $first) {
        Write-Host "  FAIL: hue 0 gave $first then $again on a repeat click" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  OK: repeat click on hue 0 reproduces $again" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "--- the S/V triangle changes the colour too ---"
    $triHalf = $rIn * 0.8660254
    $tTop = $cy0 - $rIn
    $tBot = $cy0 + $rIn * 0.5
    Click ([int][Math]::Round($cx0)) ([int][Math]::Round($tTop + ($tBot - $tTop) * 0.25))
    $cTri = Read-Big (Get-Shot)
    Write-Host ("  triangle 25% down -> {0}" -f (Hex $cTri))
    if ($cTri[0] -eq $first.Substring(1,2)) { }
    if ((Hex $cTri) -eq $first) {
        Write-Host "  FAIL: clicking inside the triangle left the colour untouched" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  OK: the triangle is live" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "--- harmony slot 0 is the current colour (the round trip) ---"
    # harmony swatches share the width left over by the tabs: 5 across, starting
    # just right of the wheel box.
    $swX0 = $wbX + $WHEEL_BOX + $GAP
    $swAvail = $cw - $PAD - $swX0
    $cellW = [int](($swAvail - $S * 4) / 5)
    $swY0 = $wbY + 21 * $S + 5 * $S
    $slot0X = $swX0 + [int]($cellW / 2)
    $slot0Y = $swY0 + [int](44 * $S / 2)
    $shot = Get-Shot
    $want = Get-Dominant $shot $swX0 $swY0 ($swX0 + $cellW) ($swY0 + 44)
    Write-Host ("  slot0 centre {0},{1} shows {2}" -f $slot0X, $slot0Y, (Hex $want))
    Click $slot0X $slot0Y
    $got = Read-Big (Get-Shot)
    Write-Host ("  after clicking it the header shows {0}" -f (Hex $got))
    if (-not (Same-Rgb $want $got)) {
        Write-Host "  FAIL: slot 0 must BE the current colour" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  OK: the app agrees that slot 0 is the colour it is showing" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "--- switching scheme recomputes the harmony row ---"
    $tabY = $wbY + [int](21 * $S / 2)
    $shot = Get-Shot
    $before = Region-Hash $shot $swX0 $swY0 ($swX0 + 5 * ($cellW + 4)) ($swY0 + 44)
    # the tabs are laid out left to right; the third one is at least 2 tabs in
    $tab3X = $swX0 + 2 * 70
    Click $tab3X $tabY
    $shot = Get-Shot
    $after = Region-Hash $shot $swX0 $swY0 ($swX0 + 5 * ($cellW + 4)) ($swY0 + 44)
    if ($before -eq $after) {
        Write-Host "  FAIL: clicking a scheme tab left the harmony row identical" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  OK: the harmony row follows the selected scheme" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "--- palette tabs switch the swatch grid ---"
    # Tab pills are #F3F3F5 on a #FFFFFF client, so they can be found without
    # knowing the font metrics: scan every row for runs of the pill colour.
    # Sixteen palettes wrap onto three rows, and the overflowing ones (the web
    # palette among them) only sit on the lower rows, so all rows are needed.
    $rows = @{}
    $shot = Get-Shot
    for ($y = 120; $y -lt 330; $y++) {
        $run = 0; $start = -1
        for ($x = [int]($cw / 2); $x -lt $cw - 4; $x++) {
            $p = $shot.GetPixel($x + $offX, $y + $offY)
            $isTab = ([Math]::Abs($p.R - 243) -le 2) -and ([Math]::Abs($p.G - 243) -le 2) -and ([Math]::Abs($p.B - 245) -le 2)
            if ($isTab) {
                if ($run -eq 0) { $start = $x }
                $run++
            } else {
                if ($run -ge 22) {
                    $key = "$y"
                    if (-not $rows.ContainsKey($key)) { $rows[$key] = @() }
                    $rows[$key] += ,([int]($start + $run / 2))
                }
                $run = 0
            }
        }
        if ($run -ge 22) {
            $key = "$y"
            if (-not $rows.ContainsKey($key)) { $rows[$key] = @() }
            $rows[$key] += ,([int]($start + $run / 2))
        }
    }
    # one point per pill, and only one scan line per pill row
    $pts = @()
    $lastY = -99
    foreach ($y in ($rows.Keys | Sort-Object { [int]$_ })) {
        if (([int]$y) - $lastY -lt 14) { continue }
        $lastY = [int]$y
        foreach ($cx in ($rows[$y] | Sort-Object -Unique)) {
            $dup = $false
            foreach ($q in $pts) { if ([Math]::Abs($q[0] - $cx) -lt 18 -and [Math]::Abs($q[1] - $y) -lt 14) { $dup = $true } }
            if (-not $dup) { $pts += ,@($cx, [int]$y) }
        }
    }
    Write-Host ("  found {0} palette tab pills across the rows" -f $pts.Count)
    if ($pts.Count -lt 2) {
        Write-Host "  FAIL: could not locate the palette tabs" -ForegroundColor Red
        $fail++
    } else {
        $gx0 = [int]($cw / 2); $gx1 = $cw - 8
        $lastTabY = 0
        foreach ($q in $pts) { if ($q[1] -gt $lastTabY) { $lastTabY = $q[1] } }
        $gy0 = $lastTabY + 34; $gy1 = $gy0 + 150
        $shot = Get-Shot
        $g0 = Region-Hash $shot $gx0 $gy0 $gx1 $gy1
        Click $pts[1][0] $pts[1][1]
        $shot = Get-Shot
        $g1 = Region-Hash $shot $gx0 $gy0 $gx1 $gy1
        if ($g0 -eq $g1) {
            Write-Host "  FAIL: clicking a different palette tab did not change the grid" -ForegroundColor Red
            $fail++
        } else {
            Write-Host "  OK: the grid follows the selected palette" -ForegroundColor Green
        }

        # The web palette is far larger than the grid, so it is the one that
        # needs the scroll indicator. Walk the tabs until one overflows.
        Write-Host ""
        Write-Host "--- the wheel scrolls an overflowing palette ---"
        $scrolled = $false
        foreach ($q in $pts) {
            Click $q[0] $q[1]
            $shot = Get-Shot
            $h0 = Region-Hash $shot $gx0 $gy0 $gx1 $gy1
            # a scrollbar thumb is a grey #9CA3AF bar near the right edge
            $bar = $false
            for ($x = $cw - 18; $x -lt $cw - 4 -and -not $bar; $x++) {
                $run = 0
                for ($y = $gy0; $y -le $gy1; $y += 2) {
                    $p = $shot.GetPixel($x + $offX, $y + $offY)
                    if ([Math]::Abs($p.R - 156) -le 12 -and [Math]::Abs($p.G - 163) -le 12 -and [Math]::Abs($p.B - 175) -le 12) { $run++ }
                }
                if ($run -ge 20) { $bar = $true }
            }
            if (-not $bar) { continue }
            Scroll ([int]($cw / 2)) ([int]($gy0 + 40))
            $shot = Get-Shot
            $h1 = Region-Hash $shot $gx0 $gy0 $gx1 $gy1
            if ($h0 -ne $h1) {
                Write-Host ("  OK: palette at tab x={0} scrolls and repaints" -f $q[0]) -ForegroundColor Green
                $scrolled = $true
            } else {
                Write-Host ("  FAIL: an overflowing palette did not move on the wheel" -f $q[0]) -ForegroundColor Red
                $fail++
            }
            break
        }
        if (-not $scrolled) {
            Write-Host "  SKIP: none of the palettes overflow the grid, so there is nothing to scroll" -ForegroundColor Yellow
            $skip++
        }
    }

    Write-Host ""
    Write-Host "--- keyboard nudges the colour ---"
    # A hue step of one degree is genuinely invisible on a near-grey colour -
    # with saturation under ~0.2 a full degree moves the green and blue channels
    # by less than one level, which rounds back to the same byte. Start from a
    # colour picked off the ring so the keys are tested where they mean
    # something, and keep the round trip as the real assertion.
    Click ([int][Math]::Round($cx0 + $rMid * [Math]::Sin(0))) ([int][Math]::Round($cy0 - $rMid * [Math]::Cos(0)))
    $shot = Get-Shot
    $k0 = Hex (Read-Big $shot)
    Write-Host ("  starting from a saturated {0}" -f $k0)
    Key $VK_RIGHT
    $k1 = Hex (Read-Big (Get-Shot))
    Key $VK_LEFT
    $k2 = Hex (Read-Big (Get-Shot))
    Write-Host ("  right -> {0}, left -> {1}" -f $k1, $k2)
    if ($k0 -eq $k1) {
        Write-Host "  FAIL: the right arrow did not change the colour" -ForegroundColor Red
        $fail++
    } elseif ($k1 -eq $k2) {
        Write-Host "  FAIL: the left arrow did not undo the right arrow" -ForegroundColor Red
        $fail++
    } elseif ($k0 -ne $k2) {
        Write-Host "  FAIL: the round trip landed on $k2, not back on $k0" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  OK: arrow keys walk the hue and come back" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "--- the process survived all of it ---"
    $proc.Refresh()
    if ($proc.HasExited) {
        Write-Host ("  FAIL: picker.exe exited with {0}" -f $proc.ExitCode) -ForegroundColor Red
        $fail++
    } else {
        # One throwaway round before the baseline. GDI+ and Win32 grow their
        # lazy caches (fonts, DIBs, misc handles) over the first few
        # interactions, so a count taken cold reads that warm-up as a leak -
        # 183 -> 229 once, then flat forever. The app's own self-tests take
        # their baselines after a warm-up round for exactly this reason.
        Click $slot0X $slot0Y
        $s = Get-Shot; $s.Dispose()
        Start-Sleep -Milliseconds 200
        $proc.Refresh()
        $gdi = [int]$proc.HandleCount
        Write-Host ("  alive, pid {0}, handle count {1}" -f $proc.Id, $gdi)
        # One more full round through every surface: if any of the cached
        # bitmaps or the memory DC were being recreated per repaint the GDI
        # object count would climb instead of staying flat.
        for ($k = 0; $k -lt 12; $k++) {
            Click ([int][Math]::Round($cx0 + $rMid * [Math]::Sin($k))) ([int][Math]::Round($cy0))
            Click $slot0X $slot0Y
            $s = Get-Shot
            $s.Dispose()
        }
        $proc.Refresh()
        $gdi2 = [int]$proc.HandleCount
        Write-Host ("  handle count after 24 more clicks: {0}" -f $gdi2)
        if ($gdi2 -gt $gdi + 8) {
            Write-Host "  FAIL: GDI handles grew from $gdi to $gdi2" -ForegroundColor Red
            $fail++
        } else {
            Write-Host "  OK: no handle growth across repeated interaction" -ForegroundColor Green
        }
    }

    $shot.Dispose()
    Write-Host ""
    if ($fail -eq 0) {
        Write-Host "RESULT: the picker responds to wheel, harmony, palette and keyboard input" -ForegroundColor Green
    } else {
        Write-Host "RESULT: $fail check(s) failed" -ForegroundColor Red
    }
    if ($skip -gt 0) { Write-Host "($skip check(s) skipped)" -ForegroundColor Yellow }
    exit $(if ($fail -eq 0) { 0 } else { 1 })
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}
