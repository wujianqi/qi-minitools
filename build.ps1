# build.ps1 - produces the standalone executables. Packing only.
#
#   .\build.ps1               build all of them into .\bin
#   .\build.ps1 -Only shot    build just shot.exe (also -Only picker, envtool, slice)
#   .\build.ps1 -Dbg          unoptimised build with debug info
#   .\build.ps1 -RegenIcon    re-render all four res\logo-*.ico from logo.svg first
#
# The tests are .\test.ps1 now, and it filters by program too, so editing one
# program costs one relink and that program's tests:
#
#   .\build.ps1 -Only shot ; .\test.ps1 shot
#
# -Test still builds and then runs the tests, forwarding to .\test.ps1. It is
# a shim for the old single-script habit; prefer calling the two separately.
#
# Every executable is statically linked against its own copy of the shared
# code, so the .exe files can be copied anywhere on their own.

[CmdletBinding()]
param(
    [string[]]$Only,
    [switch]$Test,
    [switch]$Dbg,
    [switch]$RegenIcon,
    [string]$Zig = "F:\sdk\zig\zig.exe"
)

$ErrorActionPreference = 'Stop'
# zig writes warnings to stderr, which PowerShell turns into terminating
# errors under 'Stop'; the build judges success by exit code instead.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $root 'src'
$bin  = Join-Path $root 'bin'
$obj  = Join-Path $root 'build\obj'
$res  = Join-Path $root 'res'

if (-not (Test-Path $Zig)) {
    # fall back to zig on PATH
    $cmd = Get-Command zig -ErrorAction SilentlyContinue
    if ($cmd) { $Zig = $cmd.Source }
    else { throw "zig not found at $Zig (expected the Zig toolchain in F:\sdk\zig)" }
}

New-Item -ItemType Directory -Force -Path $bin, $obj, $res | Out-Null

$common = @(
    "$src\common\winutil.c"
    "$src\common\uikit.c"
    "$src\common\tray.c"
    "$src\common\trayicon.c"
    "$src\common\about.c"
)

# Each target carries the fill its icon is drawn in, as RRGGBB without the '#'.
# These are the same four values as APP_ICON_COLOR_* in src\common\trayicon.h -
# one list here so a colour is changed in one place, and the check at the bottom
# compares the icon actually embedded against it, so a half-applied change to
# the other side fails the build instead of shipping. Green and blue are held
# back for programs to come, so the four step violet -> fuchsia -> rose ->
# orange instead.
$targets = @(
    @{ name = 'shot';    color = '7C3AED'; files = @("$src\shot\main.c", "$src\shot\editor.c", "$src\shot\draw.c",
                                   "$src\shot\chrome.c", "$src\shot\surface.c", "$src\shot\output.c",
                                   "$src\shot\hotkey.c") },
    @{ name = 'picker';  color = 'C026D3'; files = @("$src\picker\picker.c", "$src\picker\eyedrop.c",
                                   "$src\picker\color.c", "$src\picker\palette.c",
                                   "$src\picker\wheel.c") },
    @{ name = 'slice';   color = 'E11D48'; files = @("$src\slice\slice.c", "$src\slice\tree.c",
                                   "$src\slice\image.c") },
    @{ name = 'envtool'; color = 'EA580C'; files = @("$src\envtool\envtool.c", "$src\envtool\envreg.c",
                                   "$src\envtool\presets.c") }
)

# -Only is what makes the short loop in the header cheap: editing
# src\shot\editor.c only changes shot.exe, so nothing else has to be
# relinked. The icon bootstrap below needs shot.exe whichever target was
# asked for, so that one target is kept before the list is narrowed - and the
# icons are shared by all four, so -RegenIcon works on the whole table even
# when only one program is being built.
$shotTarget = @($targets | Where-Object { $_.name -eq 'shot' })[0]
$allTargets = $targets
if ($Only) {
    $names   = @($targets | ForEach-Object { $_.name })
    $unknown = @($Only | Where-Object { $names -notcontains $_ })
    if ($unknown.Count) {
        throw "unknown -Only target(s): $($unknown -join ', ') (expected $($names -join ', '))"
    }
    $targets = @($targets | Where-Object { $Only -contains $_.name })
}

