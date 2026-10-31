
-- ================================================================ FortsGFX test bench
-- Runs by itself a moment after the battle starts: frames the camera, builds a test tower, fires
-- real projectiles, sets fires, calls lightning, damages devices. At each beat it drops an invisible
-- marker sprite (effects/fortsgfx_capture_NN_name.lua); FortsGFX (opengl32.dll) sees its texture and
-- saves a capture named after the beat: E:\FortsGFX\capture_NNN\label.txt, with the 2.5 s before it
-- in history\. Beats are 6 s apart so each capture is written before the next one.
-- Press F6 to run it again (build something first if you like). Progress is written to the console.

local MARKERS = "maps/fortsgfx testbench/effects/"   -- the markers live in the land map's folder
local bench = { run = 0, cam = nil }

local function Say(fmt, ...)
    local ok, msg = pcall(string.format, fmt, ...)
    pcall(Log, "[bench] " .. (ok and msg or tostring(fmt)))
end

-- every game call goes through Try: a missing function or a refused build is logged, not fatal
local function Try(what, f, ...)
    if type(f) ~= "function" then Say("%s: not available here", what) return nil end
    local ok, r = pcall(f, ...)
    if not ok then Say("%s failed: %s", what, tostring(r)) return nil end
    return r
end

local function P(x, y) return Vec3(x, y, 0) end

