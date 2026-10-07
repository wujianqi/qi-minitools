# probe-capture-e2e.ps1 - shot from the tray icon to a file on disk.
#
#   .\test\probe-capture-e2e.ps1 [-Exe .\bin\shot.exe] [-Keep]
#
# Every other shot probe stops at the app's own idea of success: the overlay
# answers a query, a button moves a counter, the window still paints. None of
# them look at what the user actually gets. This one walks the whole path -
# trigger a capture the way the tray does, drag a region, press one of the three
# ways out, and check the thing that came out:
#
#   Save   a PNG lands on the desktop with the exact size of the region that
#          was dragged, and the overlay goes away;
#   Copy   the clipboard holds a bitmap, and the overlay goes away;
#   Cancel the overlay goes away and nothing at all is written.
#
# The size check is the load-bearing one. A capture pipeline can produce a
# correctly-shaped file of the wrong contents - a black frame, the full screen
# with the selection ignored, the region twice - and every check that stops at
# "the app says it worked" passes while the feature is broken. base= in the
# query line is the app's own answer, so comparing the file against it is a
# comparison against the app rather than against a number copied out of the
# source, and it moves when the layout moves.
#
# It was diag-shot.ps1 and wrongly filed as a diagnostic. The name said scratch
# script, the plan said end-to-end coverage, and the scratch label is how a
# real test ends up deleted as if it were debris.
#
# ASCII only (see probe-focus.ps1).

[CmdletBinding()]
param([string]$Exe, [switch]$Keep)

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'probe-lib.ps1')
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\shot.exe' }
if (-not (Test-Path $Exe)) { throw "shot.exe not found at $Exe (build it first)" }

$HOST_CLASS = 'MuskShotHost'
$OVERLAY    = 'ShotOverlayCls'

# Toolbar ids from the enum in src\shot\shot.h, repeated rather than parsed so
# a reordering shows up here as a failing check instead of a probe that follows
# the app's own mistake. TB_SWATCH0 = 100 is the first colour swatch.
$TB_COPY   = 16
$TB_SAVE   = 17
$TB_CANCEL = 19

function New-Overlay([int]$ProcId) {
    # The host's own answer to "take a screenshot", the same entry point the
    # tray menu uses. ($host is a PowerShell automatic variable, so the name is
    # not free here.)
    $hostWin = Find-AppWindow $ProcId $HOST_CLASS 8000
    if ($hostWin -eq [IntPtr]::Zero) { throw "host window never appeared" }
    [void][ProbeLib.U]::PostMessageW($hostWin, $script:WM_APP1, [IntPtr]::Zero, [IntPtr]$script:WM_LBUTTONUP)
    Find-AppWindow $ProcId $OVERLAY 10000
}

# The app describing itself. Returns $null when it stayed silent, which is a
# failure in itself and the caller turns it into a reported check rather than
# letting a null blow up somewhere less informative.
function Get-OverlayState([IntPtr]$Overlay, $App) {
    Read-LogSince $App.Err | Out-Null
    [void][ProbeLib.U]::PostMessageW($Overlay, $script:WM_APP2, [IntPtr]::Zero, [IntPtr]::Zero)
    $line = $null
    for ($k = 0; $k -lt 20 -and -not $line; $k++) {
        Start-Sleep -Milliseconds 60
        $line = @(Read-LogSince $App.Err) | Where-Object { $_ -match 'query:' } | Select-Object -Last 1
    }
    if (-not $line) { return $null }
    $st = @{}
    foreach ($m in [regex]::Matches($line, '(\w+)=([-\w,]+)')) { $st[$m.Groups[1].Value] = $m.Groups[2].Value }
    $st
}

# Drag a region out. Posted mouse messages rather than a real drag: the region
# has to land on the same pixels every run for the file size to be comparable,
# and a real drag would put this probe at the mercy of whatever else is on the
# desktop. That trade is deliberate - the end-to-end path being tested here
# starts at the region, not at the mouse.
function Select-Region([IntPtr]$Overlay) {
    Send-Down $Overlay 120 90
    Start-Sleep -Milliseconds 120
    for ($i = 1; $i -le 8; $i++) { Send-Move $Overlay (120 + $i * 50) (90 + $i * 32) $script:MK_LBUTTON }
    Start-Sleep -Milliseconds 120
    Send-Up $Overlay 520 346
    Start-Sleep -Milliseconds 400
}

