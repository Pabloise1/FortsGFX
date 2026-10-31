-- FortsGFX test bench (sea, showcase): the -template water- map (High Seas) with the test bench script.
-- Sandbox only. Built by install.ps1 from testbench\ in the FortsGFX folder.
AllowEdit = true
RequiresMoonshot = false
RequiresHighSeas = true

-- Determines where the map will show up
Sandbox = true
Skirmish = false
Multiplayer = false

-- Shown in map selection screen
Author = L"FortsGFX"
DescLine1 = L"FortsGFX showcase: every feature switched OFF, then ON, in a real battle"
DescLine2 = L"F6 runs the sequence again"

dlc2_WaterLevel = 5200

Mods = { "weapon_pack", "dlc2", "unlimited-metal", "unlimited-energy", "fast-build" }

FORTSGFX_BENCH_SEA = true

FORTSGFX_SHOWCASE = true
