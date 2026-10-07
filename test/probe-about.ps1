# probe-about.ps1 - opens the new About box for real and photographs it.
#   .\test\probe-about.ps1 [slice|envtool|picker]
#
# The unit checks in --selftest cannot see this: it is about whether the button
# is where a reader would look, whether the link is drawn where it can be
# clicked, and whether the box lands in front of the window that opened it.
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr p);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
}
'@
$ErrorActionPreference = 'Stop'

# The four programs are per-monitor DPI aware, so their windows are larger in
# real pixels than in the coordinates a DPI-unaware PowerShell is handed. Left
# unfixed, GetWindowRect returns the *virtualised* size and PrintWindow draws a
# 1:1 bitmap into it - which photographs the top-left corner of the window and
# makes a perfectly good dialog look as though its bottom had been cut off.
[void][W]::SetProcessDPIAware()

function Find-Win([string]$cls, [string]$text) {
  # script scope on purpose: the callback runs outside this function's scope,
  # and it has to be cleared first or a miss hands back the previous answer.
  $script:found = [IntPtr]::Zero
  $cb = [W+EnumProc]{
    param($h, $p)
    $sb = New-Object System.Text.StringBuilder 128
    [void][W]::GetClassName($h, $sb, 128)
    if ($cls -and $sb.ToString() -ne $cls) { return $true }
    if ($text) {
      $t = New-Object System.Text.StringBuilder 256
      [void][W]::GetWindowText($h, $t, 256)
      if ($t.ToString() -notlike "*$text*") { return $true }
    }
    $script:found = $h
    return $false
  }
  [void][W]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:found
}

function Find-Child([IntPtr]$parent, [string]$cls, [string]$text) {
  $script:kid = [IntPtr]::Zero
  $cb = [W+EnumProc]{
    param($h, $p)
    $sb = New-Object System.Text.StringBuilder 128
    [void][W]::GetClassName($h, $sb, 128)
    if ($cls -and $sb.ToString() -ne $cls) { return $true }
    $t = New-Object System.Text.StringBuilder 256
    [void][W]::GetWindowText($h, $t, 256)
    if ($text -and $t.ToString() -replace '\s', '' -ne ($text -replace '\s','')) { return $true }
    $script:kid = $h
    return $false
  }
  [void][W]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
  return $script:kid
}

function Save-Win([IntPtr]$h, [string]$path) {
  $r = New-Object W+RECT
  [void][W]::GetWindowRect($h, [ref]$r)
  $w = $r.R - $r.L; $ht = $r.B - $r.T
  if ($w -le 0 -or $ht -le 0) { return "no rect for $h" }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  # 2 = PW_RENDERFULLCONTENT, which is what a layered/GDI+ window needs
  [void][W]::PrintWindow($h, $dc, 2)
  $g.ReleaseHdc($dc)
  $g.Dispose()
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  return "saved $path ($w x $ht)"
}

$which = if ($args.Count) { $args[0] } else { 'slice' }
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$top  = Split-Path -Parent $root
$exe  = Join-Path $top "bin\$which.exe"
# the window classes are not named after the .exe files
$class = @{ 'slice' = 'MuskSliceMain'; 'envtool' = 'MuskEnvTool';
            'picker' = 'MuskPickerMain' }[$which]
if (-not $class) { throw "unknown program '$which' (expected slice, envtool or picker)" }
Write-Host "probing $exe (class $class)"

$p = Start-Process -FilePath $exe -PassThru
$main = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $main -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 250
  $main = Find-Win $class $null
}
if ($main -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id -Force; throw "no $which window appeared" }
Write-Host "  main window: $main"

$btn = Find-Child $main 'Button' '关于'
if ($btn -ne [IntPtr]::Zero) {
  $br = New-Object W+RECT
  [void][W]::GetWindowRect($btn, [ref]$br)
  Write-Host ("  关于 button at {0},{1} {2}x{3}" -f $br.L, $br.T, ($br.R-$br.L), ($br.B-$br.T))
  Write-Host ("  -> {0}" -f (Save-Win $main (Join-Path $top "build\about-$which-window.png")))
  # BM_CLICK, posted: SendMessage would not return until the modal box closed
  [void][W]::PostMessage($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
} else {
  # envtool draws its own buttons, so there is no child window to press: click
  # where build_layout puts 关于 - the far end of the top toolbar row. Two
  # candidates, because the row sits below the admin banner when the window is
  # too short for it to be dropped: y = pad + bh/2 with no banner, and
  # y = pad + banH + gap + bh/2 with one. Both are in the app's own pixels,
  # which is what the client coordinates already are (envtool is DPI aware).
  $cx = New-Object W+RECT
  [void][W]::GetClientRect($main, [ref]$cx)
  $x = $cx.R - 48
  Write-Host ("  no child button - 关于 is painted, clicking client x={0}" -f $x)
  Write-Host ("  -> {0}" -f (Save-Win $main (Join-Path $top "build\about-$which-window.png")))
  foreach ($y in 25, 59) {
    $lp = [IntPtr](($y -shl 16) -bor $x)
    [void][W]::PostMessage($main, 0x0201, [IntPtr]1, $lp)   # WM_LBUTTONDOWN
    [void][W]::PostMessage($main, 0x0202, [IntPtr]0, $lp)   # WM_LBUTTONUP
    Start-Sleep -Milliseconds 400
    # The parentheses matter: without them PowerShell binds `$null -ne [IntPtr]::Zero`
    # as the $text argument and this test is always true.
    if ((Find-Win 'MuskAboutDlgCls' $null) -ne [IntPtr]::Zero) {
      Write-Host "  the press at y=$y hit it"
      break
    }
    Write-Host "  nothing at y=$y"
  }
}
$dlg = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $dlg -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 200
  $dlg = Find-Win 'MuskAboutDlgCls' $null
}
if ($dlg -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id -Force; throw "the About box never appeared" }
Start-Sleep -Milliseconds 400
$dr = New-Object W+RECT
[void][W]::GetWindowRect($dlg, [ref]$dr)
$mw = New-Object W+RECT
[void][W]::GetWindowRect($main, [ref]$mw)
Write-Host ("  about box at {0},{1} {2}x{3}" -f $dr.L, $dr.T, ($dr.R-$dr.L), ($dr.B-$dr.T))
Write-Host ("  -> {0}" -f (Save-Win $dlg (Join-Path $top "build\about-$which.png")))

# close the box, then the app
[void][W]::PostMessage($dlg, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE, posted
Start-Sleep -Milliseconds 400
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Write-Host "done"
