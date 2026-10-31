# FortsGFX — development notes

How it works and how it got here, newest first. For installing and playing, see the [README](../README.md).

A proxy `opengl32.dll` for Forts. Windows loads it from the game folder instead of the
system one; it forwards every OpenGL call to the real `C:\Windows\System32\opengl32.dll`
and adds a graphics layer on top.

## Vulkan, phase V1 — the bridge (looks, bloom and the control panel on Vulkan)

FortsGFX's effects are GL, so instead of rewriting them for Vulkan, each Vulkan frame visits a GL context of ours
(`vulkan.cpp`, "the bridge"). The game's device is created with `VK_KHR_external_memory(_win32)` and
`VK_KHR_external_semaphore(_win32)` added (only if the driver has them; otherwise as the game asked). On the first
present a hidden window and GL context are made, checked to be on the same GPU (device UUIDs), and two images (IN,
OUT) are made in Vulkan with exportable dedicated memory and imported into GL (`GL_EXT_memory_object_win32`), plus two
shared semaphores (`GL_EXT_semaphore_win32`). Each present: Vulkan blits the swapchain image into IN, flipped so GL
sees it the right way up, and signals A; GL waits on A, runs `GfxBridgeFrame` (IN -> OUT) and the control panel
(`OverlayBridgeFrame`, input from the window of the Vulkan surface), signals B; Vulkan waits on B, blits OUT back
(flipped again) and signals C, which the present waits on. Three command-buffer slots with fences; nothing else waits
on the CPU. An sRGB swapchain gets sRGB shared images, read and written raw in GL (`SKIP_DECODE`). Device and
swapchain destruction release everything of ours on them. Any failure turns the bridge off for the session and says
why in vulkan.log and session.log.

