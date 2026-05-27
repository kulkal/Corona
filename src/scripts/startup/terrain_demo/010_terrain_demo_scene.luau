-- Procedural terrain demo: a 1km × 1km heightmap-generated terrain
-- (FBM + ridged + domain warp Simplex noise) rendered with the engine's
-- GBuffer pass and gray Phase-1 material.
--
-- Triggered by `--terrain-demo` (or `--startup-mode=terrain_demo`).
--
-- Scene composition:
--   • Camera (free-fly, looking at terrain center from elevated SE corner)
--   • Directional sun
--   • Procedural terrain (1024×1024 samples, 256 chunks @ 65×65 verts)

-- Camera at terrain center area; engine auto-centers terrain around origin
-- (translates by (-Width/2, 0, -Depth/2)) so positions ~origin are sensible.
local cameraStartX = 0.0
local cameraStartY = 200.0
local cameraStartZ = -300.0
local cameraLookX  = 0.0
local cameraLookY  = 50.0
local cameraLookZ  = 0.0

-- 1. Terrain --------------------------------------------------------------
local terrainEntity = corona.Entity.create("TerrainDemoTerrain")
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

-- 2. Camera --------------------------------------------------------------
local cameraEntity = corona.Entity.create("TerrainDemoCamera")
corona.CameraComponent.add(cameraEntity, {
    position   = { cameraStartX, cameraStartY, cameraStartZ },
    look_at    = { cameraLookX,  cameraLookY,  cameraLookZ },
    up         = { 0.0, 1.0, 0.0 },
    fov        = 0.55,
    near_plane = 1.0,
    far_plane  = 8000.0,
    active     = true,
})

-- 3. Sun -----------------------------------------------------------------
local sunEntity = corona.Entity.create("TerrainDemoSun")
corona.LightComponent.add(sunEntity, {
    type       = "directional",
    enabled    = true,
    direction  = { -0.35, -1.0, 0.45 },
    color      = { 1.0, 0.97, 0.88 },
    intensity  = 2.6,
    persistent = false,
})

corona.log("[terrain_demo] spawned terrain (1024^2, seed=1) + camera + sun")

local function on_update(dt)
end

local function on_imgui()
end

return { update = on_update, imgui = on_imgui }