$libs = @(
    '-luser32', '-lgdi32', '-lgdiplus', '-lshell32', '-lshlwapi', '-lcomctl32',
    '-lole32', '-luuid', '-lpsapi', '-lcomdlg32', '-ladvapi32', '-lversion',
    '-lsetupapi', '-ldbghelp'
)

$optFlags = if ($Dbg) { @('-O0', '-g') } else { @('-O2') }

# -static keeps the runtime inside the exe: the result has no DLL side-car.
#
# --subsystem,windows is what keeps the console window away. The default is
# console, which makes every one of these spawn a black cmd window the
# moment it starts, even though every entry point is wWinMain and not one of
# them reads stdin. It is a linker-level decision, so no amount of window
# styling in the source can undo it. Do not "simplify" this back to -mwindows:
# `zig cc` accepts that spelling, ignores it, and only says
# "argument unused during compilation" - which the warning filter below eats,
# so the exes would quietly keep the black box.
#
# The exes re-attach to the parent console on their own
# (app_attach_parent_console in src\common\winutil.c) when started from a
# terminal, so --selftest and --verbose still print and still return an exit
# code. Started from Explorer there is no parent console, nothing is attached,
# and no window is ever created.
$linkFlags = @('-static', '-Wl,--subsystem,windows', '-municode')

# res\logo-<program>.ico is generated, not hand-drawn: it is logo.svg rendered
# by shot.exe's own --ico exporter, so the icon the shell shows and the one the
# tray draws are the same artwork. One per program, because each is filled with
# its own colour. That creates a chicken-and-egg on a fresh checkout - there is
# no .ico to embed until something has been built - so the first build (or an
# explicit -RegenIcon) compiles shot.exe without the resource script, runs the
# exporter once per program, and then carries on and embeds the results.
$staleIco = @($allTargets | Where-Object {
    -not (Test-Path (Join-Path $res "logo-$($_.name).ico"))
})
if ($RegenIcon -or $staleIco.Count) {
    if ($staleIco.Count) {
        Write-Host ("res\logo-{0}.ico missing - regenerating the icons from logo.svg" -f
                    (($staleIco | ForEach-Object { $_.name }) -join ', ')) -ForegroundColor Yellow
    }
    $regen = @('cc', '-target', 'x86_64-windows-gnu') + @($optFlags) +
             @("-I$src") + @($linkFlags) +
             @(@($shotTarget.files) + @($common)) +
             @('-o', (Join-Path $bin 'shot.exe')) + @($libs)
    $rout = & $Zig @regen 2>&1
    if ($LASTEXITCODE -ne 0) {
        $rout | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        throw "could not build shot.exe to regenerate res\logo-*.ico"
    }
    # Start-Process -Wait -PassThru, not `& shot.exe --ico $ico`. The exes are
    # GUI-subsystem, and PowerShell's call operator does not wait for those -
    # it returns straight away and leaves $LASTEXITCODE unset, so a bare `&`
    # here would report success before the icon was written. Same reason the
    # -Test pass below uses Start-Process.
    foreach ($t in $allTargets) {
        $ico = Join-Path $res "logo-$($t.name).ico"
        $icoProc = Start-Process -FilePath (Join-Path $bin 'shot.exe') `
                                 -ArgumentList @('--ico', $ico, "#$($t.color)") -Wait -PassThru -NoNewWindow
        if ($icoProc.ExitCode -ne 0) {
            throw "shot.exe --ico failed for $($t.name) (exit $($icoProc.ExitCode))"
        }
        Write-Host ("  -> {0}  #{1}  ({2:N0} bytes)" -f $ico, $t.color, (Get-Item $ico).Length) -ForegroundColor Green
    }
}

Write-Host "`ncompiling resources..." -ForegroundColor Cyan
# One .res per program, because each embeds only its own logo: the resource
# script is three lines and its only variable is the file name, so it is written
# out here rather than kept as four near-identical files in res\. ASCII, not
# Set-Content - PowerShell 5.1 would add a UTF-8 BOM that `zig rc` has no
# reason to expect.
$resObj = @{}
foreach ($t in $targets) {
    $rcPath = Join-Path $obj "app-$($t.name).rc"
    $resObj[$t.name] = Join-Path $obj "app-$($t.name).res"
    $rcText = @"
/* generated by build.ps1 - the icon res\logo-$($t.name).ico carries, embedded
 * under the id src\common\trayicon.h loads. No #include here on purpose: the
 * only thing this file needs from windows.h is a resource id. */
#define APP_ICON_RES 1

APP_ICON_RES ICON "logo-$($t.name).ico"
"@
    [System.IO.File]::WriteAllText($rcPath, $rcText, [System.Text.Encoding]::ASCII)

    # `zig cc` no longer takes -rcfile (0.14 hands unknown options straight to
    # the clang driver, which rejects it), so the resource is compiled on its
    # own with `zig rc` and linked in as an object. The /i matters: it is what
    # lets the script say ICON "logo-*.ico" without knowing where the build was
    # run from.
    $rout = & $Zig rc "/fo$($resObj[$t.name])" "/i$res" $rcPath 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $resObj[$t.name])) {
        $rout | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        throw "zig rc failed on $rcPath"
    }
    Write-Host ("  -> {0}  ({1:N0} bytes)" -f $resObj[$t.name], (Get-Item $resObj[$t.name]).Length) -ForegroundColor Green
}