`GfxBridgeFrame` (gfx.cpp) copies IN into the world target with the masks cleared to "playfield everywhere, nothing
solid or glowing" and runs `RunPost` with `g_bridge` set: no day/night, events, weather, world particles, snow,
lightning, lighting, fog or materials (all need the world from the Vulkan draws, phase V2), so the look, bloom from
the bright parts, grade, FXAA, sharpening, vignette and grain. F9 turns it off. Photo mode saves from OUT
(`CapBridgePhoto`). Known gap: the game's HUD is in the image too, so it gets the look; separating it needs the draw
stream (V0's dumps).

Tested with `tools/vktest.cpp` (an orange `TestMark` square shows which way up the round trip comes out; `vktest
srgb` uses an sRGB swapchain; `[Vulkan] ShotAfterEffects=1` / `DumpAt=1` in `build\FortsGFX.ini`).

## Vulkan, phase V0 — recon

Forts' Vulkan renderer (`EnableVulkan = true` in `users\<id>\settings_hardware.lua`) is native Vulkan with its own
SPIR-V shaders (`data\shaders\*.spv`), so the OpenGL hooks see nothing. Forts still imports opengl32.dll, and it loads
vulkan-1.dll itself and gets `vkGetInstanceProcAddr` through `GetProcAddress`. `src/vulkan.cpp` hooks that call in
Forts.exe's import table and wraps the Vulkan functions the game looks up, so one DLL covers both renderers.
V0 only watches: `vulkan.log` (instance, device, swapchain, render passes, shaders by name, pipelines with vertex
layout and blend, descriptor layouts, a 10 s summary of what a frame does), `VKIMG` lines in loading.log (each image
with the DevIL file just loaded), and `vk_frame_NN.txt` + `.png` (every command of one frame, with the first vertices
of each draw decoded from mapped memory, and the presented image copied out after the game's semaphores) on
Ctrl+Shift+F11, 10 s in and then once a minute, at most 8. The swapchain gets `TRANSFER_SRC` added when the surface
allows it, for that copy. `[Vulkan] Hook=0` turns it all off. Headers: `third_party/vulkan` (Vulkan-Headers 1.4.309).
`tools/vktest.cpp` tests it without the game (imports our opengl32.dll from `build\`, draws 13 s of frames).

## 1.1 — rebindable keys

Control panel → *Keys*: every hotkey (panel, Lab bar, photo mode, looks, on/off, lighting) is a button
showing its key; click it and press the new one (`kHotKeys` / `BindPoll` in `overlay.cpp`). Mouse buttons
and Shift / Ctrl / Alt / Windows are ignored, Esc cancels, a key another hotkey has swaps with it, and the
new key is written to the ini at once (`WritePrivateProfileString`, nothing else is saved). While waiting,
and until the new key is let go, no hotkey fires (`OverlayKeyBlocked`, also used by F9 / F10 in `gfx.cpp`)
and the keyboard is held back from the game, so the press doesn't also do something in Forts. Every hint
that names a key (tooltips, the Lab bar, the tour's captions) shows the bound key. The raw `...KeyVK`
number fields are gone from the other sections.

## Phases 4–5 — showcase effects, weather, 3D backdrop, control panel

**Control panel** — press **Insert** in game (Dear ImGui, `third_party/imgui`, MIT). Live fps/frame
graphs, *Showcase / Cinematic / Clean* style presets and *Low / High / Ultra* quality presets (▲/▼
arrows cycle any drop-down), on-screen debug views, a section per feature, Save/Reload ini. Every
setting comes from one table (`BuildSettingsTable` in `gfx.cpp`). While the panel has the mouse or
keyboard, both window messages and DirectInput are held back from the game.

**Impact effects** — world draws with explosion / flash / spark / fire textures are measured as
they are drawn (`GfxTrackVertex`) and tracked across frames, so each explosion fires once:
shockwave distortion, screen shake (HUD stays still), a flash light injected into the ray-traced
lighting, a volumetric 3D fireball, GPU particles (debris, sparks, embers; 16k, collide with the
real terrain/struts via the solid mask), colour pulse, heat haze over fire and — on big hits — an
anime-style impact frame (silhouettes, speed lines, alternating polarity).

**Weather** — the game's rain (huge tiled streak overlays, detected by size + streak coherence) is
hidden and replaced: particle drops that splash on terrain, a near rain layer lit by the scene,
wet surfaces. Storms: procedural branching lightning that finds the ground from the solid mask,
restrike flicker, sky flash through the sky-visibility term, light along the bolt, synthesized
thunder (`PlaySound`, delayed). Modes: Auto (follow the map), Off, Rain, Storm.

**Hybrid 3D backdrop** — backdrop layers are counted as they draw; the farthest ones are hidden and
`kFsBackdrop` (sky, sun, stars, two lit cloud decks, three parallax mountain ranges with toon rock
shading and aerial perspective) is rendered in their place, behind the kept near hills. Colours are
sampled from the hidden layers' textures.

**Weather, round 2** — modes Auto / Off / Rain / Storm / **Snow** / **Thick snow**. Rain: wet paint
(darker, richer), a sky-coloured wet sheen and a bright wet rim along every top, water trails running
down, puddle reflections on flat wet tops (the scene mirrored about the water line, with ripples;
`Puddles`), surfaces dry over about a minute after the rain. Snow: drifting flakes that settle where
they land, snow caps that build up on everything that faces up — terrain, struts, armour, devices —
(`SnowCover`, found from the solid mask: how far each solid pixel is below open air) and melt slowly,
snow on the 3D backdrop's mountains, near flakes and a white-out veil in thick snow, gusting wind.
Weather sets a mood: rain is overcast (dimmer sun and ambient, cooler), snow is bright and cold.
Looks *Snowfall* and *Blizzard*. `replay.exe <capture> <ini dir> weather` renders all three.
Solid surfaces are now lit from the edge that faces the light and less further in, so light follows
the shape of struts instead of brightening whole sprites.

**Test bench** (`testbench\`, installed by `install.ps1` as two Sandbox maps, *FortsGFX Test Bench* and
*FortsGFX Test Bench (sea)*, built from the game's own `-template-` / `-template water-` maps). The map script
(`bench_script.lua` after `land_header.lua` / `sea_header.lua`) runs by itself 2 s after the battle starts,
~90 s in all. It frames the camera, builds a test tower (bracing, back bracing, armour, a laser) beside
fort 1 and fires real projectiles at fort 2 (`dlc2_CreateProjectile`): a minigun burst, cannon,
a mortar barrage, swarm missiles, the big warhead twice (falling / landing). It also sets fires on
the enemy fort and inside the tower, calls lightning (Firebird), fires the laser, damages devices for
smoke, and ends with a barrage that tests the bloom. At each of the 15 beats it spawns an invisible
marker (`maps\fortsgfx testbench\effects\fortsgfx_capture_NN_name.lua`); FortsGFX recognises the image
name (`kEvBridgeCapture`), hides it and saves a capture named after the beat (`label.txt`, log line
`capture NNN is test bench beat ...`). Beats are 6 s apart, so each ~170 MB capture is written before
the next one. F6 runs the sequence again (build something first if you like). Every game call is
wrapped and logged to the console as `[bench] ...`. `python testbench\make_markers.py` regenerates
the markers after beats change. No mod is needed, and nothing touches multiplayer.

**Showcase tour** (Lab bar → *Tour*, or the Sandbox maps *FortsGFX Showcase* / *FortsGFX Showcase (sea)*):
14 chapters, one per thing FortsGFX adds. The intro wipes from the game as it ships to FortsGFX. After
that, every chapter with a switch flips its feature OFF for 2 s, then ON for 3 s, with a badge at the
top (OFF *without it* / ON *with FortsGFX* and a countdown to the flip). The chapters are lighting,
materials & sun, explosions, bloom, simulated smoke, rain & lightning, snow, fog banks, day & night,
water, looks, photo mode and keys. Your settings, time of day and snow are put back afterwards.
From the Lab bar the chapters advance by themselves and FortsGFX's own effects stand in for a battle.
Explosions don't flip there, and smoke and water are skipped when there's nothing to show. On the
showcase maps the map script moves the chapters along (`TourMark` → `fortsgfx_tour_NN_name` marker →
`kEvBridgeTour` → `GfxTourChapter`) and fires real shells, missiles and fires, so OFF is the real
vanilla game. `replay.exe <capture> <ini dir> tour` renders a few chapters OFF and ON
(`replay_tour_*.png`).

**Round 6 — reading the world from the drawing, no mod needed**
- *Where the playfield starts*: the backdrop is drawn entirely as 4-vertex strips, so the first other
  draw (a terrain display list, quads, triangles) starts the playfield, as does the first object
  matrix. Some maps draw the terrain, the sea or a fort's back wall (`bracing-bg`) before any object
  matrix. Those used to count as backdrop: no solid mask (snow and rain fell into the terrain) and the
  distance haze and weather veil (boat interiors went white in Snow / Blizzard / Sandstorm, the sea
  turned grey).
- *The sea from the drawing*: an immediate triangle mesh across the view that is flat on top and
  reaches deep is the sea. Its top edge, kept per screen column (64), is the water line the water
  effects follow, waves included. That works on packed DLC maps, where the map script is never seen.
  The menus read every map's script, so the script's `WaterLevel` is only logged now. Log line:
  `gfx: sea found in the drawing (tex N, V verts): water line Y, waves H`. The sea's whitecap band is
  no longer mistaken for the game's rain overlay and hidden.
- *Snow in forts*: snow that falls inside a fort's back wall (covered but not a front surface) is gone
  instead of filling the hull or room, and flakes that land in there vanish. Snow melts in the sea.
- *Fire sparks*: the flash textures serve both the sniper's muzzle flash (a sprite) and the sparks of
  fires and impacts (particles, `GL_QUADS`). A particle with one now glows softly (`SoftGlowLight`), so
  catching fire no longer blooms. No companion mod: everything works in multiplayer with the
  unmodded game.
- *Smoke trails puff by puff*: the game draws a whole trail's particles in one `GL_QUADS` batch, which
  used to become one event: a giant simulated puff mid-trail (the "something floating above" a
  warhead). Each smoke/steam particle is now its own event and yields, on average, one simulated puff
  over its life, where it is. A rocket's plume adds its own trail only where the game draws none.
  Buster's depleted-uranium trails (`fireflareuranium`) count as smoke, not fire, so they are replaced
  too instead of doubling up. `replay.exe … rocket` also writes `replay_rocket_trail.png`, with a game-style
  trail batch fed through the draw tracker.
- *Keys*: the DirectInput hook kept one "original" GetDeviceState / GetDeviceData for every device. If the
  keyboard's vtable differs from the mouse's, the keyboard ran the other device's code and no key reached
  the game (Escape, chat), from launch. Each patched vtable now keeps its own originals, and the log
  names every device and vtable. `[Overlay] HookInput=0` leaves the game's input completely alone.
- *Engine watch* (`src/engine.cpp`): FortsGFX now reads what the game itself does instead of guessing
  from sprites. The CPU's debug registers watch four of the game's functions: CreateEffect (every effect
  spawned: its file and world position), and the script callbacks for a projectile hitting, a weapon
  firing and a device dying. Read-only: no game code or data is changed, so multiplayer is unaffected.
  First use: blasts. An explosion-named sprite is a blast only where the game has just spawned an impact
  or explosion effect (each spawn makes one blast); a fire's own explosion-like frames never have one.
  This replaces the draw-order rule, which in big battles called nearly every blast a fire frame. It
  switches over once 3 sprites have lined up with the game's positions (logged), and the log lists
  every effect the game spawned. `[Engine] WatchEvents=0` turns it off. `tools/enginetest.cpp` tests
  the watch without the game.
- *The milk miko* (`[Fun] MilkMiko`, a joke; Lab Systems "Milk miko"): the DLC water cannon is drawn as an
  over-the-top chibi shrine maiden (art drawn in code: `fun/make_art.py`) holding a giant milk-bottle cannon -
  the game's textures are swapped per draw (letterboxed to their shape) and rebound after - and her water shots
  are hidden and become simulated milk: particles (type 16) that fall and run along surfaces, splatted into a
  half-res density target and drawn as one glossy liquid (`kFsMilk`). Sakura petals (type 17) and manga words
  (ドーン！ ピュー！ バシャ！ やったぁ！ いたっ！), speed lines and sparkles (screen-space sprites after the final
  pass), faces that change (happy when her milk lands, >_< when she's hit: `weapon_hurt` effects), voice lines by
  ElevenLabs (`fun/make_voice.py`, voice "Kano"; `[Fun] Voice`), Mount Fuji in the 3D sky while she's on the map
  (`[Fun] MountFuji`). Assets install to `<game>\FortsGFX_fun`. The parts are matched by name (`water_cannon`
  under weapons/); the log lists each part it found ("fun: water cannon part ..."). `replay.exe ... miko` previews it.
- *Forts no longer darken when you zoom out*: the "ambient occlusion" is sky visibility traced over a grid
  of screen cells, so zoomed out each cell covers more of the fort and it came out more occluded (measured
  with `replay.exe <capture> <ini dir> zoom [settings] [glow]`, which shrinks a capture 2x and 3x: occlusion
  grows ~ view size^0.45, while a reactor-like light added the same light at every zoom). The occlusion is
  now scaled back by that law (`AoZoomScale`, world units per lighting cell, reference a close 600-unit view):
  structures stay within a few percent across zooms on all three test captures.
- *Looks re-balanced* (ini v12 re-applies yours): structures came out much darker than the original game
  (Vanilla+ 0.81, Realistic Preview 0.63, Cinematic 0.59, Film Noir 0.11 of the game's brightness).
  `replay.exe ... calibrate` searched each look's ambient level for a target - neutral looks ~1.0, moody
  ones ~0.8, dark ones ~0.55-0.65 - on two captures, and the averages are in the looks (`replay.exe ... looks`
  prints the brightness of every look). The ambient slider now goes to 2.
- *Effects from the game's events* (`[GameFx]`, Lab row "Game events", each part switchable): driven by
  what the engine watch reports, never by sprite guesses. Every impact effect the game spawns has a name
  (`effects/armor_bullet_hit.lua`, `impact_heavy_ground.lua`...) and every projectile hit says what it hit
  (`"bracing"`, `"armour"`) and the surface normal, so:
  - *Impacts by material*: sparks and molten slag (new particle) off armour, splinters (new) and dust off
    bracing, a ripple and crackling arcs (new) on shields, a splash column on water, earth off the ground,
    sand off sandbags. The effect's own name wins; otherwise the projectile's hit at the same spot (once).
  - *Scorch marks and craters* where things really hit (only on surfaces, under glowing sprites, lit like the
    structure, fading over `ScorchSeconds`), still-hot cracks for a moment; molten marks where lasers cut.
  - *Muzzles* by weapon: cannon pressure ring and a smoke cloud that hangs, mortar puff, sniper shimmer
    along the shot, machine-gun barrels that heat up and smoke when fired for long, launch exhaust and dust.
  - *Lasers*: a charge-up from the game's own timing - `beamlaser.lua` plays `laser_reload.lua` 3.4 s before
    it is ready: the emitter glows up (light and a flickering core, crackling arcs), idles ready, and discharges
    on `fire_laser.lua` (the beam follows after `FireDelay` 0.1 s). Then light along the beam, a molten glow and
    dripping slag where it cuts. Missile silos warm up the same way (`warhead_warmup.lua`, launched
    `FireDelay` 5 s later on `warhead_launch.lua`): a glow, heat and venting steam until launch.
  - *Heavy weapons* (warhead, nuke, howitzer, reactor): a delayed shockwave ring, the camera stopping down
    after the flash (final pass `uPulse.w`), a dust wave along the ground.
  - *After a hit*, a more physical alternative to the screen-warping shockwave (Lab rows *Explosion* →
    *blast wind*, *After a hit* → four switches):
    - *Blast wind* (`BlastWind`): a front sweeping out from each blast (from 0.3 to 1 of its radius, ~260 world
      units for a bullet .. 520 a missile .. 980 a nuke, weakening as it spreads) shoves the smoke, steam, dust,
      rain, snow, embers and petals around it. Settled snow is blown off. Then the air rushing back draws things in
      a little for 0.7 s. The blast's own smoke and gas at its heart are left alone. Particle update `uBlast[8]` /
      `uBlastW[8]`, from the game's impacts (by size) and blast sprites, de-duplicated.
    - *Dust shaken off* (`ShakeOff`): dust and grit come loose from the structure above and around a hit, most at
      once and then a trickle for a second, more and wider for bigger hits. Spawned as candidates (particle type 19)
      that the update keeps only where the solid mask's front surfaces really are: they become grit (type 18,
      in the colour of the surface it fell from) or a dust puff under an underside, and are dropped elsewhere.
    - *Debris coloured like the hit* (`HitColour`): chunks and splinters sample the game's frame next to the hit
      on their first update (front surfaces only, skipping glowing sprites and the black ink outlines) and keep
      the colour packed in their velocity's z.
    - *Dust haze* (`DustHaze`): hits from size 0.5 leave a few large thin dust puffs (negative size = haze, 0.4
      opacity) that hang for 5-8 s and settle.
    - *Ground blast* (`GroundBlast`): ground hits blow a ring of dark smoke and dust out along the ground, and
      the spreading blast kicks dust off the tops of the surfaces around any hit from size 0.3.
    - Offline check: `replay.exe <cap> <ini> blast on|off [ax ay bx by cx cy]` (shockwave off, smoke hanging
      near three hits; `replay_blast_0..4.png` and particle counts by type).
  - *Scaled shake*: from the first impact, shake comes from the impact's real size (the game's effect) instead of
    the sprite's; once the positions are confirmed, also from its distance from the middle of the view.
  - *Device deaths* (OnDeviceDestroyed + the nearest explosion effect for its position): reactor flash and
    glow, battery arcs and flicker, mine dust, a smoking wreck. *Collapse dust* where struts break.
  - *Weather*: rain hissing to steam on fires, snow melting around fires and fresh blasts (snow step
    `uHeatW`), storm wind bending smoke and embers. *Light from shots and hits* lights the structure around.
  - *In flight*, from the game's own effect list: the effect manager (its pointer is the global most
    CreateEffect callers load) keeps live effects in a `std::list` at +0xB8; an effect's position is at +0x134,
    its id at +0x14C. Each new effect is named by the CreateEffect call at its spot (tracking starts once 5 line
    up exactly, and stops if 300 don't). A projectile carries its `TrailEffect`, so trails are the projectiles
    in flight with their kind and speed: shells shimmer behind them, rounds above 4500 units/s (cannon 6000,
    sniper 10000 in the game's weapons) leave a faint ring once. Until it is tracked: from the tracer sprites.
    Missile and rocket bodies (the game's own sprites) cast shadows - occluders that rain and snow don't land on.
  `replay.exe <capture> <ini dir> gamefx [off]` feeds a scripted set of game events into a capture and writes
  `replay_gamefx_a/b[_off].png` (mid-action and aftermath) to compare with and without.
- *Snow only on tops*: snow can't lie anywhere with a structure (strut, roof, deck, back wall) within
  ~600 world units above it, and flakes that land under one vanish. Open frames and fort interiors stay
  clear in Snow / Blizzard / Sandstorm.
- *Less red*: Showcase is calmer (saturation 1.16, effect saturation 1.25, light 1.4, bloom 1.7), and the
  day/night golden hour is shorter and softer: `[DayNight] DuskGlow` (default 0.45). Lab bar → More →
  *Water* row: reflections, ripples, foam, dusk glow.
- *Explosions the same on every look*: the effects preset now also owns how effects glow and light the
  world (`[Impact] EffectGlow`, `EffectLight`; Showcase 2.0 / 1.6, Cinematic 1 / 1, Subtle 0.7 / 0.8).
  Effect sprites' bloom and the explosion / flash / plume / tracer lights are compensated for the look's
  `BloomEmissive` and `Lighting.Intensity`, so Vanilla+ or Clean with FX: Showcase has Showcase
  explosions. Effect *saturation* stays with the look (Film Noir keeps black-and-white fire). ini v9.
- *Fog inside forts*: fog pooling ("in valleys": where little sky is visible) only happens in the open
  air, not on anything drawn in the playfield. Inside a fort's frame the sky is hidden too, and the
  planks took on the backdrop's colour. A drawn back-wall panel counts as fully solid even where the
  game draws it partly transparent.
- *Bloom tuning* (`[Bloom]`, kept when you change looks; Lab bar → More → *Bloom* row):
  `Strength` (how much bloom is laid over the image), `Spread` (each wider pyramid level adds
  spread^k, normalised so a lone light keeps its brightness: lower = tight glows, no haze over the
  base; default 0.85), `Limit` (soft cap: unchanged up to half of it, then eased towards it; 0 = off),
  `AutoDim` (the coarsest bloom level averaged into a 1×1 target, rising in ~0.15 s and falling in
  ~0.8 s, turns bloom down while much of the screen glows). Effect glow sits in the same row.
- *Fire catching is not an explosion*: as a strut ignites, the game draws `ExplosionSmall` over its
  `FireStart` frames. Each spread fired a full explosion burst (volumetric fireball, flash light,
  shockwave): the opaque white ball on burning forts, worst at night. FireStart sprites now mark
  ignition points for 0.6 s, and an explosion on one that is no bigger than 2.5x it becomes part of the
  fire. Volumetric fireballs also need a blast of at least 2% of the view height.
- *Effects pass, newer replacements* (`[Impact] SimulateMoreEffects`, Lab Systems row *More sim fx*,
  default on): smoke clouds (`cloud`, `CloudLarge`) → simulated smoke; `sandcloud` / dust → dust puffs
  (tan, heavy, settling); the High Seas deck gun's blast cloud (`fire_deckgun*`) → a burst of hot gas
  that glows orange and cools into a dark cloud; steam the game tints hot (the deck gun's yellow gas) →
  hot gas instead of white steam. Only effect images (`effects/` paths) are replaced. Packed DLC
  images are named after the file the game looked for on disk just before loading them (log line
  `packed image named after the game's lookup`). Puffs now count as near things (their coverage marks
  the playfield, so they are not hazed like the backdrop). Their surface lighting is capped, and
  lit-smoke scattering goes with their colour, so dark smoke stays dark in sunlight.
  `replay.exe … puffs` shows the four kinds.
- *Fire sprites by draw order*: the game shares one texture between identical images, so one of a
  fire's own animation frames shows up under the name `ExplosionSmall`. Read as an explosion, each one
  fired a full burst (shake, fireball, flash light, bloom): 11 in one frame in capture_010. The fire
  system draws a burning structure's sprites one after another, with the forts. Real explosions come
  later, with the terrain and glows. So an explosion-named sprite with a fire sprite within two draws
  either side is a fire frame. In all 14 captures every `ExplosionSmall` sat among fire sprites, and
  every real explosion (`ExplosionLarge`, `RocketExplosion`) was drawn apart. Blasts are as they were;
  the earlier distance / size guesses are gone.
- *Names count only where effects live*: once the DLC images had names, their fort materials and
  weapon sprites matched the effect words. Blast doors (`blastdoor*`, "blast") went off as
  explosions, and again each time zooming brought another door into view. Explosion, smoke, steam
  and spark classes now need an `effects/` path; fire needs `effects/` or `materials/` (burning
  fuses). Muzzle flashes need `effects/` or `weapons/`, minus flashbang projectiles, and bullet-hole
  decals are no tracers. Blast doors don't glow. Checked against every image name of a session:
  only those were dropped, and all 217 effect images were kept.
- *The log*: `session.log` is now what happened. It has the build time, `--- battle view` start / end
  lines with the look, effects preset, weather and switches, and every 20 s an effects summary: each
  game sprite drawn (times drawn) and what FortsGFX did with it (blast, the fire's own frame, flames,
  hidden → simulated puffs / dust / hot gas, muzzle light, tracer, particle soft glow...), plus how
  many bursts and puffs FortsGFX made. There's a perf line once a minute with the worst frame, and
  settings, looks and problems as before. Every texture and script the game loads goes to
  `loading.log`.
- *Burning structures, tamed*: the game draws a fire's puffs (`fireball.tga`) and sparks as quads
  batches, one draw for a whole patch of flames, so a fire was tracked as one huge flame. Its heat-haze
  source and light covered the burning area ("a massive heat wave"), and effect glow had doubled the
  bloom of every fire sprite and spark. Fire batches are now split per flame. Heat over fires is
  `[Impact] FireHeat` (default 0.45), with each source at most 2.5% of the view height. Fires and their
  sparks glow by `[Bloom] FireGlow` (default 0.7); effect glow applies only to blasts and flashes. Lab
  bar → More → *Bloom* row: fire glow, fire heat.
- *Seeing the ray tracing*: Lab bar → More → *Lighting* row: ambient (`Materials.AmbientLevel`: lower
  it and the traced light and shadows carry the scene), light (`Lighting.Intensity`), shadows (ambient
  occlusion), sun. New look **Dramatic Light** (last in the list): ambient 0.42, light 2.2, shadows
  0.85, sun 0.3. Fires and blasts visibly light and shadow the forts.
- *Showcase is neutral now* (ini v11): strong saturation and vibrance amplified each map's dominant
  colour, so warm maps went red (canyon red/green 1.75 against vanilla's 1.53). Showcase is now
  saturation 1.05, vibrance 0.2, contrast 1.0, bloom 1.3, vignette 0.12, temperature -0.06, effect
  colour 1.1, on top of the brightness fix below. It measures red/green 1.53, with contrast a hair over
  vanilla. Explosions stay at FX: Showcase strength (a separate preset).
- *Showcase as bright as the game*: on a daytime map Showcase left the forts ~25% darker than vanilla
  (contrast 1.12, ambient occlusion 0.6, ambient 0.68). It is now contrast 1.08, AO 0.45, ambient 1.0,
  gamma 1.1: forts match vanilla and sky / ground are a little brighter. ini v10 re-applies the
  built-in look you use so the new values arrive. The tour starts every chapter from Showcase at midday
  with no weather (your night look made it all dark), the lighting chapter uses a low evening sun, and
  the day/night time-lapse ends at 21:30.
- `replay.exe <capture> <ini dir> sea [water line]` renders a High Seas capture with its sea as playfield
  in rain, snow and blizzard (`replay_sea_*.png`).

**Round 5 — lit smoke, day/night, water, photo mode, more weather, tracers**
- *Lit smoke*: simulated puffs write their density into the solid target's spare `g` channel; the
  composite scatters the ray-traced light through it (smoke glows near fires, darkens away from light).
- *Day / night* (`[DayNight]`, default on, 20 min per day, starts at 13:00): a real-time clock moves the
  sun along an arc — direction, colour and strength for the traced sun light and the 3D backdrop's sun /
  moon, golden dawn and dusk, a dark blue moonlit night with stars, light sources brighter at night
  (`NightLights`), ambient / exposure / white balance and a backdrop tint follow. Applied on top of the
  user's settings at uniform time. Lab bar: ◀ Day 13:06 ▶ (click to pause), photo bar time slider.
- *Water* (`[Water]`, High Seas maps): the water line comes from the game's own sea mesh (round 6);
  below it: rippled reflections of everything above (fires and
  explosions glow in them), depth tint, foam at the waterline, glints, explosion light on the surface;
  rain splashes on it, sparks / embers / flakes go out in it, debris sinks slowly, explosions at the
  surface throw spray and mist.
- *Photo mode* (**End**, or *Photo* on the Lab bar): freezes every FortsGFX effect, hides the HUD and our
  UI behind a toolbar — depth of field (click to focus: the other layer blurs; tilt-shift), framing bars
  16:9 / 21:9 / 2.39:1 / 4:5 / 1:1 (in the photo), thirds grid (not in the photo), look and time of day,
  **Save photo** → `FortsGFX\Photos\FortsGFX_<date>_<time>.png` at full resolution without any UI.
- *More weather*: **Sandstorm** (sand driven sideways that piles into drifts in the snow layer, dust
  sheets, orange haze), **Fog banks** (the fog system faded in thick and low, lights glow through),
  **Ash & embers** (falling ash settling thin, embers on the wind, red sky). Looks *Sandstorm*,
  *Fog Banks*, *Ashfall*.
- *Tracers* (`TracerGlow`): bullets, cannon and mortar shells are matched frame to frame for their
  velocity and get a glowing streak behind them plus a small travelling light.
- `replay.exe <capture> <ini dir> r5` renders all of it (`replay_r5_*.png`).

**Every effect, round 3** — from an inventory of all textures the game loads:
- *Rocket and missile plumes* (`missiletail01-08`, boosters, thrusters): every frame a light that
  travels with the rocket, heat shimmer behind it and a simulated smoke trail (`RocketPlumeLight`).
- *Steam and smoke* (`Steam` — also the missile smoke trail —, `steamcloud`, `smoke`: mortar trails,
  overheating guns, damaged devices): the game's sprites are hidden and replaced by simulated puffs
  that grow, rise, billow on the wind and are lit by the scene (`SimulatedSmoke`, `SmokeAmount`).
- *Lasers and beams* (`laserfx_*`, `beam.*`, effect beams — never the weapon sprites or aim lines):
  a white-hot core, energy flowing along the beam and a richer glow (`LaserGlow`).
- *Front surfaces*: the fort's back wall (`*-bg`, `*_background`) is no longer where rain lands or
  snow settles.

**Snow physics** — snow is a simulated layer (¼ resolution, `kFsSnowStep`): settled flakes deposit
into it, it falls into free space, slides off edges and slopes, piles up on front surfaces only where
flakes can actually reach (roofs shelter what is under them), collapses when what holds it is
destroyed, is limited to `SnowDepth`, builds at `SnowBuildUp`, slides with `SnowSliding` and melts
after the snow stops. Flakes also land on snow already lying there, so piles grow. The layer is
reprojected as the camera moves (snow that leaves the screen is not kept). Lab: *Clear snow*,
snow depth and build-up sliders. `replay.exe <capture> <ini dir> rocket` tests plumes and steam.

**Effects preset** — explosion strength (shockwave, shake, colour pulse, flash light, fireballs,
debris, sparks, embers, impact frames, heat haze) is its own preset: Showcase / Cinematic / Subtle
(`[Presets] Effects`, panel and Lab "FX:" button). Looks never change it.

**Looks** — 20 complete styles (`BuildLooks` in `gfx.cpp`): Showcase, Cinematic, Clean, Vivid Cartoon,
Comic Book, Golden Hour, Moonlit Night, Thunderstorm, Gritty War, Film Noir, Old Film, Neon Synthwave,
Pastel Dream, Arcade Punch, Misty Morning, Hellfire, Realistic Preview, Vanilla+. A look resets every
look-related setting to its default, optionally applies a style preset, then its overrides
(`Section.Key=value;…`), so switching is always clean. **F7 / F8** step through them (a toast shows
the name; the look you land on is saved to the ini in the background), the panel and the Lab have a
▲/▼ picker, the Lab can auto-cycle them and save the current settings as your own look
(`[CustomLooks] LookN=Name|overrides`). New grade controls used by the looks: split toning
(`SplitTone`, `ShadowTint`, `HighlightTint`), comic `InkOutlines`, `Posterize` (forts/terrain only).
`replay.exe <capture> <ini dir> looks` renders every look on a capture (`replay_look_NN.png`).

**Lab** — a HUD bar along the top of the screen, opened on its own with **Home** (`LabKeyVK`) or from
the panel. Row 1: click tools + effect size. Row 2: look ◀ name ▶ (+ Auto), Before/After (+ Sweep),
debug view ◀ name ▶, effect speed 1x / 0.5x / 0.2x, weather (click to cycle), Strike, Hide HUD, Tour,
More, Panel, X. *More* adds light size/power/colour, sun angle, split position, look saving and an
on/off button per system. `replay.exe <capture> <ini dir> ui` draws the panel and bar over a capture
(`replay_ui.png`) to check the layout. What the bar does:
- *Click tools* — Explosion, Mega blast (impact frame), Lightning, Shockwave, Flash, Heat, Sparks,
  Light (hold and drag: a live ray-traced light with shadows), Place light (up to 7). Left-click in the
  battle; right-click to stop. While a tool is armed the game ignores the mouse (the wheel still zooms).
  Spawns are queued and run inside the next post chain.
- *Before / after* — a split-screen wipe (left: the game's own colours captured before any of our
  effects are drawn; right: FortsGFX), optional sweep.
- *Show the tech* — every debug view as a button; on/off for lighting, materials, fog, impacts,
  backdrop, rain, impact frames, FXAA.
- *Time & weather* — slow motion for FortsGFX's effects, weather mode, strike lightning, sun angle.
- *Presentation* — hide the game's HUD and cursor, and a ~75 s captioned **showcase tour**
  (before/after sweep, orbiting lights at night, distance field / traced light / sky visibility,
  normals and a moving sun, slow-motion explosions, an impact frame, a storm, a carousel of looks).
  Every step starts from your own settings and everything is restored at the end; Insert stops it.

**Tuning after the first in-game tests** — effects only trigger for sprites that are visible
(opacity ≥ 0.06, tracked through `glColor`) and at least 0.6 % of the screen height across; the game
draws many invisible or pixel-sized flash sprites at bullet impacts. `fireball` sprites (puffed by
burning structures) count as fire, not explosions. Halo sprites (`[Emitters] SoftGlowNames`, default
`glow`) light the world at `SoftGlowLight` (0.25). Heat haze comes only from fire, explosions,
fireballs and guns that are firing (heat builds with sustained fire, cools over ~5 s: `HeatHaze`,
`GunHeat`); muzzle flashes give a small light (`MuzzleFlashLight`). Fog is **off by default**
(`[Fog] Enabled`). HUD textures (`ui/`) and weapon reload animations (`reload` in `NameExcludes`)
never count as light sources. Lighting gain on bright paint is rolled off by luminance so light
colours (bone, highlights) keep their hue instead of washing out. The panel logs to `session.log`
when it opens, every ▲/▼ click with the new value, and the first click it keeps from the game.
`ConfigVersion=5`; v3/v4 files are upgraded in place (only values still at the old default change).

**Mod bridge** — a sprite whose texture file name contains `fortsgfx_lightning`, `fortsgfx_impact`,
`fortsgfx_shock` or `fortsgfx_flash` is never shown; instead it triggers that effect at its position
and size. Lua mods can spawn such a sprite (e.g. as an effect) to fire lightning, impact frames,
shockwaves or flashes exactly where they want.

`tools/replay.cpp` also runs an effects sequence (explosion + lightning in a storm, simulated clock)
and a backdrop preview on a capture: `replay_fx_*.png`, `replay_backdrop.png`.

## Phase 3 — materials, directional light, sun

Every world draw now goes through our own shader (`kVsWorld`/`kFsWorld`) instead of fixed-function.
It reproduces the game's colour exactly (`tools/mattest.cpp` diffs it against fixed-function:
0 differences) and additionally writes emission, playfield/solid coverage and a **screen-space
normal** computed on the fly from the bound texture (alpha bevel + painted-luminance relief, rotated
into screen space through the UV Jacobian — works for display-list terrain too). Display lists, an
unusual texture-env mode or `[Materials] Enabled=0` fall back to the Phase 2 path.

The cascades also output the light's dominant direction (L1); a **sun** enters through rays that
escape to the sky (soft shadows from forts and terrain). The composite shades with normals: lit /
shadowed sides, metal highlights, sky-above / ground-below tint, silhouette bevel on terrain edges,
soft toon banding. Tunables: `[Materials]` and `[Sun]` in `FortsGFX.ini`; captures add `gi_normals`.

Fog is view-independent: backdrop haze, drifting fbm banks and pooling where sky visibility is low,
lit by the ray-traced light (sun shafts), tinted by a playfield-excluded backdrop blur that also
softens the painted background for depth. `session.log` gets a `perf:` line every 5 s.

`tools/replay.cpp` runs the real post chain offline on a capture folder
(`replay.exe E:\FortsGFX\capture_005`) and writes `replay_off.png` / `replay_on.png` plus per-stage
GPU timings — use it to judge any tuning on a real frame before shipping.

## Phase 2 — ray-traced 2D lighting + volumetric fog

During the world pass every draw is also routed into two masks (in spare channels of our
targets, never touching the game's own colour): **playfield coverage** (everything after the
parallax background — the background ends at the first `glLoadMatrixf` object) and **solid
coverage** (terrain display lists + `materials/`, `devices/`, `weapons/` textures).

At a quarter of screen resolution (`[Lighting] Resolution`):
1. emission + solid mask → **jump-flood distance field**
2. **radiance cascades** (6 cascades, 4→16k directions, bilinear-fix merging): every pixel gathers
   light from fire/explosions/beams with true occlusion, plus sky visibility (soft AO)
3. **pull/push inpainting** carries the light of the surrounding air onto solid faces, so struts,
   armour and terrain are lit by the fire next to them
4. composite: playfield × (AO + light), background gets a little light, then **height fog** in world
   space (denser toward the ground, drifting fbm noise), lit by the same ray-traced light so
   explosions glow through it and struts cast light shafts.

| key | action |
|---|---|
| **F10** | lighting + fog on / off |

Captures now also write `gi_masks` (red = solid, green = playfield, blue = emissive), `gi_light`,
`gi_sky` and `gi_distance`. `tools/shadertest.cpp` compiles every shader against the local driver;
`tools/gitest.cpp` runs the lighting passes on a synthetic scene and writes PNGs.

## Phase 1 — cinematic post-processing

The world pass is redirected into HDR render targets (scene + emissive). Fire, explosions,
flashes, beams and anything drawn additively write into the emissive target; everything else
occludes it. At the first HUD draw the world is post-processed into the back buffer and the HUD
draws on top untouched: emitter-driven bloom, highlight roll-off, colour grade, FXAA,
contrast-adaptive sharpening, vignette, film grain.

| key | action |
|---|---|
| **F9** | post-processing on / off (A/B compare) |
| **Ctrl+F9** | reload `Forts\FortsGFX.ini` (all tunables are written there on first run) |
| **Ctrl+Shift+F11** | capture a frame (now also saves `post_in_scene`, `post_in_emissive`, `post_bloom`) |

`[Emitters] NameContains` / `NameExcludes` decide which textures glow (matched against the
texture's file name); `AdditiveBlendEmits=1` also makes every additively blended sprite glow.

## Phase 0 — recon logger

On request it records **one frame** of rendering so we can design the graphics overhaul
around how Forts actually draws.

## Build & install

```
build.cmd                      # -> build\opengl32.dll  (needs VS 2022 "Desktop development with C++")
powershell -File install.ps1   # copies it next to Forts.exe
powershell -File install.ps1 -OutputDir E:\FortsGFX   # same, but logs/captures go to E:
powershell -File install.ps1 -Uninstall
```

Captures are refused (error beep, reason in `session.log`) when the output drive has under 3 GB free.

Uninstalling = deleting `opengl32.dll` from the Forts folder. Keep Vulkan off in Forts' settings
(`EnableVulkan = false`) — the hook only sees the OpenGL renderer.

## Capturing a frame

**Flight recorder** — always running: the last ~2.5 s of frames (about 960 px wide, every 50 ms,
read back asynchronously so the GPU never waits) and a per-frame timeline of the effect sprites the
game drew (class, texture, box, opacity, quad/strip, and whether it was ignored) and the effects
FortsGFX spawned. Every capture adds them as `history\frame_NN_-1.23s.png` and
`history\timeline.txt`, so the moment *before* the key press is there too.

In game (ideally mid-battle with explosions, beams, smoke and the HUD visible) press
**Ctrl+Shift+F11**. You hear one beep when it starts and another when files are written.
Alternatively create an empty file `Forts\FortsGFX\capture.trigger`.

Everything lands in `Forts\FortsGFX\` (or the `OutputDir` from `Forts\FortsGFX.ini`):

| file | contents |
|---|---|
| `session.log` | always on: context creation, hooks installed, every texture upload (size, format, content hash, source file / pack hash), shader sources |
| `procs.txt` | every GL extension function the game asked for |
| `capture_NNN\frame.txt` | every GL call of the frame; each `glBegin..glEnd` summarised as a `DRAW` line with screen bbox, UVs, colour, texture, blend mode |
| `capture_NNN\snap_*.png` | the render target just before each clear / framebuffer switch / projection reload (half res) |
| `capture_NNN\final.png` | the finished frame |
| `capture_NNN\textures.txt`, `tex\*.png` | every texture drawn with that frame |

## Layout

- `src/proxy.cpp` — DllMain, real DLL loading, pass-through table, IAT patching, logging utils
- `src/hooks.cpp` — hand-written wrappers (any `hk_<export>` here replaces the generic stub)
- `src/capture.cpp` — frame capture, framebuffer/texture readback, PNG writing (WIC)
- `src/gfx.cpp`, `src/shaders.h` — Phase 1 renderer: world redirection, emitter routing, post chain, ini settings
- `src/thunk.asm` — the generic pass-through thunk (one compare + jump when not capturing)
- `tools/gen_exports.ps1` — regenerates `src/gen/*` (exports .def, asm stubs) from `src/exports.txt`
