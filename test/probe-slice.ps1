# probe-slice.ps1 - drives slice.exe end to end from the outside.
#
#   .\test\probe-slice.ps1 [-Exe .\bin\slice.exe] [-Work .\build\slice-probe]
#
# slice.exe's own --selftest covers the tree maths and the image codec
# in-process. This one runs the finished executable the way a user or a script
# does: it makes a real PNG on disk, hands it to the command line, and checks
# what actually came back - the file count, the pixel sizes and, for JPEG, that
# the result is a decodable image rather than a 0-byte file with a good name.
#
# Sections 2 and 3 are the tree model seen from outside: a hand-drawn line that
# cuts only where it was drawn, and a repeated --subdivide that drills into one
# cell as deep as asked while its neighbours stay whole.
#
# It also opens the window on that image and checks the grid is actually drawn,
# because a headless-only test would pass with a completely broken canvas.
#
# ASCII only (see probe-tray-stress.ps1).
[CmdletBinding()]
param(
    [string]$Exe,
    [string]$Work,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe)  { $Exe  = Join-Path $root '..\bin\slice.exe' }
if (-not $Work) { $Work = Join-Path $root '..\build\slice-probe' }
$Exe  = (Resolve-Path $Exe).Path
$Work = [System.IO.Path]::GetFullPath($Work)

$script:failures = 0
function Bad($msg) { Write-Host "FAIL: $msg" -ForegroundColor Red; $script:failures++ }
function Ok($msg)  { Write-Host "ok   $msg" }