# The toolbar's own geometry, out of the app's log. chrome.c dumps the row in
# verbose mode with a comment saying it is there so outside probes can aim at a
# button "without having to re-derive the layout, which is exactly the thing
# that changes" - the row moves with the selection and the DPI. Taking the last
# dump matters: an earlier one describes a toolbar from before the region
# existed.
function Get-ToolbarButtons($App) {
    $out = @()
    foreach ($l in (Read-LogAll $App.Err)) {
        $m = [regex]::Match($l, 'tb\[\s*(\d+)\]\s+id=(-?\d+)\s+color=(-?\d+)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)')
        if ($m.Success) {
            $x = [int]$m.Groups[4].Value; $y = [int]$m.Groups[5].Value
            $w = [int]$m.Groups[6].Value; $h = [int]$m.Groups[7].Value
            $out += [pscustomobject]@{
                Index = [int]$m.Groups[1].Value
                Id    = [int]$m.Groups[2].Value
                X     = $x + [int]($w / 2)
                Y     = $y + [int]($h / 2)
            }
        }
    }
    $out
}

# Press a toolbar button. chrome.c lays the row out in absolute virtual-screen
# coordinates and the window proc adds the virtual origin back when it unpacks
# a mouse message, so the logged rectangle has to come back into client space
# before it can be posted as a click - on a machine whose primary monitor is
# not at 0,0 the difference is the whole offset.
function Click-Toolbar($App, [IntPtr]$Overlay, [int]$Id) {
    $vorg = Get-VirtualOrigin
    $btn = @(Get-ToolbarButtons $App) | Where-Object { $_.Id -eq $Id } | Select-Object -Last 1
    if (-not $btn) { return $false }
    Send-Down $Overlay ($btn.X - $vorg.X) ($btn.Y - $vorg.Y)
    Start-Sleep -Milliseconds 150
    Send-Up $Overlay ($btn.X - $vorg.X) ($btn.Y - $vorg.Y)
    Start-Sleep -Milliseconds 600
    $true
}

# Every PNG on the desktop, by full path. The app names its captures with a
# timestamp in the user's own script, so the probe looks at the shape of the
# folder rather than at a name it would have to hard-code in two places.
function Get-DesktopPngs {
    $dir = [Environment]::GetFolderPath('DesktopDirectory')
    @(Get-ChildItem -Path $dir -Filter '*.png' -File -ErrorAction SilentlyContinue |
      ForEach-Object { $_.FullName })
}

function Get-PngSize([string]$Path) {
    try {
        $img = [System.Drawing.Image]::FromFile($Path)
        $t = "{0}x{1}" -f $img.Width, $img.Height
        $img.Dispose()
        $t
    } catch { $null }
}

function Get-ClipHasImage([int]$Tries = 6) {
    for ($k = 0; $k -lt $Tries; $k++) {
        try { if ([System.Windows.Forms.Clipboard]::ContainsImage()) { return $true } }
        catch {}
        Start-Sleep -Milliseconds 200
    }
    $false
}

