-- Procedural terrain demo: 2km × 2km @ 1 m vertex spacing, gentle rolling
-- hills. Wind-swayed procedural grass on the surface, a dungeon block
-- character (~2 m tall) walks the terrain with third-person orbit camera.
--
-- Triggered by `--terrain-demo` (or `--startup-mode=terrain_demo`).
--
-- Controls:
--   WASD / arrows      walk character on the terrain surface
--   Shift              sprint (4×)
--   right-mouse drag   rotate camera around character (yaw + pitch)

-- Static scene comes from the map snapshot assets/maps/terrain.map.
-- First run on a fresh checkout: the map doesn't exist yet → we run the
-- explicit spawn block (everything between `else` and the `corona.save_map`
-- call below) once and persist the result. Subsequent runs: load_map skips
-- the spawn block entirely. Delete the .map file to force a regen.
local mapName = "terrain"
local player = { x = 0.0, y = 0.0, z = 0.0, yaw = 0.0, eyeHeight = 1.6 }
local playerExtent = 2.0
local terrainEntity, grassEntity, playerEntity, cameraEntity, sunEntity

if corona.map_exists(mapName) then
    local ok, err = corona.load_map(mapName)
    if ok then
        terrainEntity = corona.Entity.by_name("TerrainDemoTerrain")
        grassEntity   = corona.Entity.by_name("TerrainDemoGrass")
        playerEntity  = corona.Entity.by_name("TerrainDemoPlayer")
        cameraEntity  = corona.Entity.by_name("TerrainDemoCamera")
        sunEntity     = corona.Entity.by_name("TerrainDemoSun")
        corona.log("[terrain_demo] loaded map " .. mapName)
    else
        corona.log("[terrain_demo] load_map failed: " .. tostring(err) .. " — falling back to spawn block")
    end
end

if not terrainEntity then
    -- 1. Terrain ----------------------------------------------------------
    terrainEntity = corona.Entity.create("TerrainDemoTerrain")
    corona.MeshComponent.add(terrainEntity, {
        primitive   = "TERRAIN",
        seed        = 1,
        position    = { 0.0, 0.0, 0.0 },
        rotation    = { 0.0, 0.0, 0.0 },
        scale       = { 1.0, 1.0, 1.0 },
        roughness   = 0.95,
        metallic    = 0.0,
        override_material = true,
        ray_tracing = false,
    })

    -- 2. Grass on terrain (12M caps to 9.5M at the C++ uint32 VB limit) ---
    grassEntity = corona.Entity.create("TerrainDemoGrass")
    corona.MeshComponent.add(grassEntity, {
        primitive    = "GRASS_ON_TERRAIN",
        blade_count  = 12000000,
        blade_height = 1.6,
        seed         = 20260527,
        position     = { 0.0, 0.0, 0.0 },
        rotation     = { 0.0, 0.0, 0.0 },
        scale        = { 1.0, 1.0, 1.0 },
        roughness    = 0.85,
        metallic     = 0.0,
        override_material = true,
        ray_tracing  = false,
    })

    -- 3. Dungeon block character (~2 m tall) ------------------------------
    playerEntity = corona.Entity.create("TerrainDemoPlayer")
    corona.MeshComponent.add(playerEntity, {
        primitive   = "block_character",
        seed        = 11337,
        position    = { player.x, player.y, player.z },
        rotation    = { 0.0, player.yaw, 0.0 },
        target_extent = playerExtent,
        roughness   = 0.7,
        metallic    = 0.0,
        override_material = true,
        ray_tracing = true,
    })

    -- 4. Camera (third-person follow, configured per-frame below) ---------
    cameraEntity = corona.Entity.create("TerrainDemoCamera")
    corona.CameraComponent.add(cameraEntity, {
        position   = { 0.0, 4.0, -6.0 },
        look_at    = { 0.0, 1.0, 0.0 },
        up         = { 0.0, 1.0, 0.0 },
        fov        = 0.9,
        near_plane = 0.1,
        far_plane  = 4000.0,
        active     = true,
    })

    -- 5. Sun --------------------------------------------------------------
    sunEntity = corona.Entity.create("TerrainDemoSun")
    corona.LightComponent.add(sunEntity, {
        type       = "directional",
        enabled    = true,
        direction  = { -0.35, -1.0, 0.45 },
        color      = { 1.0, 0.97, 0.88 },
        intensity  = 2.6,
        persistent = false,
    })

    -- First-spawn-only environment setup. After the map is saved these
    -- values live in the map file; on later launches the load_map path
    -- restores them, and the inspector lets the user edit + persist via
    -- "Save Map". Per-frame `on_update` no longer rewrites them.
    corona.set_grass_bend_params(3.0, 1.6)
    corona.set_grass_render_distance(grassRenderDistance)
    corona.set_wind_params(windDirX, windDirZ, windStrength, windTempFreq, windSpaceFreq)

    corona.log("[terrain_demo] spawned 2km terrain + 12M grass + ~2m player + orbit camera + sun")

    -- Persist this fresh spawn so the next launch can skip the slow blade
    -- generation step entirely. Cheap (~few KB text file).
    local ok, err = corona.save_map(mapName)
    if ok then
        corona.log("[terrain_demo] saved initial scene to " .. mapName)
    else
        corona.log("[terrain_demo] save_map failed: " .. tostring(err))
    end