$failed = $false
foreach ($t in $targets) {
    $exe = Join-Path $bin ("{0}.exe" -f $t.name)
    $all = @($t.files) + @($common)
    Write-Host ("compiling {0}..." -f $t.name) -ForegroundColor Cyan
    # Build one argument array and splat it: mixing several @array splats with
    # literal arguments in a single native call misbehaves on PowerShell 5.
    # $resObj[$t.name] is what embeds that program's own logo - the one the
    # shell shows in Explorer, the taskbar and Alt+Tab.
    $argList = @('cc', '-target', 'x86_64-windows-gnu') +
               @($optFlags) + @("-I$src") + @($linkFlags) + @($all) +
               @($resObj[$t.name]) +
               @('-o', $exe) + @($libs)
    # zig writes warnings to stderr; under $ErrorActionPreference='Stop' that
    # would abort the build, so relax it and judge success by the exit code.
    $out = & $Zig @argList 2>&1
    $code = $LASTEXITCODE
    if ($out) {
        $out | Where-Object { $_ -notmatch 'unused during compilation' } |
            ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    }
    if ($code -ne 0) {
        Write-Host ("FAILED: {0}.exe (exit {1})" -f $t.name, $code) -ForegroundColor Red
        $failed = $true
        continue
    }
    $size = (Get-Item $exe).Length
    # lld-link drops a .pdb next to the output even without -g; keep bin/ to
    # exactly the built executables so it can be copied as a unit. A debug
    # build keeps its symbols beside the exe - that is the whole point of -Dbg.
    if (-not $Dbg) {
        Get-ChildItem $bin -Filter "$($t.name).pdb" -ErrorAction SilentlyContinue |
            Move-Item -Destination $obj -Force
    }
    Write-Host ("  -> {0}  ({1:N0} bytes)" -f $exe, $size) -ForegroundColor Green
}

if ($failed) { exit 1 }

# A freshly written .exe is not always readable straight away: a virus scanner
# or a shell that is still mapping the old image can hold it without sharing
# reads. Retrying for a moment is the difference between a flaky build and a
# wrong answer, and it matters because both checks below treat "read nothing" as
# a pass.
function Read-ExeBytes($path) {
    for ($try = 1; $try -le 6; $try++) {
        try { return [System.IO.File]::ReadAllBytes($path) }
        catch { Start-Sleep -Milliseconds 300 }
    }
    return $null
}

# Verify nothing outside the Windows system DLL set is imported, so the exes
# really are single-file and copy-anywhere.
Write-Host "`nverifying dependencies..." -ForegroundColor Cyan
$allowed = @('kernel32','user32','gdi32','gdiplus','shell32','shlwapi','comctl32',
             'ole32','uuid','psapi','comdlg32','advapi32','version','setupapi',
             'dbghelp','ntdll','oleaut32','imm32','msvcrt','ws2_32','crypt32','secur32')