-- ---------------------------------------------------------------- the two forts
local function FortOf(side)
    local f = { side = side, nodes = {}, minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9 }
    local n = Try("NodeCount", NodeCount, side) or 0
    local seen = {}
    for i = 0, n do
        local id = Try("GetNodeId", GetNodeId, side, i)
        if id and id >= 0 and not seen[id] then
            local p = Try("NodePosition", NodePosition, id)
            if p then
                seen[id] = true
                table.insert(f.nodes, { id = id, x = p.x, y = p.y })
                f.minX = math.min(f.minX, p.x); f.maxX = math.max(f.maxX, p.x)
                f.minY = math.min(f.minY, p.y); f.maxY = math.max(f.maxY, p.y)
            end
        end
    end
    if #f.nodes == 0 then f.minX, f.maxX, f.minY, f.maxY = -500 * side, -500 * side + 400, -400, 0 end
    f.cx = (f.minX + f.maxX) * 0.5
    f.cy = (f.minY + f.maxY) * 0.5
    f.ground = f.maxY                -- world y grows down: the lowest node is the ground
    f.top = f.minY
    Say("fort %d: %d nodes, x %.0f..%.0f, y %.0f..%.0f", side, #f.nodes, f.minX, f.maxX, f.minY, f.maxY)
    return f
end

local function Forts()
    local a, b = FortOf(1), FortOf(2)
    a.dir = a.cx < b.cx and -1 or 1    -- away from the enemy
    b.dir = -a.dir
    return a, b
end

-- ---------------------------------------------------------------- camera and markers
local function Look(x, y, height, secs)
    bench.cam = P(x, y)
    Try("SetNamedScreenByHeight", SetNamedScreenByHeight, "fortsgfx_bench", bench.cam, height)
    Try("RestoreScreen", RestoreScreen, "fortsgfx_bench", secs or 1.0, 0.35, true)
end

local function Mark(label)
    Say("beat %s", label)
    local pos = Try("GetCameraFocus", GetCameraFocus) or bench.cam or P(0, 0)
    Try("marker " .. label, SpawnEffect, MARKERS .. "fortsgfx_capture_" .. label .. ".lua", pos)
end

-- ---------------------------------------------------------------- shooting
local function Gravity(save, team)
    local g = Try("gravity", GetProjectileTypeGravity, save, team)
    if type(g) ~= "number" then g = 981 end
    return g
end

-- a projectile that lands on `to` after `t` seconds (world y grows down, gravity pulls +y)
local function Shoot(save, team, from, to, t)
    local g = Gravity(save, team)
    local vx = (to.x - from.x) / t
    local vy = (to.y - from.y) / t - 0.5 * g * t
    return Try("projectile " .. save, dlc2_CreateProjectile, save, save, team, P(from.x, from.y), P(vx, vy), 30)
end

local function Rand(a, b) return a + (b - a) * math.random() end
local function Target(f) return { x = Rand(f.minX + 40, f.maxX - 40), y = Rand(f.top + 20, f.ground - 30) } end

-- ---------------------------------------------------------------- the test tower
-- A ladder of square cells beside fort 1, away from the enemy: bracing sides, braced diagonals,
-- back-bracing panels, an armour outer wall and a platform on top for a laser and a minigun.
local function BuildTower(f)
    local team = f.side
    local ground = {}
    for _, n in ipairs(f.nodes) do if n.y >= f.ground - 8 then table.insert(ground, n) end end
    table.sort(ground, function(a, b) return a.x * f.dir > b.x * f.dir end)   -- outermost first
    local base = ground[1] or f.nodes[1]
    if not base then Say("tower: no node to build from") return end
    local cell, x0, y0 = 110, base.x, base.y
    local a = Try("tower node", CreateNode, team, "bracing", base.id, P(x0 + f.dir * cell, y0))
    local b = a and Try("tower node", CreateNode, team, "bracing", a, P(x0 + f.dir * 2 * cell, y0))
    if not (a and b) or a < 0 or b < 0 then Say("tower: no room on the ground beside fort %d", team) return end
    bench.tower = { a = a, b = b, x = x0 + f.dir * 1.5 * cell, ground = y0, levels = 0 }
    for k = 1, 5 do
        local na = Try("tower node", CreateNode, team, "bracing", a, P(x0 + f.dir * cell, y0 - k * cell))
        local nb = Try("tower node", CreateNode, team, k % 2 == 0 and "armour" or "bracing", b, P(x0 + f.dir * 2 * cell, y0 - k * cell))
        if not (na and nb) or na < 0 or nb < 0 then break end
        Try("tower link", CreateLink, team, "bracing", na, nb)
        Try("tower link", CreateLink, team, k % 2 == 0 and "backbracing" or "bracing", a, nb)
        Try("tower link", CreateLink, team, "backbracing", b, na)
        a, b = na, nb
        bench.tower.levels = k
        bench.tower.top = y0 - k * cell
    end
    bench.tower.topA, bench.tower.topB = a, b
    -- weapons on the top platform (8 = CREATEDEVICEFLAG_FORCEENABLE)
    bench.laser = Try("laser", CreateDeviceWithFlags, team, "laser", a, b, 0.5, 8, 0)
    Say("tower: %d levels, laser %s", bench.tower.levels, tostring(bench.laser))
end

-- ---------------------------------------------------------------- the sequence
local function Beats(sea)
    local f1, f2 = Forts()
    local wide = math.max(math.abs(f2.cx - f1.cx) * 1.6, 2000)
    local beats = {}
    local function at(t, fn) table.insert(beats, { t = t, fn = fn }) end
    local near = function(f) return { x = f.cx - f.dir * 700, y = f.top - 200 } end   -- a launch point in front of f

    at(0.0, function() Look((f1.cx + f2.cx) * 0.5, (f1.cy + f2.cy) * 0.5 - 150, wide * 0.6) end)
    at(2.0, function() Mark("01_overview") end)
    at(2.5, function() if not sea then BuildTower(f1) end; Look(f1.cx + f1.dir * 300, f1.cy - 150, 1500) end)
    at(8.0, function() Mark("02_tower") end)

    at(9.0, function() Look(f2.cx, f2.cy - 100, 1400) end)
    for i = 0, 24 do at(12.5 + i * 0.06, function() local s = near(f2); Shoot("minigun", 1, s, Target(f2), 0.35) end) end
    at(14.0, function() Mark("03_minigun") end)

    for i = 0, 3 do at(19.2 + i * 0.2, function() local s = near(f2); Shoot("cannon", 1, s, Target(f2), 0.6) end) end
    at(20.0, function() Mark("04_cannon") end)

    for i = 0, 7 do at(24.0 + i * 0.12, function() local s = { x = f2.cx - f2.dir * 1600, y = f2.ground - 50 }; Shoot("mortar", 1, s, Target(f2), 2.2) end) end
    at(26.5, function() Mark("05_mortar") end)

    for i = 0, 5 do at(31.0 + i * 0.15, function() local s = near(f2); Shoot("missile", 1, s, Target(f2), 1.6) end) end
    at(32.4, function() Mark("06_missile_trails") end)

    -- the big warhead, twice: one to see it falling, one to see it land
    at(37.6, function() Look(f2.cx, f2.cy - 500, 2200) end)
    at(38.0, function() Shoot("missile2", 1, { x = f2.cx - f2.dir * 300, y = f2.top - 2200 }, { x = f2.cx, y = f2.cy }, 2.2) end)
    at(39.2, function() Mark("07_warhead_falling") end)
    at(43.0, function() Shoot("missile2", 1, { x = f2.cx - f2.dir * 300, y = f2.top - 2200 }, { x = f2.cx, y = f2.cy }, 2.0) end)
    at(45.1, function() Mark("08_warhead_impact") end)

    at(46.0, function() Look(f2.cx, f2.cy - 100, 1300) end)
    at(47.0, function()
        for i = 1, 5 do Try("IgniteFire", IgniteFire, P(Rand(f2.minX + 30, f2.maxX - 30), Rand(f2.top + 30, f2.ground - 40)), 60, 120, 2) end
    end)
    at(51.0, function() Mark("09_fire") end)

    at(52.0, function()
        local t = bench.tower
        if t and t.top then
            Look(t.x, (t.top + t.ground) * 0.5, 1200)
            Try("IgniteFire", IgniteFire, P(t.x, (t.top + t.ground) * 0.5), 80, 160, 1)
        else
            Look(f1.cx, f1.cy, 1200)
            Try("IgniteFire", IgniteFire, P(f1.cx, f1.cy), 80, 160, 1)
        end
    end)
    at(57.0, function() Mark("10_fire_inside_fort") end)

    at(58.0, function() Look(f2.cx, f2.cy - 300, 1800) end)
    for i = 0, 2 do
        at(62.4 + i * 0.15, function()
            local x = Rand(f2.minX, f2.maxX)
            local fn = SpawnLightning or dlc3_SpawnLightning
            Try("lightning", fn, "mods/dlc3/effects/lightningbolt.lua", P(x + Rand(-200, 200), f2.top - 1800), P(x, f2.top))
        end)
    end
    at(63.0, function() Mark("11_lightning") end)

    at(64.0, function() Look((f1.cx + f2.cx) * 0.5, (f1.cy + f2.cy) * 0.5 - 150, wide * 0.5) end)
    at(67.5, function()
        if bench.laser and bench.laser >= 0 then Try("fire laser", FireWeapon, bench.laser, P(f2.cx, f2.cy), 0, 0) end
    end)
    at(69.0, function() Mark("12_laser") end)

    at(70.0, function()
        Look(f2.cx, f2.cy - 100, 1300)
        local n = Try("GetDeviceCount", GetDeviceCount, 2) or 0
        for i = 0, n - 1 do
            local id = Try("GetDeviceId", GetDeviceId, 2, i)
            if id and id >= 0 then Try("damage", ApplyDamageToDevice, id, 250) end
        end
    end)
    at(75.0, function() Mark("13_device_smoke") end)

    -- everything at once: the bloom stress test
    at(76.0, function() Look(f2.cx, f2.cy - 200, 1800) end)
    for i = 0, 11 do
        at(78.0 + i * 0.22, function()
            local s = near(f2)
            Shoot(i % 3 == 0 and "cannon" or i % 3 == 1 and "missile" or "mortar", 1,
                  i % 3 == 2 and { x = f2.cx - f2.dir * 1600, y = f2.ground - 50 } or s, Target(f2), i % 3 == 2 and 2.0 or 0.8)
        end)
    end
    at(81.0, function() Mark("14_barrage_bloom") end)

    at(82.0, function() Look((f1.cx + f2.cx) * 0.5, (f1.cy + f2.cy) * 0.5 - 150, wide * 0.6) end)
    at(87.0, function() Mark("15_aftermath") end)
    at(88.0, function() Try("EnableCameraControls", EnableCameraControls, true); Say("done - press F6 to run it again") end)
    return beats
end

-- ---------------------------------------------------------------- the showcase
-- FortsGFX's tour, driven from here: each TourMark switches the overlay to tour chapter NN (it flips that
-- feature OFF / ON every 5 s with a badge) while real shells, missiles and fires give it something to show,
-- so the OFF side is the real vanilla game. Chapter numbers match kTour in overlay.cpp.
local function TourMark(label)
    Say("chapter %s", label)
    local pos = Try("GetCameraFocus", GetCameraFocus) or bench.cam or P(0, 0)
    Try("chapter " .. label, SpawnEffect, MARKERS .. "fortsgfx_tour_" .. label .. ".lua", pos)
