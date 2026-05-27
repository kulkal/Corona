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

-- Free-fly camera state. CameraComponent.add disables the engine's native
-- WASD/mouse path (bScriptCameraControlEnabled=true), so we drive position
-- + look_at every frame here.
local cam = {
    x = cameraStartX, y = cameraStartY, z = cameraStartZ,
    yaw = 0.0,      -- world rotation around Y (radians), 0 looks toward +Z
    pitch = 0.0,    -- up/down (radians)
}

-- Aim yaw/pitch so the initial look_at points at the configured target.
do
    local dx = cameraLookX - cam.x
    local dy = cameraLookY - cam.y
    local dz = cameraLookZ - cam.z
    cam.yaw = math.atan2(dx, dz)
    local horizLen = math.sqrt(dx * dx + dz * dz)
    cam.pitch = math.atan2(dy, math.max(horizLen, 0.0001))
end

local moveSpeed = 200.0      -- world units / sec  (Shift = 4x)
local mouseSensitivity = 0.0035

local function on_update(dt)
    -- Mouse look: hold right mouse button to rotate
    local m = corona.get_mouse()
    if m and m.right_down then
        cam.yaw = cam.yaw - (m.dx or 0) * mouseSensitivity
        cam.pitch = cam.pitch - (m.dy or 0) * mouseSensitivity
        local maxPitch = math.pi * 0.49
        if cam.pitch >  maxPitch then cam.pitch =  maxPitch end
        if cam.pitch < -maxPitch then cam.pitch = -maxPitch end
    end

    -- Forward/right vectors in world space from yaw/pitch.
    -- Corona is right-handed (BuildSafeLookAtRH) with Y up. For yaw=0
    -- forward=+Z, the strafe-right direction is -X (forward × up).
    local cp = math.cos(cam.pitch)
    local sp = math.sin(cam.pitch)
    local sy = math.sin(cam.yaw)
    local cy = math.cos(cam.yaw)
    local fx, fy, fz = sy * cp, sp, cy * cp
    local rx, rz = -cy, sy

    -- WASD + QE for vertical, Shift sprint
    local mf, mr, mu = 0.0, 0.0, 0.0
    if corona.is_key_down("W") or corona.is_key_down("UP")    then mf = mf + 1.0 end
    if corona.is_key_down("S") or corona.is_key_down("DOWN")  then mf = mf - 1.0 end
    if corona.is_key_down("D") or corona.is_key_down("RIGHT") then mr = mr + 1.0 end
    if corona.is_key_down("A") or corona.is_key_down("LEFT")  then mr = mr - 1.0 end
    if corona.is_key_down("E") or corona.is_key_down("SPACE") then mu = mu + 1.0 end
    if corona.is_key_down("Q") or corona.is_key_down("CTRL")  then mu = mu - 1.0 end

    local speed = moveSpeed
    if corona.is_key_down("SHIFT") then speed = speed * 4.0 end

    cam.x = cam.x + (fx * mf + rx * mr) * speed * dt
    cam.y = cam.y + (fy * mf + mu) * speed * dt
    cam.z = cam.z + (fz * mf + rz * mr) * speed * dt

    corona.CameraComponent.set(cameraEntity, {
        position = { cam.x, cam.y, cam.z },
        look_at  = { cam.x + fx, cam.y + fy, cam.z + fz },
        up       = { 0.0, 1.0, 0.0 },
        fov      = 0.55,
        active   = true,
    })
end

return { update = on_update }