$bad = $false
foreach ($t in $targets) {
    $exe = Join-Path $bin ("{0}.exe" -f $t.name)
    $bytes = Read-ExeBytes $exe
    if (-not $bytes) {
        Write-Host ("  {0}: could not read the executable" -f $t.name) -ForegroundColor Red
        $bad = $true
        continue
    }
    $text  = [System.Text.Encoding]::ASCII.GetString($bytes)
    # anchored so long api-ms-win-* names are not truncated by the length bound
    $imports = [regex]::Matches($text, '(?<![A-Za-z0-9_\-])[A-Za-z0-9_\-]{3,40}\.dll') |
               ForEach-Object { $_.Value.ToLower() } | Sort-Object -Unique
    $extra = $imports | Where-Object {
        $n = $_ -replace '\.dll$',''
        $n -notlike 'api-ms-win-*' -and $n -notlike 'ext-ms-*' -and ($allowed -notcontains $n)
    }
    if ($extra) {
        Write-Host ("  {0}: UNEXPECTED {1}" -f $t.name, ($extra -join ', ')) -ForegroundColor Red
        $bad = $true
    } else {
        Write-Host ("  {0}: only Windows system DLLs" -f $t.name) -ForegroundColor Green
    }
}
if ($bad) { exit 1 }

# Verify the icon really landed in each executable, in that program's own
# colour. A resource script that fails to compile or a .res that never gets
# linked does not fail the build - you just get the stock Windows application
# icon, silently - so this is the only thing standing between a broken
# resource script and four exes that look like every other program on the
# machine. The check is the icon the way a user sees it: pull it back out of
# the built .exe and count the pixels.
#
# It also counts the other three programmes' colours in the same icon. That is
# the point of the four fills: if two of them ever landed close enough to read
# as the same colour, this is where it shows up.
Write-Host "`nverifying the embedded icon..." -ForegroundColor Cyan
Add-Type -AssemblyName System.Drawing

# $palette[name] = the RRGGBB the table above asked for, as an r/g/b triple.
# Built from the whole table rather than the -Only subset: an icon is checked
# against every other programme's colour, not just the ones being built.
$palette = @{}
foreach ($t in $allTargets) {
    $v = [Convert]::ToInt32($t.color, 16)
    # One element per line, no commas: `-band` binds looser than `,`, so
    # @(a -band 1, b -band 2) is one -band applied to a two-element array, not
    # two masked numbers - which then indexes as $rgb[0] being an array.
    $palette[$t.name] = @(
        (($v -shr 16) -band 0xFF)
        (($v -shr 8) -band 0xFF)
        ($v -band 0xFF)
    )
}

# The same four colours live in src\common\trayicon.h, where the tray icons and
# the --ico exporter read them from. Two copies of a colour list drift, so the
# header is parsed and compared rather than trusted to a comment: a value
# changed on one side only fails here, before an .ico is even written.
$declared = @{}
foreach ($m in [regex]::Matches(
        [System.IO.File]::ReadAllText((Join-Path $src 'common\trayicon.h')),
        'APP_ICON_COLOR_([A-Z]+)\s+0xFF([0-9A-Fa-f]{6})u')) {
    $declared[$m.Groups[1].Value.ToLower()] = $m.Groups[2].Value.ToLower()
}
$mismatch = @()
foreach ($t in $allTargets) {
    $want = $t.color.ToLower()
    $have = $declared[$t.name]
    if ($have -ne $want) {
        $mismatch += ("{0}: trayicon.h has #{1}, the table says #{2}" -f
                      $t.name, $(if ($have) { $have } else { '<missing>' }), $want)
    }
}
if ($mismatch.Count) {
    $mismatch | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host "  the icon colours disagree - fix src\common\trayicon.h" -ForegroundColor Red
    exit 1
}
Write-Host ("  trayicon.h agrees on all {0} icon colours" -f $allTargets.Count) -ForegroundColor Green

# Counts pixels close to one colour, within 28 per channel.
function Get-ColorCount($bmp, $rgb) {
    if (-not $bmp) { return 0 }
    $n = 0
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            $c = $bmp.GetPixel($x, $y)
            if ([Math]::Abs($c.R - $rgb[0]) -le 28 -and
                [Math]::Abs($c.G - $rgb[1]) -le 28 -and
                [Math]::Abs($c.B - $rgb[2]) -le 28) { $n++ }
        }
    }
    return $n
}

