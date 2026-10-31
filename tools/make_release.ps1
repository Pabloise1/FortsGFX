# Packs build\opengl32.dll and what install.ps1 needs into dist\FortsGFX-<version>.zip, the file a GitHub release carries.
#   powershell -File tools\make_release.ps1 1.0
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$name = "FortsGFX-$Version"
$stage = Join-Path $root "dist\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage 'fun') | Out-Null

Copy-Item (Join-Path $root 'build\opengl32.dll') $stage
foreach ($f in 'install.ps1', 'install.cmd', 'uninstall.cmd', 'LICENSE') { Copy-Item (Join-Path $root $f) $stage }
Copy-Item (Join-Path $root 'fun\anime') (Join-Path $stage 'fun\anime') -Recurse
Copy-Item (Join-Path $root 'testbench') (Join-Path $stage 'testbench') -Recurse
Get-ChildItem $stage -Recurse -Filter *.py | Remove-Item

@"
FortsGFX $Version - a graphics overhaul for Forts
https://github.com/Pabloise1/FortsGFX

Install:   double-click install.cmd (it finds Forts through Steam), then start Forts.
           Or copy opengl32.dll into the Forts folder, next to Forts.exe.
Uninstall: double-click uninstall.cmd, or delete opengl32.dll from the Forts folder.

In game:   Insert = control panel, Home = Lab bar, F7 / F8 = change look, F9 = before / after,
           End = photo mode.

Forts has to use its OpenGL renderer (the default). Settings are saved in FortsGFX.ini in the Forts folder,
logs in the FortsGFX folder there.
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'README.txt')

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
"Wrote $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)