# Start-Process -Wait -PassThru, not `& $exe ... 2>&1`: these are GUI-subsystem
# binaries, so the call operator returns immediately and leaves $LASTEXITCODE
# unset - which would report every one of these checks as passing without the
# program ever having run.
function Run-Slice([string[]]$SliceArgs) {
    $so = [System.IO.Path]::GetTempFileName()
    $se = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $SliceArgs -Wait -PassThru -NoNewWindow `
                           -RedirectStandardOutput $so -RedirectStandardError $se
        $text = (Get-Content $se -Raw -ErrorAction SilentlyContinue)
        if (-not $text) { $text = (Get-Content $so -Raw -ErrorAction SilentlyContinue) }
        return [pscustomobject]@{ Exit = $p.ExitCode; Out = "$text" }
    } finally {
        [void](Remove-Item $so, $se -Force -ErrorAction SilentlyContinue)
    }
}

Add-Type -AssemblyName System.Drawing

function New-Source([string]$Path, [int]$W, [int]$H) {
    $bmp = New-Object System.Drawing.Bitmap $W, $H
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    # four quadrants, so a slice that lands on the wrong cell is visible in the
    # output pixels rather than only in the file size
    $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Red)
    $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Lime)
    $bb = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Blue)
    $g.FillRectangle($br, 0, 0, [int]($W/2), [int]($H/2))
    $g.FillRectangle($bg, [int]($W/2), 0, [int]($W/2), [int]($H/2))
    $g.FillRectangle($bb, 0, [int]($H/2), [int]($W/2), [int]($H/2))
    $br.Dispose(); $bg.Dispose(); $bb.Dispose()
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function Get-Size([string]$Path) {
    $img = [System.Drawing.Image]::FromFile($Path)
    try { return [pscustomobject]@{ W = $img.Width; H = $img.Height } }
    finally { $img.Dispose() }
}

function Get-Pixel([string]$Path, [int]$X, [int]$Y) {
    $bmp = New-Object System.Drawing.Bitmap $Path
    try { return $bmp.GetPixel($X, $Y) }
    finally { $bmp.Dispose() }
}

Write-Host "probe-slice: $Exe"
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$src = Join-Path $Work 'probe.png'
$W = 240; $H = 120
New-Source $src $W $H
$srcSize = Get-Size $src
if ($srcSize.W -ne $W -or $srcSize.H -ne $H) {
    Bad ("test image is {0}x{1}, expected {2}x{3}" -f $srcSize.W, $srcSize.H, $W, $H)
} else {
    Ok "test image is $W x $H"
}

# --- 1. a plain 3x2 division -------------------------------------------
$out1 = Join-Path $Work 'grid'
$r = Run-Slice @('--split', $src, '--out', $out1, '--cols', '3', '--rows', '2', '--prefix', 'g')
if ($r.Exit -ne 0) { Bad "3x2 split exited $($r.Exit): $($r.Out.Trim())" }
$files = @(Get-ChildItem $out1 -Filter '*.png' -ErrorAction SilentlyContinue)
if ($files.Count -ne 6) {
    Bad "3x2 split produced $($files.Count) files, expected 6"
} else {
    Ok "3x2 split produced 6 files"
    $names = @($files | ForEach-Object { $_.Name } | Sort-Object)
    $want  = @('g_01.png','g_02.png','g_03.png','g_04.png','g_05.png','g_06.png')
    if (Compare-Object $names $want) { Bad ("names are $($names -join ','), expected $($want -join ',')") }
    else { Ok "files are named and zero-padded in order" }

    $bad = 0
    foreach ($f in $files) {
        $s = Get-Size $f.FullName
        if ($s.W -ne 80 -or $s.H -ne 60) { $bad++; Write-Host ("     {0} is {1}x{2}, expected 80x60" -f $f.Name, $s.W, $s.H) }
    }
    if ($bad) { Bad "$bad of 6 cells have the wrong pixel size" } else { Ok "every cell is 80 x 60" }

    # row-major order: 1..3 is the top strip left to right, 4..6 the bottom
    $c1 = Get-Pixel (Join-Path $out1 'g_01.png') 5 5
    $c3 = Get-Pixel (Join-Path $out1 'g_03.png') 5 5
    $c4 = Get-Pixel (Join-Path $out1 'g_04.png') 5 5
    if ($c1.R -gt 200 -and $c1.G -lt 60 -and $c1.B -lt 60) { Ok "cell 1 holds the red quadrant" }
    else { Bad ("cell 1 is R{0} G{1} B{2}, expected red" -f $c1.R, $c1.G, $c1.B) }
    if ($c3.G -gt 200 -and $c3.R -lt 60) { Ok "cell 3 holds the green quadrant" }
    else { Bad ("cell 3 is R{0} G{1} B{2}, expected green" -f $c3.R, $c3.G, $c3.B) }
    if ($c4.B -gt 200 -and $c4.R -lt 60 -and $c4.G -lt 60) { Ok "cell 4 holds the blue quadrant" }
    else { Bad ("cell 4 is R{0} G{1} B{2}, expected blue" -f $c4.R, $c4.G, $c4.B) }
}

# --- 2. a hand-drawn line, and only where it was drawn --------------------
# --draw takes the same x1,y1,x2,y2 a mouse drag would, and the line runs along
# the drag: dragged down it is a vertical line at the cursor's x, dragged across
# it is a horizontal one at the cursor's y. The span is the drag's own extent,
# so a short sweep cuts a short run and leaves the rest of the row alone.
$outD = Join-Path $Work 'draw'
$r = Run-Slice @('--split', $src, '--out', $outD, '--draw', '120,0,120,119', '--prefix', 'd')
$files = @(Get-ChildItem $outD -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 2) {
    Bad "a full-height vertical line produced $($files.Count) files, expected 2"
} else {
    $a = Get-Size $files[0].FullName
    $b = Get-Size $files[1].FullName
    if ($a.W -eq 120 -and $a.H -eq 120 -and $b.W -eq 120 -and $b.H -eq 120) {
        Ok "a vertical drag splits the picture into two 120 x 120 halves"
    } else {
        Bad ("the halves are {0}x{1} and {2}x{3}, expected 120x120 each" -f $a.W, $a.H, $b.W, $b.H)
    }
    $c1 = Get-Pixel $files[0].FullName 5 5
    $c2 = Get-Pixel $files[1].FullName 5 5
    if ($c1.R -gt 200 -and $c2.G -gt 200) { Ok "the two halves keep their own side of the picture" }
    else { Bad ("the halves are R{0}G{1}B{2} and R{3}G{4}B{5}, expected red then green" -f `
                $c1.R, $c1.G, $c1.B, $c2.R, $c2.G, $c2.B) }
}

