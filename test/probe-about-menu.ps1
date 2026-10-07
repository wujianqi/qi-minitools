# probe-about-menu.ps1 - opens the About box the way the tray menu does.
#   .\test\probe-about-menu.ps1 shot|picker
#
# shot and picker have no window button: their 关于 lives in the tray menu,
# which arrives at the program as WM_COMMAND with the menu id and lParam == 0.
# Posting that one message is the same thing the menu item does, which makes
# this a real end-to-end check of the entry point a user actually clicks.
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class M {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
$ErrorActionPreference = 'Stop'
[void][M]::SetProcessDPIAware()

function Find-Win([string]$cls) {
  $script:found = [IntPtr]::Zero
  $cb = [M+EnumProc]{
    param($h, $p)
    $sb = New-Object System.Text.StringBuilder 128
    [void][M]::GetClassName($h, $sb, 128)
    if ($sb.ToString() -eq $cls) { $script:found = $h; return $false }
    return $true
  }
  [void][M]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:found
}

function Save-Win([IntPtr]$h, [string]$path) {
  $r = New-Object M+RECT
  [void][M]::GetWindowRect($h, [ref]$r)
  $w = $r.R - $r.L; $ht = $r.B - $r.T
  if ($w -le 0 -or $ht -le 0) { return "no rect for $h" }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  [void][M]::PrintWindow($h, $dc, 2)
  $g.ReleaseHdc($dc)
  $g.Dispose()
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  return "saved $path ($w x $ht)"
}

$which = $args[0]
# program -> (window class, tray-menu id of 关于, caption we expect to see)
$cfg = @{
  'shot'   = @{ cls = 'MuskShotHost';    id = 40006 }
  'picker' = @{ cls = 'MuskPickerMain';  id = 40003 }
}
# The empty case is checked first on purpose. Hashtable.ContainsKey throws on a
# null key rather than returning false, so a probe invoked without its argument
# died with "Key cannot be null" from three lines further down, which says
# nothing about the thing that is actually wrong.
if (-not $which) { throw "no program given - call this as .\test\probe-about-menu.ps1 shot|picker" }
if (-not $cfg.ContainsKey($which)) { throw "unknown program '$which' (expected shot or picker)" }
$top  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe  = Join-Path $top "bin\$which.exe"

$p = Start-Process -FilePath $exe -PassThru
$main = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $main -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 250
  $main = Find-Win $cfg[$which].cls
}
if ($main -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id -Force; throw "no window of class $($cfg[$which].cls)" }
Write-Host ("{0}: window {1}, posting WM_COMMAND {2}" -f $which, $main, $cfg[$which].id)

# lParam 0 is the whole point: a child control's notification carries a handle
# there, and both programs ignore commands that do.
[void][M]::PostMessage($main, 0x0111, [IntPtr]$cfg[$which].id, [IntPtr]::Zero)

$dlg = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $dlg -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 200
  $dlg = Find-Win 'MuskAboutDlgCls'
}
if ($dlg -eq [IntPtr]::Zero) {
  Stop-Process -Id $p.Id -Force
  throw "the About box never appeared for $which"
}
Start-Sleep -Milliseconds 400
$r = New-Object M+RECT
[void][M]::GetWindowRect($dlg, [ref]$r)
Write-Host ("  about box {0}x{1}" -f ($r.R-$r.L), ($r.B-$r.T))
Write-Host ("  -> {0}" -f (Save-Win $dlg (Join-Path $top "build\about-$which.png")))
[void][M]::PostMessage($dlg, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
Start-Sleep -Milliseconds 500
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Write-Host "done"
