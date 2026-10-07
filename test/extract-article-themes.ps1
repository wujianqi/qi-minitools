# extract-article-themes.ps1 - pulls the real swatch values out of the ten
# colour-scheme boards in the reference article, so MuskPicker's theme palettes
# carry measured colours instead of hand-picked guesses.
#
#   .\extract-article-themes.ps1              # download + report
#   .\extract-article-themes.ps1 -Show 1,2,3  # also save downscaled previews
#
# Provenance: baijiahao.baidu.com/s?id=1722180419317727466, "theme style colour
# schemes". The article ships one board per style; the text beside each image
# only states the rule (saturation, hue family, how much white to use), and the
# values themselves exist nowhere but as pixels - so they get sampled here and
# transcribed into the THEME_* tables in palette.c.
#
# This file stays ASCII-only, like every other script in the repo: Windows
# PowerShell 5.1 reads a BOM-less .ps1 as ANSI, which mangles non-ASCII source
# into a parse error. The Chinese style names live in palette.c instead, where
# the C compiler reads UTF-8 properly.

[CmdletBinding()]
param(
    [int[]]$Show = @()
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out  = Join-Path $root 'build\theme'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# In article order. 'style' is the ASCII tag; palette.c maps it to the Chinese
# name the app shows.
$boards = @(
    @{ style = 'sample';   url = 'https://pics1.baidu.com/feed/060828381f30e92489c5e4fe939ad00f1c95f73c.jpeg' },
    @{ style = 'soft';     url = 'https://pics6.baidu.com/feed/55e736d12f2eb938b39ff27e35f03b3ce7dd6fec.jpeg' },
    @{ style = 'lively';   url = 'https://pics0.baidu.com/feed/80cb39dbb6fd5266a804cd1a498a2922d4073629.jpeg' },
    @{ style = 'natural';  url = 'https://pics5.baidu.com/feed/aa18972bd40735fa367415e278c3b1ba0e240805.jpeg' },
    @{ style = 'classic';  url = 'https://pics7.baidu.com/feed/c9fcc3cec3fdfc032d91b30a3ead399da5c22637.jpeg' },
    @{ style = 'elegant';  url = 'https://pics5.baidu.com/feed/d8f9d72a6059252dd28d8f8fd209bd325ab5b93b.jpeg' },
    @{ style = 'clean';    url = 'https://pics6.baidu.com/feed/3c6d55fbb2fb431605e26725c936f82a08f7d308.jpeg' },
    @{ style = 'rich';     url = 'https://pics7.baidu.com/feed/48540923dd54564e53829262524c228bd0584f26.jpeg' },
    @{ style = 'feminine'; url = 'https://pics2.baidu.com/feed/9e3df8dcd100baa19043b6c6a282071bc9fc2ef8.jpeg' },
    @{ style = 'rational'; url = 'https://pics1.baidu.com/feed/0824ab18972bd4070f531675821b20580eb3093a.jpeg' }
)

function Get-Hsv {
    param([int]$R, [int]$G, [int]$B)
    $mx = [Math]::Max($R, [Math]::Max($G, $B)); $mn = [Math]::Min($R, [Math]::Min($G, $B))
    $d = $mx - $mn
    if ($d -eq 0) { return @(0.0, 0.0, ($mx / 255.0)) }
    $s = $d / $mx
    if ($mx -eq $R) { $h = (60.0 * (($G - $B) / $d)) % 360 }
    elseif ($mx -eq $G) { $h = 60.0 * ((($B - $R) / $d) + 2) }
    else { $h = 60.0 * ((($R - $G) / $d) + 4) }
    if ($h -lt 0) { $h += 360 }
    return @($h, $s, ($mx / 255.0))
}

function Get-DominantColors {
    param([System.Drawing.Bitmap]$Bmp, [int]$Max = 14)

    # Sample on a stride so a 1242x878 board costs a fraction of a second.
    $step = [Math]::Max(1, [int]([Math]::Sqrt(($Bmp.Width * $Bmp.Height) / 60000.0)))
    $rect = New-Object System.Drawing.Rectangle 0, 0, $Bmp.Width, $Bmp.Height
    $data = $Bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                           [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $bytes = New-Object byte[] ($stride * $Bmp.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $Bmp.UnlockBits($data)

    $acc = @{}
    for ($y = 0; $y -lt $Bmp.Height; $y += $step) {
        $row = $y * $stride
        for ($x = 0; $x -lt $Bmp.Width; $x += $step) {
            $i = $row + $x * 4
            $b = $bytes[$i]; $g = $bytes[$i + 1]; $r = $bytes[$i + 2]
            # Quantise to 16 levels per channel: that absorbs JPEG noise and the
            # anti-aliased edges between swatches, while keeping genuinely
            # different swatches apart.
            $qr = [Math]::Min(255, [Math]::Round($r / 16) * 16)
            $qg = [Math]::Min(255, [Math]::Round($g / 16) * 16)
            $qb = [Math]::Min(255, [Math]::Round($b / 16) * 16)
            $lum = 0.299 * $r + 0.587 * $g + 0.114 * $b
            # Near-white and near-black are page background, not swatches.
            if ($lum -ge 246 -or $lum -le 8) { continue }
            $key = "$qr,$qg,$qb"
            if ($acc.ContainsKey($key)) {
                $e = $acc[$key]
                $e.n++; $e.sx += $x; $e.sy += $y
                $e.sr += $r; $e.sg += $g; $e.sb += $b
            } else {
                $acc[$key] = [pscustomobject]@{ n = 1; sx = $x; sy = $y; sr = $r; sg = $g; sb = $b }
            }
        }
    }
    if ($acc.Count -eq 0) { return @() }

    $total = ($acc.Values | Measure-Object -Property n -Sum).Sum
    $top = $acc.Values |
        Where-Object { $_.n / $total -ge 0.004 } |       # >= 0.4% of the board
        Sort-Object -Property n -Descending |
        Select-Object -First $Max
    # Reading order (top-to-bottom, then left-to-right) is how a palette board is
    # meant to be scanned, and it is what makes the transcribed order sensible.
    $top | Sort-Object @{Expression = { $_.sy / $_.n }}, @{Expression = { $_.sx / $_.n }}
}

$i = 0
foreach ($b in $boards) {
    $i++
    $jpg = Join-Path $out ("board{0}.jpg" -f $i)
    if (-not (Test-Path $jpg)) {
        Write-Host ("downloading board {0} ({1})..." -f $i, $b.style) -ForegroundColor DarkGray
        Invoke-WebRequest -Uri $b.url -OutFile $jpg -UseBasicParsing -TimeoutSec 60
    }
    $bmp = [System.Drawing.Bitmap]::FromFile($jpg)
    try {
        $cols = @(Get-DominantColors -Bmp $bmp -Max 22)
        $n = ($cols | Measure-Object -Property n -Sum).Sum
        # A board is a UI mockup, so its greys are page chrome (card fill,
        # placeholder text) rather than theme colours. Split the clusters by
        # saturation: the chromatic ones are the palette, the greys are not.
        $chroma = @()
        $neutral = @()
        foreach ($c in $cols) {
            $r = [int][Math]::Round($c.sr / $c.n); $g = [int][Math]::Round($c.sg / $c.n); $b = [int][Math]::Round($c.sb / $c.n)
            $hsv = Get-Hsv -R $r -G $g -B $b
            $entry = [pscustomobject]@{
                hex = '#{0:X2}{1:X2}{2:X2}' -f $r, $g, $b
                pct = 100.0 * $c.n / $n
                s = $hsv[1]; l = $hsv[2]; h = $hsv[0]
                cx = $c.sx / $c.n; cy = $c.sy / $c.n
            }
            if ($hsv[1] -ge 0.14) { $chroma += $entry } else { $neutral += $entry }
        }
        Write-Host ("`n[{0}] {1}  ({2}x{3}, {4} chromatic / {5} grey)" -f `
            $i, $b.style, $bmp.Width, $bmp.Height, $chroma.Count, $neutral.Count) -ForegroundColor Cyan
        Write-Host '  -- theme colours (by saturation) --' -ForegroundColor DarkGray
        foreach ($c in ($chroma | Sort-Object -Property s -Descending)) {
            Write-Host ("    {0}  s={1,4:N2} l={2,4:N2}  {3,5:N1}%  at ({4,4:N0},{5,4:N0})" -f `
                $c.hex, $c.s, $c.l, $c.pct, $c.cx, $c.cy)
        }
        Write-Host '  -- greys (page chrome, not palette) --' -ForegroundColor DarkGray
        foreach ($c in ($neutral | Sort-Object -Property pct -Descending)) {
            Write-Host ("    {0}  l={1,4:N2}  {2,5:N1}%" -f $c.hex, $c.l, $c.pct) -ForegroundColor DarkGray
        }
        if ($Show -contains $i) {
            $w = 480
            $small = New-Object System.Drawing.Bitmap $w, ([int]($w * $bmp.Height / $bmp.Width))
            $gg = [System.Drawing.Graphics]::FromImage($small)
            $gg.InterpolationMode = 'HighQualityBicubic'
            $gg.DrawImage($bmp, 0, 0, $small.Width, $small.Height)
            $small.Save((Join-Path $out ("preview{0}.png" -f $i)), [System.Drawing.Imaging.ImageFormat]::Png)
            $gg.Dispose(); $small.Dispose()
        }
    } finally {
        $bmp.Dispose()
    }
}
