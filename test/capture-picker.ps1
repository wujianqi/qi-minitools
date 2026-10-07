# capture-picker.ps1 - screenshots the running picker's own window.
#
#   .\capture-picker.ps1                    # default window capture
#   .\capture-picker.ps1 -Click 3           # also click client point 3 first
#
# PrintWindow renders the *whole* window, frame included, so the bitmap has to
# be sized from GetWindowRect. Sizing it from GetClientRect instead silently
# squeezes and offsets everything, which looks exactly like a layout bug.
#
# The script must also be DPI aware. Windows PowerShell 5.1 runs DPI-unaware,
# so GetWindowRect hands back *virtualised* coordinates (divided by the scale
# factor) while PrintWindow still draws the window at its true physical size.
# The bitmap is then built 1/scale too small and the capture loses that much of
# the right and bottom edges - which reads as the app clipping its own layout.
# Declaring awareness first makes both sides agree on physical pixels.
#
# ASCII-only like the rest of the repo (see extract-article-themes.ps1).

[CmdletBinding()]
param(
    [string]$Out = 'capture.png',
    [int[]]$Click = @(),
    [switch]$Wheel
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class Cap {
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, IntPtr e);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
}
"@ -ReferencedAssemblies System.Drawing, System.Windows.Forms

# Must run before any window call so every coordinate below is physical.
# PER_MONITOR_AWARE_V2 = -4; fall back to the old system-wide call.
[void][Cap]::SetProcessDpiAwarenessContext([IntPtr](-4))
[void][Cap]::SetProcessDPIAware()

$proc = Get-Process picker -ErrorAction SilentlyContinue
if (-not $proc) { throw "picker.exe is not running" }
$h = [IntPtr]$proc.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "picker.exe has no main window yet" }
[void][Cap]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 500

$cr = New-Object Cap+RECT
[void][Cap]::GetClientRect($h, [ref]$cr)
"dpi=$([Cap]::GetDpiForWindow($h))  client=$($cr.R - $cr.L) x $($cr.B - $cr.T)  visible=$([Cap]::IsWindowVisible($h))"

function Save-Shot([string]$path) {
    $wr = New-Object Cap+RECT
    [void][Cap]::GetWindowRect($h, [ref]$wr)
    $w = $wr.R - $wr.L
    $ht = $wr.B - $wr.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [Cap]::PrintWindow($h, $hdc, 2)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    "saved $path  (${w}x${ht}, PrintWindow=$ok)"
}

function Send-Click([int]$cx, [int]$cy) {
    $p = New-Object Cap+POINT
    $p.X = $cx; $p.Y = $cy
    [void][Cap]::ClientToScreen($h, [ref]$p)
    [void][Cap]::SetCursorPos($p.X, $p.Y)
    Start-Sleep -Milliseconds 120
    [Cap]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
    Start-Sleep -Milliseconds 80
    [Cap]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
    "click client $cx,$cy"
    Start-Sleep -Milliseconds 250
}

foreach ($c in $Click) { Send-Click $c }
if ($Wheel) {
    $p = New-Object Cap+POINT
    $p.X = 100; $p.Y = 300
    [void][Cap]::ClientToScreen($h, [ref]$p)
    [void][Cap]::SetCursorPos($p.X, $p.Y)
    Start-Sleep -Milliseconds 200
    [Cap]::mouse_event(0x0800, 0, 0, [uint32]4294967295, [IntPtr]::Zero)  # WHEEL down
    "wheel down"
    Start-Sleep -Milliseconds 300
}

Save-Shot (Join-Path $PSScriptRoot "..\build\$Out")
