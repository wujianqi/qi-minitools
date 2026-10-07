# zoom-theme-labels.ps1 - 4x crops of individual palette-card label strips, so
# the transcribed Chinese colour names can be checked glyph by glyph before
# they are hard-coded into palette.c.
#
#   .\zoom-theme-labels.ps1
#
# A whole 1242px board renders too small to trust for 18px-tall CJK text, and
# a full-page crop is just as unreadable once the image is scaled to fit. Only
# a single card's label strip survives the trip, so that is what this cuts.
# Cards sit on a fixed 3x3 grid: columns start at x=80 and are 400 wide, rows at
# y=225 and are 251 apart, measured off the boards themselves.
#
# ASCII-only like the rest of the repo (see extract-article-themes.ps1).

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dir  = Join-Path $root 'build\theme'
$out  = Join-Path $dir 'zoom'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# board, row (0-2), col (0-2), and what is being checked in that strip.
$wants = @(
    @(3, 0, 0, 'fengxian/nvzhen/tengshu'),
    @(3, 0, 1, 'kecao/tuishou-lan'),
    @(3, 1, 0, 'yingse'),
    @(2, 0, 0, 'lianse'),
    @(2, 0, 1, 'huangbaiyou'),
    @(2, 0, 2, 'mihe'),
    @(4, 0, 0, 'qianlu'),
    @(4, 1, 1, 'zhengqise'),
    @(4, 2, 1, 'shifa/tuheise'),
    @(4, 1, 2, 'miaose'),
    @(5, 0, 0, 'hongmose'),
    @(5, 0, 1, 'huangliang/hongfeng'),
    @(5, 2, 2, 'zaohè'),
    @(7, 0, 2, 'tengshu-2'),
    @(7, 2, 0, 'yingmengmu'),
    @(8, 1, 2, 'yingse-2'),
    @(9, 2, 2, 'qizi/zuanlv'),
    @(10, 1, 1, 'yundou/hongbi/tietie'),
    @(10, 2, 1, 'qianzhai'),
    @(10, 0, 2, 'sushu')
)

$SCALE = 3.5
$CW = 310; $CH = 88
$tiles = @()

foreach ($w in $wants) {
    $board = [int]$w[0]; $row = [int]$w[1]; $col = [int]$w[2]; $tag = $w[3]
    $src = Join-Path $dir ("board{0}.jpg" -f $board)
    $bmp = [System.Drawing.Bitmap]::FromFile($src)
    try {
        $x = 80 + $col * 400
        $y = 225 + $row * 251
        $x = [Math]::Max(0, [Math]::Min($x, $bmp.Width  - $CW))
        $y = [Math]::Max(0, [Math]::Min($y, $bmp.Height - $CH))
        $rect = New-Object System.Drawing.Rectangle $x, $y, $CW, $CH
        $crop = $bmp.Clone($rect, $bmp.PixelFormat)
        $big = New-Object System.Drawing.Bitmap ([int]($CW * $SCALE)), ([int]($CH * $SCALE))
        $g = [System.Drawing.Graphics]::FromImage($big)
        $g.InterpolationMode = 'NearestNeighbor'
        $g.PixelOffsetMode = 'Half'
        $g.DrawImage($crop, 0, 0, $big.Width, $big.Height)
        $g.Dispose(); $crop.Dispose()
        $path = Join-Path $out ("b{0}r{1}c{2}-{3}.png" -f $board, $row, $col, ($tag -replace '[^a-zA-Z0-9]', ''))
        $big.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $big.Dispose()
        $tiles += $path
        Write-Host ("board{0} r{1} c{2}  {3}  -> {4}" -f $board, $row, $col, $tag, (Split-Path -Leaf $path))
    } finally {
        $bmp.Dispose()
    }
}

# Stack them three per sheet: past that the sheet gets downscaled on the way
# into the reader and the whole point of the zoom is lost.
$sheet = 0
for ($i = 0; $i -lt $tiles.Count; $i += 3) {
    $sheet++
    $group = $tiles[$i..([Math]::Min($i + 2, $tiles.Count - 1))]
    $w = 0; $h = 0
    foreach ($p in $group) {
        $im = [System.Drawing.Image]::FromFile($p)
        $w = [Math]::Max($w, $im.Width); $h += $im.Height + 8
        $im.Dispose()
    }
    $canvas = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($canvas)
    $g.Clear([System.Drawing.Color]::White)
    $y = 0
    foreach ($p in $group) {
        $im = [System.Drawing.Image]::FromFile($p)
        $g.DrawImage($im, 0, $y)
        $y += $im.Height + 8
        $im.Dispose()
    }
    $g.Dispose()
    $outPath = Join-Path $out ("sheet{0}.png" -f $sheet)
    $canvas.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $canvas.Dispose()
    Write-Host ("sheet {0}: {1} tiles -> {2}" -f $sheet, $group.Count, (Split-Path -Leaf $outPath))
}
