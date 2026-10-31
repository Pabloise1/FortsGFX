-- FortsGFX test bench (land): the -template- map with a script that runs a sequence of events and
-- has FortsGFX capture each one. Sandbox only. Built by install.ps1 from testbench\ in the FortsGFX folder.
Mods = { "unlimited-metal", "unlimited-energy", "fast-build" }
AllowEdit = true

-- Determines where the map will show up
Sandbox = true
Skirmish = false
Multiplayer = false

-- Shown in map selection screen
Author = L"FortsGFX"
DescLine1 = L"Scripted test bench: builds, fires, burns and captures by itself"
DescLine2 = L"F6 runs the sequence again"

FORTSGFX_BENCH_SEA = false