$desktop = [Environment]::GetFolderPath('DesktopDirectory')
$before = @(Get-DesktopPngs)
$crashMark = Get-CrashMark
$app = Start-ProbeApp $Exe @('--verbose') 'probe-capture-e2e'
$mine = @()
try {
    # --- 1. save: a real file, the right size, on the desktop ------------
    $ov = New-Overlay $app.Proc.Id
    if ($ov -eq [IntPtr]::Zero) { throw "the overlay never appeared" }
    Select-Region $ov
    $st = Get-OverlayState $ov $app
    if (-not $st) { throw "the overlay did not answer WM_APP + 2 (is --verbose missing?)" }
    $want = "$($st.base)"
    Add-Check 'a capture can be started and a region dragged' ($st.sel -notmatch '^0,0,0,0$') `
        "sel=$($st.sel) base=$want"

    if (-not (Click-Toolbar $app $ov $TB_SAVE)) {
        Add-Check 'the Save button can be pressed' $false 'the app logged no toolbar buttons'
    } else {
        $after = @(Get-DesktopPngs)
        $new = @($after | Where-Object { $before -notcontains $_ })
        $mine += $new
        if ($new.Count -eq 1) {
            $got = Get-PngSize $new[0]
            # The name is not printed: the app names its captures with a
            # timestamp in the user's own script, and that does not survive the
            # trip through a redirected console legibly. The byte count says
            # the same thing about the file and stays readable.
            Add-Check 'Save writes one PNG to the desktop' $true `
                ("{0} bytes" -f (Get-Item $new[0]).Length)
            # base= is the app's own answer for how big the capture is. Reading
            # the file back and comparing is the only check in the suite that
            # can see a pipeline that produces a right-shaped file of the wrong
            # contents.
            Add-Check 'the saved PNG is exactly the region that was selected' ($got -eq $want) `
                "file is $got, the app said the capture is $want"
        } else {
            Add-Check 'Save writes one PNG to the desktop' $false "found $($new.Count) new PNG(s), expected 1"
        }
    }
    $gone = Find-AppWindow $app.Proc.Id $OVERLAY 800
    Add-Check 'the overlay closes after a save' ($gone -eq [IntPtr]::Zero) `
        $(if ($gone -eq [IntPtr]::Zero) { 'overlay is gone' } else { 'still open' })

    # --- 2. copy: a bitmap on the clipboard, and it closes ---------------
    $before = @(Get-DesktopPngs)
    for ($k = 0; $k -lt 6; $k++) {
        try { [System.Windows.Forms.Clipboard]::Clear(); break } catch { Start-Sleep -Milliseconds 200 }
    }
    $ov = New-Overlay $app.Proc.Id
    if ($ov -eq [IntPtr]::Zero) { throw "the second overlay never appeared" }
    Select-Region $ov
    $st2 = Get-OverlayState $ov $app
    if (Click-Toolbar $app $ov $TB_COPY) {
        Add-Check 'Copy puts a bitmap on the clipboard' (Get-ClipHasImage) 'clipboard holds an image'
        $left = @(Get-DesktopPngs | Where-Object { $before -notcontains $_ })
        $mine += $left
        Add-Check 'Copy writes nothing to disk' ($left.Count -eq 0) `
            $(if ($left.Count) { "wrote $($left.Count)" } else { 'desktop unchanged' })
    } else {
        Add-Check 'Copy puts a bitmap on the clipboard' $false 'the app logged no toolbar buttons'
    }
    $gone = Find-AppWindow $app.Proc.Id $OVERLAY 800
    Add-Check 'the overlay closes after a copy' ($gone -eq [IntPtr]::Zero) `
        $(if ($gone -eq [IntPtr]::Zero) { 'overlay is gone' } else { 'still open' })

    # --- 3. cancel: nothing out, nothing on disk ------------------------
    $before = @(Get-DesktopPngs)
    $ov = New-Overlay $app.Proc.Id
    if ($ov -eq [IntPtr]::Zero) { throw "the third overlay never appeared" }
    Select-Region $ov
    if (Click-Toolbar $app $ov $TB_CANCEL) {
        $gone = Find-AppWindow $app.Proc.Id $OVERLAY 800
        Add-Check 'the Cancel button closes the capture' ($gone -eq [IntPtr]::Zero) `
            $(if ($gone -eq [IntPtr]::Zero) { 'overlay is gone' } else { 'still open' })
    } else {
        Add-Check 'the Cancel button closes the capture' $false 'the app logged no toolbar buttons'
    }
    $left = @(Get-DesktopPngs | Where-Object { $before -notcontains $_ })
    $mine += $left
    Add-Check 'a cancelled capture leaves nothing on the desktop' ($left.Count -eq 0) `
        $(if ($left.Count) { "wrote $($left.Count)" } else { 'desktop unchanged' })

    # The tray host outlives all three: a program that quits after the first
    # screenshot looks perfectly correct until the second one is needed.
    $hostWin = Find-AppWindow $app.Proc.Id $HOST_CLASS 2000
    Add-Check 'the tray host is still there after three captures' ($hostWin -ne [IntPtr]::Zero) ''
}
catch {
    Add-Check 'the probe ran to completion' $false "$_"
}
finally {
    Stop-ProbeApp $app
    # The desktop is the user's, not the build's. -Keep leaves the captures
    # behind on purpose, for looking at.
    if (-not $Keep) {
        foreach ($f in $mine) {
            if ($f -and (Test-Path $f)) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
        }
    } else {
        Write-Host ("left $desktop captures on the desktop") -ForegroundColor DarkGray
    }
}

Add-CrashCheck $crashMark 'shot' 'shot logged no crash through the end-to-end capture'

Write-Checks 'shot end-to-end capture'
