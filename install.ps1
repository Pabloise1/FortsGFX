# Copies opengl32.dll (build\ in the source tree, or next to this script in a release) next to Forts.exe, or
# removes it again with -Uninstall. Forts is found through Steam's library folders unless -GameDir is given.
# -OutputDir writes FortsGFX.ini so logs/captures go somewhere other than <game>\FortsGFX.
param(
    [switch]$Uninstall,
    [string]$GameDir = "",
    [string]$OutputDir = ""
)
$ErrorActionPreference = 'Stop'

# Every Steam library (libraryfolders.vdf) under every Steam install the registry knows, default folder first.
function Find-Forts {
    $roots = @("${env:ProgramFiles(x86)}\Steam")
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $p = Get-ItemProperty $key -ErrorAction SilentlyContinue
        if ($p.SteamPath) { $roots += $p.SteamPath }
        if ($p.InstallPath) { $roots += $p.InstallPath }
    }
    $libs = @()
    foreach ($r in $roots) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $libs += [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
        }
    }
    foreach ($l in $libs) {
        $d = Join-Path $l 'steamapps\common\Forts'
        if (Test-Path -LiteralPath (Join-Path $d 'Forts.exe')) { return (Resolve-Path -LiteralPath $d).Path }
    }
}

if (-not $GameDir) {
    $GameDir = Find-Forts
    if (-not $GameDir) { throw "Couldn't find Forts in your Steam libraries. Run it again with the folder that has Forts.exe in it: install.ps1 -GameDir 'D:\Games\Forts'" }
    "Forts found in $GameDir"
}
$dst = Join-Path $GameDir 'opengl32.dll'
$ini = Join-Path $GameDir 'FortsGFX.ini'

function Test-OurDll($path) {
    (Test-Path $path) -and ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)) -match 'FortsGFX')
}

if ($Uninstall) {
    if (-not (Test-Path $dst)) { "Nothing to remove: $dst does not exist"; return }
    if (-not (Test-OurDll $dst)) { throw "$dst is not a FortsGFX build - leaving it alone" }
    Remove-Item $dst
    if (Test-Path $ini) { Remove-Item $ini }
    "Removed $dst"
    $funDst = Join-Path $GameDir 'FortsGFX_fun'
    if (Test-Path (Join-Path $funDst 'miko_normal.png')) { Remove-Item -LiteralPath $funDst -Recurse; "Removed $funDst" }
    foreach ($name in 'fortsgfx testbench', 'fortsgfx testbench sea', 'fortsgfx showcase', 'fortsgfx showcase sea') {
        $m = Join-Path $GameDir "data\maps\$name"
        if ((Test-Path -LiteralPath "$m\$name.lua") -and ((Get-Content -LiteralPath "$m\$name.lua" -TotalCount 1) -match 'FortsGFX test bench')) {
            Remove-Item -LiteralPath $m -Recurse; "Removed the $name map"
        }
    }
    return
}

if (-not (Test-Path (Join-Path $GameDir 'Forts.exe'))) { throw "Forts.exe not found in $GameDir" }
if ((Test-Path $dst) -and -not (Test-OurDll $dst)) { throw "$dst exists and is not a FortsGFX build - not overwriting" }
$dll = @((Join-Path $PSScriptRoot 'build\opengl32.dll'), (Join-Path $PSScriptRoot 'opengl32.dll')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dll) { throw "opengl32.dll not found next to this script (or in build\ - run build.cmd first)" }
# A running Forts keeps the DLL locked; a loaded DLL can still be renamed, so move it aside.
$aside = Join-Path $GameDir 'opengl32.fortsgfx-old.dll'
if (Test-Path $aside) { Remove-Item $aside -ErrorAction SilentlyContinue }
if (Test-Path $dst) {
    try { Remove-Item $dst -ErrorAction Stop } catch { Rename-Item $dst (Split-Path $aside -Leaf); "Forts is running - new build takes effect on restart" }
}
try { Copy-Item $dll $dst -Force }
catch [UnauthorizedAccessException] { throw "Windows didn't allow writing to $GameDir. Right-click install.cmd and choose 'Run as administrator'." }
"Installed $dst"