end

local function ShowcaseBeats(sea)
    local f1, f2 = Forts()
    local wide = math.max(math.abs(f2.cx - f1.cx) * 1.6, 2000)
    local beats = {}
    local function at(t, fn) table.insert(beats, { t = t, fn = fn }) end
    local near = function(f) return { x = f.cx - f.dir * 700, y = f.top - 200 } end
    local function shot(kind)   -- one real projectile into fort 2
        if kind == "mortar" then Shoot("mortar", 1, { x = f2.cx - f2.dir * 1600, y = f2.ground - 50 }, Target(f2), 2.0)
        else Shoot(kind, 1, near(f2), Target(f2), kind == "missile" and 1.4 or 0.6) end
    end
    local function every(t0, t1, dt, kinds)   -- a steady stream of shots, cycling through kinds
        local i = 0
        for t = t0, t1, dt do
            local k = kinds[i % #kinds + 1]
            at(t, function() shot(k) end)
            i = i + 1
        end
    end
    local function fires(t, n)
        at(t, function()
            for i = 1, n do Try("IgniteFire", IgniteFire, P(Rand(f2.minX + 30, f2.maxX - 30), Rand(f2.top + 30, f2.ground - 40)), 60, 120, 2) end
        end)
    end
    local overview = function() Look((f1.cx + f2.cx) * 0.5, (f1.cy + f2.cy) * 0.5 - 150, wide * 0.6) end
    local onFort2 = function() Look(f2.cx, f2.cy - 120, 1500) end

    local t = 0
    at(t, overview); at(t + 0.5, function() TourMark("00_intro") end)
    every(t + 2, t + 9, 1.5, { "cannon", "mortar" })
    t = 11; at(t, onFort2); at(t + 0.2, function() TourMark("01_lighting") end)
    fires(t + 0.5, 4); every(t + 1, t + 14, 1.6, { "mortar", "cannon" })
    t = 26; at(t, function() Look(f1.cx, f1.cy - 100, 1200) end); at(t + 0.2, function() TourMark("02_materials") end)
    t = 37; at(t, onFort2); at(t + 0.2, function() TourMark("03_explosions") end)
    every(t + 0.5, t + 15, 0.55, { "cannon", "mortar", "missile", "cannon" })
    t = 53; at(t + 0.2, function() TourMark("04_bloom") end)
    every(t + 0.3, t + 10, 0.4, { "cannon", "missile", "mortar" })
    t = 64; at(t + 0.2, function() TourMark("05_smoke") end)
    at(t + 0.5, function()
        local n = Try("GetDeviceCount", GetDeviceCount, 2) or 0
        for i = 0, n - 1 do
            local id = Try("GetDeviceId", GetDeviceId, 2, i)
            if id and id >= 0 then Try("damage", ApplyDamageToDevice, id, 200) end
        end
    end)
    every(t + 1, t + 10, 1.0, { "missile" })
    t = 75; at(t + 0.2, function() TourMark("06_rain") end)
    every(t + 1, t + 10, 1.2, { "cannon", "mortar" })
    t = 86; at(t, overview); at(t + 0.2, function() TourMark("07_snow") end)
    every(t + 2, t + 10, 2.0, { "mortar" })
    t = 98; at(t, onFort2); at(t + 0.2, function() TourMark("08_fog") end)
    every(t + 1, t + 10, 1.5, { "cannon", "mortar" })
    t = 109; at(t + 0.2, function() TourMark("09_day_night") end)
    fires(t + 0.5, 4); every(t + 2, t + 11, 2.5, { "mortar" })
    t = 121
    if sea then
        at(t, overview); at(t + 0.2, function() TourMark("10_water") end)
        every(t + 0.5, t + 10, 0.8, { "cannon", "mortar", "missile" })
        t = 132
    end
    at(t, onFort2); at(t + 0.2, function() TourMark("11_looks") end)
    every(t + 0.5, t + 15, 1.1, { "cannon", "mortar", "missile" })
    t = t + 16
    at(t - 1.8, function() Shoot("missile2", 1, { x = f2.cx - f2.dir * 300, y = f2.top - 2200 }, { x = f2.cx, y = f2.cy }, 1.9) end)
    at(t + 0.2, function() TourMark("12_photo") end)
    t = t + 8; at(t, overview); at(t + 0.2, function() TourMark("13_end") end)
    t = t + 7; at(t, function() TourMark("99_end") end)
    at(t + 0.5, function() Try("EnableCameraControls", EnableCameraControls, true); Say("showcase done - press F6 to run it again") end)
    return beats
end

local function RunBench(sea)
    bench.run = bench.run + 1
    local run = bench.run
    Say("run %d starting (%s%s)", run, FORTSGFX_SHOWCASE and "showcase, " or "", sea and "sea" or "land")
    Try("EnableCameraControls", EnableCameraControls, false)
    for _, b in ipairs(FORTSGFX_SHOWCASE and ShowcaseBeats(sea) or Beats(sea)) do
        Try("ScheduleCall", ScheduleCall, b.t, function()
            if bench.run == run then
                local ok, err = pcall(b.fn)
                if not ok then Say("beat at %.1f s failed: %s", b.t, tostring(err)) end
            end
        end)
    end
end

function Load(gameStart)
    math.randomseed(1234)   -- the same shots every run, so runs can be compared
    Try("ScheduleCall", ScheduleCall, 2.0, function() RunBench(FORTSGFX_BENCH_SEA) end)
end

function OnRestart()
    bench.run = bench.run + 1   -- stops the old run's remaining beats
end

function OnKey(key, down)
    if down and (key == "f6" or key == "F6") then
        math.randomseed(1234)
        RunBench(FORTSGFX_BENCH_SEA)
        return true
    end
    return false
end