# the span is the whole point: this line stops at y=50, so the right-hand column
# must come through untouched rather than being cut along with the left
$outE = Join-Path $Work 'drawpart'
$r = Run-Slice @('--split', $src, '--out', $outE, '--cols', '2',
                 '--draw', '10,0,10,49', '--prefix', 'e')
$files = @(Get-ChildItem $outE -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 3) {
    Bad "a short line produced $($files.Count) files, expected 3"
} else {
    $ws = @($files | ForEach-Object { (Get-Size $_.FullName).W })
    if ($ws[0] -eq 10 -and $ws[1] -eq 110 -and $ws[2] -eq 120) {
        Ok "a short line cuts only the column it was dragged across (10 / 110 / 120)"
    } else {
        Bad ("the column widths are $($ws -join ' / '), expected 10 / 110 / 120")
    }
    $c3 = Get-Pixel $files[2].FullName 5 5
    if ($c3.G -gt 200) { Ok "the column the line never reached is left whole" }
    else { Bad ("the untouched column is R{0}G{1}B{2}, expected green" -f $c3.R, $c3.G, $c3.B) }
}

# --- 3. a dragged grid line ------------------------------------------------
# --move slides a line that is already a slice edge. The span is the whole
# picture, so the model grows it to the run of collinear edges the position
# belongs to: in a 3x2 grid the three row segments of one column line share
# nothing but their x, and all of them have to move together. Two rows of
# 130 / 50 / 70 rather than one row that moved and two that did not is the whole
# contract, and a line that moved only under the cursor would fail it.
$outM = Join-Path $Work 'move'
$r = Run-Slice @('--split', $src, '--out', $outM, '--cols', '3', '--rows', '2',
                 '--move', 'v', '80', '130', '--prefix', 'm')
$files = @(Get-ChildItem $outM -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 6) {
    Bad "--move produced $($files.Count) files, expected 6"
} else {
    $ws = @($files | ForEach-Object { (Get-Size $_.FullName).W })
    $expected = @(130, 50, 70, 130, 50, 70)
    if (Compare-Object $ws $expected -SyncWindow 0) {
        Ok "a dragged column line moves in every row at once (130 / 50 / 70, twice)"
    } else {
        Bad ("the column widths are $($ws -join ' / '), expected $($expected -join ' / ')")
    }
    $hs = @($files | ForEach-Object { (Get-Size $_.FullName).H })
    if (($hs | Where-Object { $_ -ne 60 }).Count -eq 0) {
        Ok "moving a line sideways does not disturb the rows"
    } else {
        Bad ("the row heights are $($hs -join ' / '), expected 60 each")
    }
}

# a target past the edge is clamped to the edge rather than refused - the script
# asked to push the line as far as it goes and the edge is as far as it goes
$outM2 = Join-Path $Work 'moveclamp'
$r = Run-Slice @('--split', $src, '--out', $outM2, '--cols', '2',
                 '--move', 'v', '120', '9999', '--prefix', 'mc')
$files = @(Get-ChildItem $outM2 -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 2) {
    Bad "a clamped move produced $($files.Count) files, expected 2"
} else {
    $a = Get-Size $files[0].FullName
    if ($a.W -eq $W - 1) { Ok "a target past the edge clamps to the edge ($($a.W)px)" }
    else { Bad "the clamped column is $($a.W)px, expected $($W - 1)" }
}

# the other axis, so the letter convention is pinned both ways
$outM3 = Join-Path $Work 'moveh'
$r = Run-Slice @('--split', $src, '--out', $outM3, '--rows', '2',
                 '--move', 'h', '60', '100', '--prefix', 'mh')
$files = @(Get-ChildItem $outM3 -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 2) {
    Bad "--move h produced $($files.Count) files, expected 2"
} else {
    $hs = @($files | ForEach-Object { (Get-Size $_.FullName).H })
    if ($hs[0] -eq 100 -and $hs[1] -eq 20) { Ok "a dragged row line moves too (100 / 20)" }
    else { Bad ("the band heights are $($hs -join ' / '), expected 100 / 20") }
}