end

-- Third-person follow camera state — survives load_map (script-side only).
local camYaw           = 0.0
local camPitch         = 0.18
local camDistance      = 6.0
local camHeightOffset  = 2.5
local mouseSensitivity = 0.005

-- Wind sway constants — scaled for the current 1.6 m blades. The ratio
-- (sway strength / blade height) ≈ 0.453 keeps visual sway proportional,
-- so 1.6 m blades use ~0.72.
local windDirX      = 0.11
local windDirZ      = 0.14
local windStrength  = 0.72
local windTempFreq  = 12.0
local windSpaceFreq = 0.04

local walkSpeed = 2.0   -- m/s (Shift = 4× = 8 m/s sprint)
local grassRenderDistance = 100.0   -- world units; 0 = unlimited
-- Visual-only sphere depression around the player. 0 = disabled.
local terrainDeformRadius = 0.0

-- Auto-capture: when launched with --exit-after-frames, trigger the engine's
-- built-in final-backbuffer screenshot a few frames before the cmdline-
-- requested exit. PNG lands in dumps/screenshots/final_<backend>_*.png
-- (managed by Corona::BuildFinalScreenshotPath / FinalScreenshotCounter).
local autoCaptureAtFrame = 90
local frameCounter = 0
local captureRequested = false
local freeFlyCamera = false

-- One-shot seeding of engine-side state so the Scene Inspector / engine
-- sliders see the demo's intended defaults, and so per-frame writes don't
-- clobber user edits coming from another panel.
local wantInitialBroadcast = true

local function on_imgui()
    if not imgui.begin("Terrain Demo") then
        imgui.end_window()
        return
    end
    local newFree, changed = imgui.checkbox("Free-fly camera (WASD)", freeFlyCamera)
    if changed then
        freeFlyCamera = newFree
        if corona.use_native_camera then
            corona.use_native_camera(freeFlyCamera)
        end
    end
    -- Each slider only writes to the engine when its value actually changes;
    -- this lets the Scene Inspector (or other panels) edit the same engine
    -- state without being clobbered by the script the next frame.
    local v, c
    v, c = imgui.slider_float("grass distance (m)", grassRenderDistance, 0.0, 1500.0)
    if c then grassRenderDistance = v; corona.set_grass_render_distance(grassRenderDistance) end
    v, c = imgui.slider_float("deform radius (m)", terrainDeformRadius, 0.0, 30.0)
    if c then terrainDeformRadius = v end
    v, c = imgui.slider_float("wind strength", windStrength, 0.0, 3.0)
    if c then windStrength = v; corona.set_wind_params(windDirX, windDirZ, windStrength, windTempFreq, windSpaceFreq) end
    v, c = imgui.slider_float("wind freq (time)", windTempFreq, 0.0, 40.0)
    if c then windTempFreq = v; corona.set_wind_params(windDirX, windDirZ, windStrength, windTempFreq, windSpaceFreq) end
    v, c = imgui.slider_float("wind freq (space)", windSpaceFreq, 0.0, 0.5)
    if c then windSpaceFreq = v; corona.set_wind_params(windDirX, windDirZ, windStrength, windTempFreq, windSpaceFreq) end
    imgui.text(string.format("player @ (%.1f, %.1f, %.1f)", player.x, player.y, player.z))
    imgui.end_window()