# The milk miko joke's art and voice ([Fun] MilkMiko): fun\anime -> <game>\FortsGFX_fun
$fun = Join-Path $PSScriptRoot 'fun\anime'
if (Test-Path $fun) {
    $funDst = Join-Path $GameDir 'FortsGFX_fun'
    New-Item -ItemType Directory -Force $funDst | Out-Null
    Copy-Item (Join-Path $fun '*') $funDst -Force
    "Installed the milk miko's art and voice ($funDst)"
}

# The test bench maps (Sandbox): the game's own -template- maps plus the bench script and its capture markers.
$bench = Join-Path $PSScriptRoot 'testbench'
$maps = Join-Path $GameDir 'data\maps'
if (Test-Path $bench) {
    $benchMaps = @(
        @{ Name = 'fortsgfx testbench'; From = '-template-'; Header = 'land_header.lua'; Title = 'FortsGFX Test Bench' },
        @{ Name = 'fortsgfx testbench sea'; From = '-template water-'; Header = 'sea_header.lua'; Title = 'FortsGFX Test Bench (sea)' },
        @{ Name = 'fortsgfx showcase'; From = '-template-'; Header = 'showcase_land_header.lua'; Title = 'FortsGFX Showcase' },
        @{ Name = 'fortsgfx showcase sea'; From = '-template water-'; Header = 'showcase_sea_header.lua'; Title = 'FortsGFX Showcase (sea)' })
    $utf8 = New-Object Text.UTF8Encoding $false
    foreach ($m in $benchMaps) {
        $src = Join-Path $maps $m.From
        if (-not (Test-Path -LiteralPath $src)) { "Test bench: the $($m.From) map is missing - $($m.Name) skipped"; continue }
        $out = Join-Path $maps $m.Name
        New-Item -ItemType Directory -Force $out | Out-Null
        foreach ($ext in '.fwe', '.spr') { Copy-Item -LiteralPath (Join-Path $src ($m.From + $ext)) (Join-Path $out ($m.Name + $ext)) -Force }
        Get-ChildItem -LiteralPath $src -Filter 'preview-*.jpg' | ForEach-Object { Copy-Item -LiteralPath $_.FullName (Join-Path $out $_.Name.Replace($m.From, $m.Name)) -Force }
        $lua = [IO.File]::ReadAllText((Join-Path $bench $m.Header)) + "`r`n" + [IO.File]::ReadAllText((Join-Path $bench 'bench_script.lua'))
        [IO.File]::WriteAllText((Join-Path $out ($m.Name + '.lua')), $lua, $utf8)
        $title = "DisplayName =`r`n{`r`n`t['English'] = L`"$($m.Title)`",`r`n}`r`n"
        [IO.File]::WriteAllText((Join-Path $out 'displayname.lua'), $title, [Text.Encoding]::Unicode)   # UTF-16 with BOM, like the game's
    }
    $fx = Join-Path $maps 'fortsgfx testbench\effects'
    New-Item -ItemType Directory -Force (Join-Path $fx 'media') | Out-Null
    Copy-Item (Join-Path $bench 'markers\effects\*.lua') $fx -Force
    Copy-Item (Join-Path $bench 'markers\effects\media\*.png') (Join-Path $fx 'media') -Force
    "Installed the test bench and showcase maps (Sandbox: FortsGFX Test Bench / Showcase, each also at sea)"
}
if ($OutputDir) {
    New-Item -ItemType Directory -Force $OutputDir | Out-Null
    "[FortsGFX]", "OutputDir=$OutputDir" | Set-Content -Encoding ascii $ini
    "Logs and captures -> $OutputDir"
}