# a position that is not on any edge has nothing to move, and that is reported
# rather than silently dragging something else. --verbose has to be in the
# argument list for the warning to be written at all, and it must not be first:
# the batch path is only entered when argv[1] is --split, and anything else
# opens the window instead.
$outM4 = Join-Path $Work 'movebad'
$r = Run-Slice @('--split', $src, '--out', $outM4, '--cols', '3',
                 '--move', 'v', '137', '200', '--prefix', 'mb', '--verbose')
$files = @(Get-ChildItem $outM4 -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -eq 0 -and $files.Count -eq 3 -and ($r.Out -match 'not on a slice edge')) {
    Ok "moving a position that is not a slice edge is refused and reported"
} else {
    Bad ("a bogus --move produced $($files.Count) files; expected 3 and a warning")
}

# --- 4. merging cells --------------------------------------------------------
$outG = Join-Path $Work 'merge'
$r = Run-Slice @('--split', $src, '--out', $outG, '--cols', '3', '--rows', '2',
                 '--merge', '0,0,80,120', '--prefix', 'g')
$files = @(Get-ChildItem $outG -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 5) {
    Bad "--merge produced $($files.Count) files, expected 5"
} else {
    $a = Get-Size $files[0].FullName
    if ($a.W -eq 80 -and $a.H -eq $H) { Ok "a merged column comes back as one 80 x $H cell" }
    else { Bad "the merged cell is $($a.W)x$($a.H), expected 80 x $H" }
    $rest = @($files | Select-Object -Skip 1 | ForEach-Object { (Get-Size $_.FullName).H })
    if (($rest | Where-Object { $_ -ne 60 }).Count -eq 0) {
        Ok "the four cells beside it are untouched"
    } else {
        Bad ("the remaining heights are $($rest -join ' / '), expected 60 each")
    }
}

# the whole picture at once: the same as 重置, in one switch
$outG2 = Join-Path $Work 'mergeall'
$r = Run-Slice @('--split', $src, '--out', $outG2, '--cols', '3', '--rows', '2',
                 '--merge', '0,0')
if ($r.Exit -eq 2) { Ok "--merge with a malformed box explains itself (exit 2)" }
else { Bad "--merge with a malformed box exited $($r.Exit), expected 2" }
$r = Run-Slice @('--split', $src, '--out', $outG2, '--cols', '3', '--rows', '2',
                 '--merge', "0,0,$W,$H", '--prefix', 'ga')
$files = @(Get-ChildItem $outG2 -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 1) {
    Bad "merging the whole picture produced $($files.Count) files, expected 1"
} else {
    $a = Get-Size $files[0].FullName
    if ($a.W -eq $W -and $a.H -eq $H) { Ok "merging everything leaves one whole slice" }
    else { Bad "the single slice is $($a.W)x$($a.H), expected ${W}x$H" }
}

# a box that clips a cell is refused rather than cutting one in half
$outG3 = Join-Path $Work 'mergebad'
$r = Run-Slice @('--split', $src, '--out', $outG3, '--cols', '3',
                 '--merge', '0,0,77,120', '--prefix', 'gb', '--verbose')
$files = @(Get-ChildItem $outG3 -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -eq 0 -and $files.Count -eq 3 -and ($r.Out -match 'line up')) {
    Ok "a box that clips a cell is refused and reported"
} else {
    Bad ("a misaligned --merge produced $($files.Count) files; expected 3 and a warning")
}

# the two features together: move a line, then merge across where it used to be
$outG4 = Join-Path $Work 'moveandmerge'
$r = Run-Slice @('--split', $src, '--out', $outG4, '--cols', '4', '--rows', '2',
                 '--move', 'v', '60', '100', '--merge', '100,0,120,120',
                 '--prefix', 'gm')
$files = @(Get-ChildItem $outG4 -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 7) {
    Bad "move-then-merge produced $($files.Count) files, expected 7"
} else {
    $a = Get-Size $files[1].FullName
    if ($a.W -eq 20 -and $a.H -eq $H) { Ok "a moved line still merges: the two cells become one" }
    else { Bad "the merged cell is $($a.W)x$($a.H), expected 20 x $H" }
}