end

-- Re-fetch handles whose backing ECS entity was destroyed by a console
-- `loadmap` (clear + replay). Without this the dynamic loop below keeps
-- writing transforms to stale handles → player sinks into the terrain
-- and the camera falls below the ground.
local function ensureHandle(name, current)
    if current and corona.Entity.exists(current) then
        return current
    end
    return corona.Entity.by_name(name)
end

local function on_update(dt)
    playerEntity  = ensureHandle("TerrainDemoPlayer",  playerEntity)
    cameraEntity  = ensureHandle("TerrainDemoCamera",  cameraEntity)
    terrainEntity = ensureHandle("TerrainDemoTerrain", terrainEntity)
    grassEntity   = ensureHandle("TerrainDemoGrass",   grassEntity)
    sunEntity     = ensureHandle("TerrainDemoSun",     sunEntity)

    -- Free-fly mode: SimpleCamera owns WASD + mouse rotation, the player
    -- stays still, the orbit-follow camera is disabled. Only the SPACE
    -- spark spawn (below) and ImGui controls run; the rest of this
    -- function returns early.
    if freeFlyCamera then
        if corona.was_key_pressed and corona.was_key_pressed("SPACE") then
            local spawnY = (player.y or 0.0) + 0.5
            corona.particle_burst(player.x, spawnY, player.z, 288)
        end
        return
    end

    local m = corona.get_mouse()
    if m and m.right_down then
        camYaw   = camYaw   - (m.dx or 0) * mouseSensitivity
        camPitch = camPitch + (m.dy or 0) * mouseSensitivity
        local maxPitch = math.pi * 0.45
        if camPitch >  maxPitch then camPitch =  maxPitch end
        if camPitch < -maxPitch then camPitch = -maxPitch end
    end

    local sy, cy = math.sin(camYaw), math.cos(camYaw)
    local fx, fz =  sy,  cy
    local rx, rz = -cy,  sy

    local mf, mr = 0.0, 0.0
    if corona.is_key_down("W") or corona.is_key_down("UP")    then mf = mf + 1.0 end
    if corona.is_key_down("S") or corona.is_key_down("DOWN")  then mf = mf - 1.0 end
    if corona.is_key_down("D") or corona.is_key_down("RIGHT") then mr = mr + 1.0 end
    if corona.is_key_down("A") or corona.is_key_down("LEFT")  then mr = mr - 1.0 end

    local speed = walkSpeed
    if corona.is_key_down("SHIFT") then speed = speed * 4.0 end

    -- Space → burst a fistful of sparks at the player's feet. Bound to a
    -- one-shot "pressed this frame" check so holding the key doesn't drain
    -- the pool every frame.
    if corona.was_key_pressed and corona.was_key_pressed("SPACE") then
        local spawnY = (player.y or 0.0) + 0.5
        local n = corona.particle_burst(player.x, spawnY, player.z, 288)
        corona.log(string.format("[terrain_demo] spark burst @ (%.1f, %.1f, %.1f) n=%d",
            player.x, spawnY, player.z, n))
    end

    local lenSq = mf * mf + mr * mr
    if lenSq > 0.0 then
        local inv = 1.0 / math.sqrt(lenSq)
        local moveX = (fx * mf + rx * mr) * inv
        local moveZ = (fz * mf + rz * mr) * inv
        player.x = player.x + moveX * speed * dt
        player.z = player.z + moveZ * speed * dt
        player.yaw = math.deg(math.atan2(moveX, moveZ))
    end

    local halfExtent = 1000.0   -- terrain is ~2km
    if player.x < -halfExtent then player.x = -halfExtent end
    if player.x >  halfExtent then player.x =  halfExtent end
    if player.z < -halfExtent then player.z = -halfExtent end
    if player.z >  halfExtent then player.z =  halfExtent end

    player.y = corona.terrain_sample_height(player.x, player.z)

    corona.MeshComponent.set(playerEntity, {
        position = { player.x, player.y, player.z },
        rotation = { 0.0, player.yaw, 0.0 },
    })

    -- Player sits at the deform sphere's center; opt its mesh out so the
    -- shockwave carve doesn't suck the avatar into its own crater.
    -- Called every frame (cheap idempotent flag write) so that handle
    -- rebinds after a console `loadmap` or `reload` keep the exclusion
    -- in sync with the freshly-rebuilt MeshResource.
    corona.set_entity_exclude_deform_sphere(playerEntity, true)

    -- Third-person orbit camera.
    local cp = math.cos(camPitch)
    local camOffsetX = -math.sin(camYaw) * camDistance * cp
    local camOffsetZ = -math.cos(camYaw) * camDistance * cp
    local camOffsetY =  math.sin(camPitch) * camDistance + camHeightOffset
    corona.CameraComponent.set(cameraEntity, {
        position = { player.x + camOffsetX, player.y + camOffsetY, player.z + camOffsetZ },
        look_at  = { player.x, player.y + player.eyeHeight, player.z },
        -- intentionally omit fov / up so per-frame writes don't clobber
        -- inspector edits or values restored from the saved map.
        active   = true,
    })

    -- One-shot sync on the first frame. Pull the engine's current state
    -- (set by map load, or the engine's hardcoded defaults if no map
    -- existed) into our locals so the demo panel's sliders show the live
    -- values and we don't clobber map-restored params with hardcoded
    -- defaults like grassRenderDistance=100.
    if wantInitialBroadcast then
        if corona.get_grass_render_distance then
            grassRenderDistance = corona.get_grass_render_distance()
        end
        if corona.get_wind_params then
            local w = corona.get_wind_params()
            if w then
                windDirX      = w.dir_x      or windDirX
                windDirZ      = w.dir_z      or windDirZ
                windStrength  = w.strength   or windStrength
                windTempFreq  = w.temp_freq  or windTempFreq
                windSpaceFreq = w.space_freq or windSpaceFreq
            end
        end
        wantInitialBroadcast = false
    end
    -- These two follow the player every frame — they're not user-edited via
    -- the inspector, so re-writing them is correct.
    -- y carries the player's terrain height so the procedural grass
    -- shader can anchor blade Y to the player's elevation while the
    -- existing legacy bend code (which only reads .xz) is unaffected.
    corona.set_grass_bend_origin(player.x, player.y, player.z, 2.0)
    corona.set_grass_render_origin(player.x, player.y, player.z)
    -- Sphere depression centered at the player's terrain-height position.
    -- Visual-only — player stays on the original terrain Y (no fall-in).
    corona.set_terrain_deform_sphere(player.x, player.y, player.z, terrainDeformRadius)

    -- Auto-capture: trigger after warmup so frame timing settles. The
    -- engine handles the actual capture on the following frame (without
    -- ImGui in the shot), then exit-after-frames closes the window.
    frameCounter = frameCounter + 1
    if not captureRequested and frameCounter >= autoCaptureAtFrame then
        captureRequested = true
        corona.run_ui_command("capture_final_backbuffer")
        corona.log("[terrain_demo] capture_final_backbuffer requested at frame " .. frameCounter)
    end
end

return { update = on_update, imgui = on_imgui }
