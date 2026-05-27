-- Procedural grass demo: WASD-controlled dungeon character walking
-- through a field of grass. Grass blades bend away from the player.
--
-- Triggered by `--grass-demo` (or `--startup-mode=grass_demo`).
--
-- Scene composition:
--   • Camera (third-person, follows player)
--   • Directional sun
--   • Ground plane (legacy 44 B Vertex layout)
--   • Procedural grass mesh (one Scene, N blades; bGrassMesh=true)
--   • Procedural block-character "player" entity
--
-- Each frame:
--   • WASD updates player.x / player.z
--   • Camera follows player
--   • corona.set_grass_bend_origin(player.x, 0, player.z, strength)
--     drives the GBuffer CB → grass blades within bendRadius lean away
--     from the player position.

local groundExtent      = 1000.0   -- ground half-extent (world units)
local grassAreaSize     = 1800.0   -- area covered by grass blades
local grassBladeCount   = 8000     -- N blades
local grassBladeHeight  = 28.0
local grassBendRadius   = 180.0    -- world units of effect radius
local grassBendStrength = 30.0
local playerWalkSpeed   = 300.0    -- units per second
local cameraDistance    = 200.0
local cameraHeight      = 90.0     -- lower → bend more visible

corona.set_grass_bend_params(grassBendRadius, grassBladeHeight)

-- 1. Ground plane ---------------------------------------------------------
local groundEntity = corona.Entity.create("GrassDemoGround")
corona.MeshComponent.add(groundEntity, {
    primitive = "BOX",
    color = { 0.45, 0.38, 0.28 },
    texture_kind = "stone",
    position = { 0.0, -5.0, 0.0 },
    rotation = { 0.0, 0.0, 0.0 },
    scale    = { groundExtent * 2.0, 10.0, groundExtent * 2.0 },
    roughness = 0.9,
    metallic = 0.0,
    override_material = true,
    ray_tracing = true,
})

-- 2. Procedural grass -----------------------------------------------------
local grassEntity = corona.Entity.create("GrassDemoGrass")
corona.MeshComponent.add(grassEntity, {
    primitive    = "GRASS",
    blade_count  = grassBladeCount,
    area_size    = grassAreaSize,
    blade_height = grassBladeHeight,
    seed         = 20260527,
    position     = { 0.0, 0.0, 0.0 },
    rotation     = { 0.0, 0.0, 0.0 },
    scale        = { 1.0, 1.0, 1.0 },
    roughness    = 0.85,
    metallic     = 0.0,
    override_material = true,
    ray_tracing  = false,
})

-- 3. Player (block character) --------------------------------------------
local player = {
    x   = 0.0,
    y   = 0.0,
    z   = 0.0,
    yaw = 0.0,
}
local playerEntity = corona.Entity.create("GrassDemoPlayer")
corona.MeshComponent.add(playerEntity, {
    primitive   = "block_character",
    seed        = 11337,
    position    = { player.x, player.y, player.z },
    rotation    = { 0.0, player.yaw, 0.0 },
    target_extent = 75.0,
    roughness   = 0.7,
    metallic    = 0.0,
    override_material = true,
    ray_tracing = true,
})

-- 4. Camera --------------------------------------------------------------
local cameraEntity = corona.Entity.create("GrassDemoCamera")
corona.CameraComponent.add(cameraEntity, {
    position = { 0.0, cameraHeight, -cameraDistance },
    look_at  = { 0.0, 50.0, 0.0 },
    up       = { 0.0, 1.0, 0.0 },
    fov      = 0.55,
    near_plane = 1.0,
    far_plane  = 12000.0,
    active   = true,
})

-- 5. Sun -----------------------------------------------------------------
local sunEntity = corona.Entity.create("GrassDemoSun")
corona.LightComponent.add(sunEntity, {
    type      = "directional",
    enabled   = true,
    direction = { -0.4, -1.0, 0.3 },
    color     = { 1.0, 0.95, 0.85 },
    intensity = 2.4,
    persistent = false,
})

corona.log(string.format(
    "[grass_demo] spawned ground + grass(blades=%d area=%.0f height=%.0f) + player + camera",
    grassBladeCount, grassAreaSize, grassBladeHeight))

-- 6. Per-frame update ----------------------------------------------------

local function on_update(dt)
    local moveX = 0.0
    local moveZ = 0.0
    if corona.is_key_down("W") or corona.is_key_down("UP")    then moveZ = moveZ + 1.0 end
    if corona.is_key_down("S") or corona.is_key_down("DOWN")  then moveZ = moveZ - 1.0 end
    if corona.is_key_down("A") or corona.is_key_down("LEFT")  then moveX = moveX - 1.0 end
    if corona.is_key_down("D") or corona.is_key_down("RIGHT") then moveX = moveX + 1.0 end

    local lenSq = moveX * moveX + moveZ * moveZ
    if lenSq > 0.0 then
        local inv = 1.0 / math.sqrt(lenSq)
        moveX = moveX * inv
        moveZ = moveZ * inv
        player.x = player.x + moveX * playerWalkSpeed * dt
        player.z = player.z + moveZ * playerWalkSpeed * dt
        player.yaw = math.deg(math.atan2(moveX, moveZ))
    end

    -- Clamp to grass area so the player doesn't wander off the field.
    local halfArea = grassAreaSize * 0.5
    if player.x < -halfArea then player.x = -halfArea end
    if player.x >  halfArea then player.x =  halfArea end
    if player.z < -halfArea then player.z = -halfArea end
    if player.z >  halfArea then player.z =  halfArea end

    -- Push player transform into the engine.
    corona.MeshComponent.set(playerEntity, {
        position = { player.x, player.y, player.z },
        rotation = { 0.0, player.yaw, 0.0 },
    })

    -- Camera follows behind player.
    local camOffsetX = -math.sin(math.rad(player.yaw)) * cameraDistance
    local camOffsetZ = -math.cos(math.rad(player.yaw)) * cameraDistance
    corona.CameraComponent.set(cameraEntity, {
        position = { player.x + camOffsetX, cameraHeight, player.z + camOffsetZ },
        look_at  = { player.x, 20.0, player.z },
        up       = { 0.0, 1.0, 0.0 },
        fov      = 0.55,
        active   = true,
    })

    -- Drive grass bend origin in world space. .w = strength (0 disables).
    corona.set_grass_bend_origin(player.x, 0.0, player.z, grassBendStrength)
end

return { update = on_update }