# Reads an icon the way the shell reads it - $px 0 for the icon associated with
# the file, otherwise that size out of an .ico - and returns
# @{ bmp; count; read }. `count` is how many pixels matched $rgb and `read`
# says whether the last attempt produced a bitmap at all; the caller owns $bmp.
#
# The retry here is not decoration. A file written moments earlier can come back
# unreadable, and - the case that has bitten this check - perfectly readable
# and stale, still carrying the icon of the build before it, so "the call did
# not throw" is not the same answer as "this is the icon". Retrying until the
# pixel count clears $want is what catches that; the dependency check above
# needs the same patience for the same reason.
function Read-Icon($path, $px, $rgb, $want) {
    $bmp = $null; $count = 0; $read = $false
    for ($try = 1; $try -le 6; $try++) {
        $icon = $null
        try {
            $icon  = if ($px) { [System.Drawing.Icon]::new($path, $px, $px) }
                     else     { [System.Drawing.Icon]::ExtractAssociatedIcon($path) }
            $bmp   = $icon.ToBitmap()
            $count = Get-ColorCount $bmp $rgb
            $read  = $true
            $icon.Dispose(); $icon = $null
            if ($count -ge $want) { break }
        } catch { $read = $false }
        if ($icon) { $icon.Dispose(); $icon = $null }
        if ($bmp)  { $bmp.Dispose();  $bmp  = $null }
        Start-Sleep -Milliseconds 300
    }
    return @{ bmp = $bmp; count = $count; read = $read }
}

$bad = $false
foreach ($t in $targets) {
    $exe = Join-Path $bin ("{0}.exe" -f $t.name)
    $r = Read-Icon $exe 0 $palette[$t.name] 100
    # How much of anyone else's colour is in here. A couple of dozen is
    # antialiasing along the edge; more than that is another logo.
    $foreign = @()
    if ($r.bmp) {
        foreach ($other in $palette.Keys) {
            if ($other -eq $t.name) { continue }
            $c = Get-ColorCount $r.bmp $palette[$other]
            if ($c -gt 25) { $foreign += ("{0}:{1}" -f $other, $c) }
        }
        $r.bmp.Dispose()
    }
    if (-not $r.read) {
        Write-Host ("  {0}: could not read the icon out of the executable" -f $t.name) -ForegroundColor Red
        $bad = $true
    } elseif ($r.count -lt 100) {
        # The artwork fills most of the 32x32 box, so a couple of hundred pixels
        # is the logo and near zero is "some other icon is there" - including a
        # stale read of the icon this exe used to carry.
        Write-Host ("  {0}: no #{1} logo embedded ({2} px)" -f $t.name, $t.color, $r.count) -ForegroundColor Red
        $bad = $true
    } elseif ($foreign.Count) {
        Write-Host ("  {0}: #{1} logo, but also {2}" -f $t.name, $t.color, ($foreign -join ' ')) -ForegroundColor Red
        $bad = $true
    } else {
        Write-Host ("  {0}: 32px logo, #{1}, {2} px, no other program's colour" -f $t.name, $t.color, $r.count) -ForegroundColor Green
    }
}
# The 16/24/48 entries are DIBs inside each .ico, and the taskbar reads the 16
# one - nothing else in the build would notice if that entry were malformed, so
# decode them here the same way the shell does. (64 and up are PNG-compressed
# entries, which System.Drawing.Icon refuses to parse even though Windows
# reads them; LoadImageW is what covers those.)
foreach ($t in $allTargets) {
    $ico = Join-Path $res "logo-$($t.name).ico"
    foreach ($px in 16, 24, 48) {
        $r = Read-Icon $ico $px $palette[$t.name] 20
        if ($r.bmp) { $r.bmp.Dispose() }
        $ok = $r.read -and ($r.count -ge 20)
        if (-not $ok) { $bad = $true }
        $tone = if ($ok) { 'Green' } else { 'Red' }
        Write-Host ("  logo-{0}.ico {1}px: {2} px of #{3}" -f $t.name, $px, $r.count, $t.color) -ForegroundColor $tone
    }
}
if ($bad) {
    Write-Host "  icon resource missing or not the logo - check the resource step above" -ForegroundColor Red
    exit 1
}

# The tests used to live right here, at the end of this script. They are in
# .\test.ps1 now, and that is not only tidiness: the tray and editor probes
# drive the real mouse, take the foreground and run for minutes, so a change
# to one program should not have to pay for every probe on the machine. What
# is left here only forwards the old entry point.
if ($Test) {
    Write-Host "`nthe tests moved to .\test.ps1 - running them now" -ForegroundColor Yellow
    if ($Only) { & (Join-Path $root 'test.ps1') -Target $Only }
    else       { & (Join-Path $root 'test.ps1') }
    exit $LASTEXITCODE
}

Write-Host "`nbuild complete." -ForegroundColor Green
exit 0