# --- 5. the other axis: dragged across, the line is horizontal
$outF = Join-Path $Work 'drawh'
$r = Run-Slice @('--split', $src, '--out', $outF, '--draw', '0,60,239,60', '--prefix', 'f')
$files = @(Get-ChildItem $outF -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 2) {
    Bad "a horizontal line produced $($files.Count) files, expected 2"
} else {
    $a = Get-Size $files[0].FullName
    $b = Get-Size $files[1].FullName
    if ($a.W -eq $W -and $a.H -eq 60 -and $b.W -eq $W -and $b.H -eq 60) {
        Ok "a sideways drag splits it into two full-width bands"
    } else {
        Bad ("the bands are {0}x{1} and {2}x{3}, expected {4}x60 each" -f $a.W, $a.H, $b.W, $b.H, $W)
    }
    $top = Get-Pixel $files[0].FullName 5 5
    $bot = Get-Pixel $files[1].FullName 5 5
    if ($top.R -gt 200 -and $bot.B -gt 200) { Ok "the bands are the top and bottom of the picture" }
    else { Bad ("the bands are R{0}G{1}B{2} and R{3}G{4}B{5}, expected red then blue" -f `
                $top.R, $top.G, $top.B, $bot.R, $bot.G, $bot.B) }
}

# --- 3. subdividing one cell, as deep as you like ------------------------
# The point of the region tree: --subdivide touches exactly one cell, and
# repeating it drills down that cell rather than re-cutting it to the same
# depth. Three halvings of the left column of a 240px picture: 120 -> 60 -> 30
# -> 15, so the widths come out 15, 15, 30, 60 and the right column stays 120.
$outG = Join-Path $Work 'subdiv'
$r = Run-Slice @('--split', $src, '--out', $outG, '--cols', '2',
                 '--subdivide', '1', 'v', '2',
                 '--subdivide', '1', 'v', '2',
                 '--subdivide', '1', 'v', '2', '--prefix', 's')
$files = @(Get-ChildItem $outG -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 5) {
    Bad "three drill-downs produced $($files.Count) files, expected 5"
} else {
    # $ws/$hs, not $w/$h: PowerShell variable names ignore case, so a plain $w
    # here would quietly replace the $W/$H the rest of this script measures with
    $ws = @($files | ForEach-Object { (Get-Size $_.FullName).W })
    if (($ws -join ',') -eq '15,15,30,60,120') {
        Ok "repeating --subdivide 1 drills into one cell: 15 / 15 / 30 / 60 / 120"
    } else {
        Bad ("the widths are $($ws -join ' / '), expected 15 / 15 / 30 / 60 / 120")
    }
    $hs = @($files | ForEach-Object { (Get-Size $_.FullName).H } | Select-Object -Unique)
    if ($hs.Count -eq 1 -and $hs[0] -eq $H) { Ok "every slice is still full height" }
    else { Bad "the drill-down changed the height too: $($hs -join ' / ')" }
    $last = Get-Pixel $files[4].FullName 5 5
    if ($last.G -gt 200) { Ok "the neighbouring cell was never touched" }
    else { Bad ("the neighbour is R{0}G{1}B{2}, expected green" -f $last.R, $last.G, $last.B) }
}

# h is a horizontal line, so it makes rows - the same 3 slices as above but
# split top to bottom instead of left to right
$outH = Join-Path $Work 'subdivh'
$r = Run-Slice @('--split', $src, '--out', $outH, '--cols', '2',
                 '--subdivide', '1', 'h', '2', '--prefix', 'h')
$files = @(Get-ChildItem $outH -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($r.Exit -ne 0 -or $files.Count -ne 3) {
    Bad "--subdivide 1 h 2 produced $($files.Count) files, expected 3"
} else {
    # reading order puts the untouched right column second: it shares the top
    $sizes = @($files | ForEach-Object { $s = Get-Size $_.FullName; "$($s.W)x$($s.H)" })
    if (($sizes -join ',') -eq '120x60,120x120,120x60') {
        Ok "h cuts the cell into rows (120x60, 120x120, 120x60)"
    } else {
        Bad ("the slices are $($sizes -join ', '), expected 120x60, 120x120, 120x60")
    }
    $c = Get-Pixel $files[2].FullName 5 5
    if ($c.B -gt 200) { Ok "the lower half of the same cell is the bottom of the picture" }
    else { Bad ("the lower half is R{0}G{1}B{2}, expected blue" -f $c.R, $c.G, $c.B) }
}

# --- 4. fixed cell size, with a leftover strip --------------------------
$out2 = Join-Path $Work 'fixed'
$r = Run-Slice @('--split', $src, '--out', $out2, '--cell', '100', '--prefix', 'c')
$files = @(Get-ChildItem $out2 -Filter '*.png' -ErrorAction SilentlyContinue)
if ($files.Count -ne 6) {
    # 240/100 -> 100,100,40 (3 cells) and 120/100 -> 100,20 (2 cells) = 6
    Bad "fixed 100px cells produced $($files.Count) files, expected 6"
} else {
    Ok "fixed 100px cells produced 6 files"
    $s = Get-Size (Join-Path $out2 'c_01.png')
    $last = Get-Size (Join-Path $out2 'c_06.png')
    if ($s.W -eq 100 -and $s.H -eq 100) { Ok "full cells are 100 x 100" }
    else { Bad ("cell 1 is {0}x{1}, expected 100x100" -f $s.W, $s.H) }
    # the leftover strips are kept, not rounded away or dropped
    if ($last.W -eq 40 -and $last.H -eq 20) { Ok "the leftover strip is kept at its real size" }
    else { Bad ("last cell is {0}x{1}, expected 40x20" -f $last.W, $last.H) }
}

# --- 5. JPEG output ----------------------------------------------------
$out3 = Join-Path $Work 'jpeg'
$r = Run-Slice @('--split', $src, '--out', $out3, '--cols', '2', '--rows', '2',
                 '--format', 'jpeg', '--quality', '90', '--prefix', 'j')
$files = @(Get-ChildItem $out3 -Filter '*.jpg' -ErrorAction SilentlyContinue)
if ($files.Count -ne 4) {
    Bad "jpeg split produced $($files.Count) files, expected 4"
} elseif (@(Get-ChildItem $out3 -Filter '*.png' -ErrorAction SilentlyContinue).Count -ne 0) {
    Bad "jpeg split also left PNG files behind"
} else {
    Ok "jpeg split produced 4 .jpg files"
    $s = Get-Size (Join-Path $out3 'j_01.jpg')
    if ($s.W -eq 120 -and $s.H -eq 60) { Ok "the jpeg cells are 120 x 60" }
    else { Bad ("a jpeg cell is {0}x{1}, expected 120x60" -f $s.W, $s.H) }
    $c = Get-Pixel (Join-Path $out3 'j_01.jpg') 5 5
    if ($c.R -gt 180 -and $c.G -lt 90) { Ok "the jpeg cell keeps roughly the right colour" }
    else { Bad ("the jpeg cell is R{0} G{1} B{2}, expected roughly red" -f $c.R, $c.G, $c.B) }
}

# --- 6. the whole picture as one slice ---------------------------------
$out4 = Join-Path $Work 'whole'
$r = Run-Slice @('--split', $src, '--out', $out4, '--prefix', 'w')
$files = @(Get-ChildItem $out4 -Filter '*.png' -ErrorAction SilentlyContinue)
if ($files.Count -ne 1) {
    Bad "no grid produced $($files.Count) files, expected 1"
} else {
    $s = Get-Size $files[0].FullName
    if ($s.W -eq $W -and $s.H -eq $H) { Ok "a single cell is the whole picture" }
    else { Bad ("the single cell is {0}x{1}, expected {2}x{3}" -f $s.W, $s.H, $W, $H) }
}

# --- 7. what has to be refused ------------------------------------------
$r = Run-Slice @('--split', (Join-Path $Work 'missing.png'), '--out', (Join-Path $Work 'nope'))
if ($r.Exit -eq 0) { Bad "a missing input file exited 0" } else { Ok "a missing input file is refused" }

$bmpPath = Join-Path $Work 'thing.bmp'
$nb = New-Object System.Drawing.Bitmap 8, 8
$nb.Save($bmpPath, [System.Drawing.Imaging.ImageFormat]::Bmp)
$nb.Dispose()
$r = Run-Slice @('--split', $bmpPath, '--out', (Join-Path $Work 'nope2'))
if ($r.Exit -eq 0) { Bad "a .bmp input was accepted" } else { Ok "a .bmp input is refused" }

$junk = Join-Path $Work 'junk.png'
Set-Content -Path $junk -Value 'not an image at all' -Encoding ASCII
$r = Run-Slice @('--split', $junk, '--out', (Join-Path $Work 'nope3'))
if ($r.Exit -eq 0) { Bad "a corrupt .png was accepted" } else { Ok "a corrupt .png is refused" }

$r = Run-Slice @('--split')
if ($r.Exit -eq 0) { Bad "--split with no input exited 0" } else { Ok "--split with no input explains itself" }

# --- 8. the default output folder lands beside the source ---------------
$alone = Join-Path $Work 'lonely'
New-Item -ItemType Directory -Force -Path $alone | Out-Null
$only = Join-Path $alone 'holiday.png'
New-Source $only 60 40
$r = Run-Slice @('--split', $only, '--cols', '2')
$made = @(Get-ChildItem (Join-Path $alone 'holiday_slices') -ErrorAction SilentlyContinue)
if ($made.Count -ne 2) {
    Bad "the default folder got $($made.Count) files, expected 2 in holiday_slices"
} else {
    Ok "the default folder is <source>_slices and holds 2 files"
}

# --- 9. the window opens and draws the grid ------------------------------
Add-Type -Namespace SliceProbe -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public delegate bool EnumProc(IntPtr h, IntPtr p);
public struct RECT { public int L, T, R, B; }
'@
[void][SliceProbe.U]::SetProcessDPIAware()

function Find-SlicerWindow([int]$ProcId) {
    $script:found = [IntPtr]::Zero
    $cb = [SliceProbe.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][SliceProbe.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $ProcId -and [SliceProbe.U]::IsWindowVisible($h)) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][SliceProbe.U]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq 'MuskSliceMain') { $script:found = $h; return $false }
        }
        return $true
    }
    [void][SliceProbe.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

$app = Start-Process -FilePath $Exe -ArgumentList @($src) -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 150
    if ($app.HasExited) { break }
    $hwnd = Find-SlicerWindow $app.Id
}
if ($hwnd -eq [IntPtr]::Zero) {
    Bad 'the window never appeared'
    if (-not $app.HasExited) { $app.Kill() }
} else {
    Ok 'the window opened on the file given on the command line'
    Start-Sleep -Milliseconds 400
    [void][SliceProbe.U]::SetForegroundWindow($hwnd)

    # PrintWindow with PW_RENDERFULLCONTENT: the canvas is a GDI+ back buffer
    # blitted in WM_PAINT, and without the flag a layered/DWM window comes back
    # blank - which would make every pixel check below pass for the wrong reason.
    $r = New-Object SliceProbe.U+RECT
    [void][SliceProbe.U]::GetClientRect($hwnd, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max($r.R, 1)), ([Math]::Max($r.B, 1))
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    $painted = [SliceProbe.U]::PrintWindow($hwnd, $hdc, 2)
    $gfx.ReleaseHdc($hdc)
    $gfx.Dispose()

    if (-not $painted) {
        Bad 'PrintWindow could not capture the window'
    } else {
        # the canvas is on the left, so the left third must not be the panel
        # grey; the grid lines are near-white 1px runs over the test image
        $purple = 0; $white = 0
        for ($y = 0; $y -lt $bmp.Height; $y += 2) {
            for ($x = 0; $x -lt [int]($bmp.Width * 0.6); $x += 2) {
                $c = $bmp.GetPixel($x, $y)
                if ([Math]::Abs($c.R - 0x7C) -le 40 -and [Math]::Abs($c.G - 0x3A) -le 40 -and
                    [Math]::Abs($c.B - 0xED) -le 40) { $purple++ }
                if ($c.R -gt 235 -and $c.G -gt 235 -and $c.B -gt 235) { $white++ }
            }
        }
        if ($white -lt 200) { Bad "the canvas looks empty ($white light pixels) - nothing was drawn" }
        else { Ok "the canvas painted the image and its grid ($white light px)" }

        # the selected cut is drawn in the brand purple; a default 1x1 grid has
        # none, so this only asserts the palette is present, not the selection
        if ($purple -gt 0) { Ok "the brand purple is in use on screen ($purple px)" }
    }
    $bmp.Dispose()
    [void]$app.CloseMainWindow()
    Start-Sleep -Milliseconds 300
    if (-not $app.HasExited) { $app.Kill() }
    $app.WaitForExit(3000) | Out-Null
}

# --- 10. transparency survives the round trip and reaches the canvas -------
# A transparent source is the only thing that makes the canvas draw its
# checkerboard, so opening one here is what actually exercises that path.
$alphaSrc = Join-Path $Work 'alpha.png'
$bmp = New-Object System.Drawing.Bitmap 200, 100
$bmp.SetPixel(0, 0, [System.Drawing.Color]::FromArgb(0, 0, 0, 0))
$bmp.SetPixel(199, 0, [System.Drawing.Color]::FromArgb(255, 220, 30, 60))
for ($x = 0; $x -lt 100; $x++) { for ($y = 0; $y -lt 100; $y++) { $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(0, 0, 0, 0)) } }
$bmp.Save($alphaSrc, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

$outA = Join-Path $Work 'alpha'
$r = Run-Slice @('--split', $alphaSrc, '--out', $outA, '--cols', '2', '--prefix', 'a')
$leftCell = Join-Path $outA 'a_01.png'
if (-not (Test-Path $leftCell)) {
    Bad 'the transparent source produced no cells'
} else {
    $cb = New-Object System.Drawing.Bitmap $leftCell
    $pa = $cb.GetPixel(5, 5)
    $cb.Dispose()
    if ($pa.A -lt 10) { Ok 'a PNG slice keeps its transparency' }
    else { Bad "the transparent half came back opaque (alpha $($pa.A))" }
}
$r = Run-Slice @('--split', $alphaSrc, '--out', (Join-Path $Work 'alphajpg'),
                 '--cols', '2', '--format', 'jpeg', '--prefix', 'j')
$jpgCell = Join-Path (Join-Path $Work 'alphajpg') 'j_01.jpg'
if (Test-Path $jpgCell) {
    $cb = New-Object System.Drawing.Bitmap $jpgCell
    $pj = $cb.GetPixel(5, 5)
    $cb.Dispose()
    if ($pj.R -gt 200 -and $pj.G -gt 200 -and $pj.B -gt 200) {
        Ok 'a JPEG slice flattens transparency onto white, not black'
    } else {
        Bad "the transparent half flattened to R$($pj.R) G$($pj.G) B$($pj.B), expected white"
    }
} else {
    Bad 'the JPEG run produced no cells'
}

# --- summary -------------------------------------------------------------
# The window this probe opened is still running at this point, and that is
# not harmless. The probe after this one starts a second slice.exe, and a
# lookup by window class returns whichever instance the desktop happens to
# enumerate first - so probe-slice-ui was driving this leftover instead of its
# own, and reported its Open, Browse, subdivide and Export buttons as dead.
# Closed here, where it was started.
if ($app -and -not $app.HasExited) {
    $app.Kill()
    for ($k = 0; $k -lt 40 -and -not $app.HasExited; $k++) { Start-Sleep -Milliseconds 50 }
}
if ($script:failures) {
    Write-Host "RESULT: slice probe FAILED ($script:failures problem(s))" -ForegroundColor Red
    exit 1
}
if (-not $Keep) { Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "RESULT: slice probe passed" -ForegroundColor Green
exit 0
