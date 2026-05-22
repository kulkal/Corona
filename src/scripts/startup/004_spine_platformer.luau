local script = {}

corona_script_controls = corona_script_controls or {}
corona_script_controls.platformer = corona_script_controls.platformer or {}
corona_script_controls.dungeon = corona_script_controls.dungeon or {}
corona_script_controls.pistol = corona_script_controls.pistol or {}
corona_script_controls.props = corona_script_controls.props or {}
corona_script_controls.spine = corona_script_controls.spine or {}

local controls = corona_script_controls.platformer
if controls.enabled == nil then
    controls.enabled = true
end
if controls.showHud == nil then
    controls.showHud = true
end
if controls.enemyStreaming == nil then
    controls.enemyStreaming = true
end
if controls.maxSceneSamplesPerFrame == nil then
    controls.maxSceneSamplesPerFrame = 1
end
if controls.maxEnemySpawnsPerFrame == nil then
    controls.maxEnemySpawnsPerFrame = 1
end
if controls.levelStreaming == nil then
    controls.levelStreaming = true
end
if controls.streamingZoneWidth == nil then
    controls.streamingZoneWidth = 780.0
end
if controls.streamingPreloadMargin == nil then
    controls.streamingPreloadMargin = 460.0
end
if controls.streamingUnloadMargin == nil then
    controls.streamingUnloadMargin = 980.0
end
if controls.maxLevelZoneLoadsPerFrame == nil then
    controls.maxLevelZoneLoadsPerFrame = 1
end
if controls.maxLevelZoneObjectsPerFrame == nil then
    controls.maxLevelZoneObjectsPerFrame = 8
end
if controls.maxInitialZoneLoads == nil then
    controls.maxInitialZoneLoads = 3
end
if controls.fallDeathEnabled == nil then
    controls.fallDeathEnabled = true
end
if controls.fallRespawnDelay == nil then
    controls.fallRespawnDelay = 0.85
end
if controls.maxLives == nil then
    controls.maxLives = 5
end
if controls.maxHealth == nil then
    controls.maxHealth = 100
end
if controls.enemyDamage == nil then
    controls.enemyDamage = 5
end
if controls.hitInvulnerabilitySeconds == nil then
    controls.hitInvulnerabilitySeconds = 0.65
end
if controls.startInMenu == nil then
    controls.startInMenu = false
end
if controls.menuPreviewAnimation == nil then
    controls.menuPreviewAnimation = false
end

if not controls.enabled then
    return script
end

corona_script_controls.dungeon.enabled = false
corona_script_controls.pistol.enabled = false
corona_script_controls.props.buddhaEnabled = false
corona_script_controls.props.shaderBallEnabled = false
corona_script_controls.spine.enabled = false

corona.set_default_world_visible(false)
corona.set_camera_control(true)
corona.set_ui_value("enable_diffuse_gi", false)
corona.set_ui_value("enable_specular_gi", false)
corona.set_ui_value("enable_sky_lighting", true)
corona.set_ui_value("enable_ray_traced_sky_lighting", false)
corona.set_ui_value("enable_prefiltered_env_specular", false)
corona.set_ui_value("sky_lighting_strength", controls.skyLightingStrength or 0.08)
corona.set_ui_value("sky_intensity", controls.skyIntensity or 0.12)
corona.set_ui_value("surface_bounce_strength", controls.surfaceBounceStrength or 0.02)
corona.set_ui_value("tone_map_mode", 0)

local enemyAssets = corona_platformer_enemy_assets or {}
local totalEnemyAssets = #enemyAssets
local levelLayoutDocument = corona_platformer_level_layouts or {}
local levelLayouts = levelLayoutDocument.levels or {}
local levelPhysics = levelLayoutDocument.physics or {}

local spawned = {}
local platformData = {}
local levelZones = {}
local levelZoneWidth = math.max(120.0, controls.streamingZoneWidth or 780.0)
local levelStreamingEnabled = controls.levelStreaming ~= false
local loadedLevelZoneCount = 0
local levelStreamingLoadFrame = 0
local sceneCache = {}
local failedSceneCache = {}
local enemies = {}
local effects = {}
local loopEffects = {}
local combat = {}
local effectEntityPools = {}
local effectPoolPrewarmed = false
local effectPoolLimitPerKind = math.max(0, math.floor(controls.effectPoolLimitPerKind or 8))
local effectPrewarmPerKind = math.max(0, math.floor(controls.effectPrewarmPerKind or 1))
local sceneSamplesThisFrame = 0
local enemySpawnsThisFrame = 0
local lastFrame = -1
local lastAnimation = ""
local lastCharacterHandle = nil
local time = 0.0
local attackTimer = 0.0
local winPulse = 0.0
local activeEnemyCount = 0
local defeatedEnemyCount = 0
local deathCount = 0
local fallDeathCount = 0
local currentLevel = 1
local currentLevelEnemyStart = 1
local currentLevelEnemyEnd = 0
local currentLevelEnemyTotal = 0
local levelAdvanceTimer = 0.0
local deathTimer = 0.0
local gameCompleted = false
local gameState = controls.startInMenu and "menu" or "playing"
local menuReason = controls.startInMenu and "title" or ""
local maxLives = math.max(1, math.floor(controls.maxLives or 5))
local maxHealth = math.max(1, math.floor(controls.maxHealth or 100))
local enemyDamage = math.max(1, math.floor(controls.enemyDamage or 5))
local lives = maxLives
local health = maxHealth
local hitInvulnerabilityTimer = 0.0
local load_level = nil
local start_new_game = nil
local gamepadJumpHeldLast = false
local gamepadAttackTriggerHeldLast = false

local playerAssetName = "ares"
local playerDisplayName = "Ares"
local spineAsset = "assets/spine/chars/ares/ares.json"
local playerSampleFps = 12.0
local enemySampleFps = 6.0
local loopSeconds = {
    idle = 2.0,
    walk = 0.95,
    attack = 0.65,
}
local playerAnimationSampleFps = {
    idle = 6.0,
    walk = 12.0,
    attack = 12.0,
}

local levelCount = #levelLayouts > 0 and #levelLayouts or 20
local enemiesPerLevel = math.max(1, math.ceil(math.max(1, totalEnemyAssets) / levelCount))
local enemyStartX = -180.0
local enemySpacing = 260.0
local firstPlayableX = -880.0
local startX = -620.0
local worldMinX = -810.0
local goalX = 5200.0
local levelEndX = goalX + 260.0
local levelAdvanceDelay = 0.65
local currentLayout = nil
local cameraFocusX = startX
local cameraFocusY = 180.0

local player = {
    x = -760.0,
    y = 0.0,
    vx = 0.0,
    vy = 0.0,
    facing = 1.0,
    grounded = false,
    dead = false,
    deathReason = "",
    groundPlatform = nil,
    won = false,
}

local respawnX = startX
local respawnY = 18.0
local respawnFacing = 1.0

local playerRadius = 30.0
local playerHeight = 142.0
local sharedCharacterVisualExtent = math.max(1.0, controls.characterVisualExtent or 150.0)
local characterSizeVarianceExponent = math.max(0.0, math.min(controls.characterSizeVarianceExponent or 0.25, 1.0))
local characterMinVisualScale = math.max(0.1, controls.characterMinVisualScale or 0.84)
local characterMaxVisualScale = math.max(characterMinVisualScale, controls.characterMaxVisualScale or 1.18)
local playerVisualExtent = sharedCharacterVisualExtent
local playerVisualFootOffset = controls.playerVisualFootOffset or 0.0
local enemyVisualExtent = sharedCharacterVisualExtent
local largeEnemyVisualExtent = sharedCharacterVisualExtent
local smallEnemyVisualExtent = sharedCharacterVisualExtent
local spritePlaneZ = controls.spritePlaneZ or -72.0
local characterZ = spritePlaneZ
local enemyZ = spritePlaneZ
local spineEffectZ = spritePlaneZ
local platformZ = 14.0
local platformDepth = 74.0
local moveSpeed = levelPhysics.move_speed or 330.0
local acceleration = levelPhysics.acceleration or 1780.0
local friction = levelPhysics.friction or 1420.0
local jumpVelocity = levelPhysics.jump_velocity or 780.0
local stompBounceVelocity = levelPhysics.stomp_bounce_velocity or 560.0
local gravity = levelPhysics.gravity or -1500.0
local maxFallSpeed = levelPhysics.max_fall_speed or -940.0
local worldMinY = levelPhysics.world_min_y or -260.0
local enemySpawnDistance = 1280.0
local enemyDespawnDistance = 1540.0
local meleeAttackWindow = 0.22
local meleeAttackRange = 118.0
local meleeAttackBackReach = 18.0
local meleeAttackVerticalReach = 92.0
local stompForgiveness = 24.0
local respawnPlatformInset = playerRadius + 12.0

local effectAssets = {
    dust = {
        path = "assets/spine/effects/dust/dust.json",
        animation = "idle",
        duration = 0.42,
        fps = 18.0,
        targetExtent = 92.0,
    },
    hit = {
        path = "assets/spine/effects/hit/hit.json",
        animation = "idle",
        duration = 0.28,
        fps = 24.0,
        targetExtent = 96.0,
    },
    explosion = {
        path = "assets/spine/effects/explosion/explosion.json",
        animation = "idle",
        duration = 0.52,
        fps = 22.0,
        targetExtent = 150.0,
    },
    spawn = {
        path = "assets/spine/effects/spawn/spawn.json",
        animation = "idle",
        duration = 0.50,
        fps = 18.0,
        targetExtent = 118.0,
    },
    skill_ready = {
        path = "assets/spine/effects/skill_ready/skill_ready.json",
        animation = "idle",
        duration = 1.05,
        fps = 18.0,
        targetExtent = 178.0,
    },
    movingflag = {
        path = "assets/spine/effects/movingflag/movingflag.json",
        animation = "idle",
        duration = 1.0,
        fps = 18.0,
        targetExtent = 132.0,
    },
    fire = {
        path = "assets/spine/effects/fire/fire.json",
        animation = "idle",
        duration = 0.86,
        fps = 18.0,
        targetExtent = 78.0,
    },
    lightning = {
        path = "assets/spine/effects/lightning/lightning.json",
        animation = "idle",
        duration = 0.62,
        fps = 22.0,
        targetExtent = 170.0,
    },
}

local native_spine_create_scene = nil
local native_spine_set_pose = nil
local native_spine_get_scene_height = nil
if corona.SpineComponent ~= nil then
    native_spine_create_scene = corona.SpineComponent.create_scene
    native_spine_set_pose = corona.SpineComponent.set_pose
    native_spine_get_scene_height = corona.SpineComponent.get_scene_height
end

local function set_spine_pose(entity, sceneHandle, x, y, z, yaw, targetExtent, roughness, metallic, mirrorX, useWorldScale)
    if native_spine_set_pose ~= nil then
        return native_spine_set_pose(
            entity,
            sceneHandle or 0,
            x,
            y,
            z,
            yaw,
            targetExtent,
            roughness or 1.0,
            metallic or 0.0,
            mirrorX == true,
            useWorldScale == true)
    end

    local patch = {
        position = { x, y, z },
        rotation = { 0.0, yaw, 0.0 },
        ray_tracing = false,
        physics = false,
    }
    if useWorldScale == true then
        local signedScaleX = mirrorX and -targetExtent or targetExtent
        patch.scale = { signedScaleX, targetExtent, targetExtent }
    else
        patch.target_extent = targetExtent
    end
    if sceneHandle ~= nil then
        patch.scene_handle = sceneHandle
        patch.roughness = roughness or 1.0
        patch.metallic = metallic or 0.0
    end
    return corona.MeshComponent.set(entity, patch)
end

local function set_effect_entity_visible(entity, visible)
    if entity == nil then
        return false
    end
    if corona.MeshComponent ~= nil and corona.MeshComponent.set_visible ~= nil then
        return corona.MeshComponent.set_visible(entity, visible == true)
    end
    if corona.entity_set_visible ~= nil then
        return corona.entity_set_visible(entity, visible == true)
    end
    return false
end

local characterReferenceSourceHeight = nil

local function get_spine_scene_source_height(handle)
    if handle == nil then
        return nil
    end
    local height = nil
    if native_spine_get_scene_height ~= nil then
        height = native_spine_get_scene_height(handle)
    elseif corona.get_spine_scene_height ~= nil then
        height = corona.get_spine_scene_height(handle)
    end
    if height ~= nil and height > 0.001 then
        return height
    end
    return nil
end

local function set_character_scale_reference(handle)
    if characterReferenceSourceHeight ~= nil then
        return
    end

    local sourceHeight = get_spine_scene_source_height(handle)
    if sourceHeight == nil then
        return
    end

    characterReferenceSourceHeight = sourceHeight
    corona.log(
        "platformer character visual normalization reference_source_height=" .. tostring(characterReferenceSourceHeight) ..
        " target_visible_extent=" .. tostring(sharedCharacterVisualExtent) ..
        " size_variance_exponent=" .. tostring(characterSizeVarianceExponent) ..
        " clamp=" .. tostring(characterMinVisualScale) .. "-" .. tostring(characterMaxVisualScale))
end

local function set_character_spine_pose(entity, sceneHandle, x, y, z, yaw, visualExtent, roughness, metallic, mirrorX)
    return set_spine_pose(
        entity,
        sceneHandle,
        x,
        y,
        z,
        yaw,
        visualExtent or sharedCharacterVisualExtent,
        roughness,
        metallic,
        mirrorX,
        false)
end

local function character_visual_extent_from_scene(handle)
    local sourceHeight = get_spine_scene_source_height(handle)
    if sourceHeight == nil or characterReferenceSourceHeight == nil then
        return sharedCharacterVisualExtent
    end

    local ratio = math.max(0.001, sourceHeight / characterReferenceSourceHeight)
    local visualScale = math.max(characterMinVisualScale, math.min(ratio ^ characterSizeVarianceExponent, characterMaxVisualScale))
    return math.max(1.0, sharedCharacterVisualExtent * visualScale)
end

local character = nil
local cameraEntity = nil
local sunEntity = nil
local goalEntity = nil
local goalFlagParts = {}
local destroy_effect = nil
local spawn_spine_effect = nil
local update_spine_effects = nil
local update_character = nil
local update_camera = nil
local update_level_streaming = nil
local destroy_level_entities = nil

local function clamp(value, minValue, maxValue)
    if value < minValue then
        return minValue
    end
    if value > maxValue then
        return maxValue
    end
    return value
end

local function approach(value, target, amount)
    if value < target then
        return math.min(value + amount, target)
    end
    if value > target then
        return math.max(value - amount, target)
    end
    return target
end

local function get_player_animation_sample_fps(animation)
    return playerAnimationSampleFps[animation] or playerSampleFps
end

local function screen_axis_to_world_axis(axis)
    return -axis
end

local function enemy_mirror_x(enemy)
    return enemy ~= nil and (enemy.facing or 1.0) > 0.0
end

combat.projectiles = {}
combat.rangedCooldownTimer = 0.0
combat.projectileCounter = 0
combat.rangedCooldownSeconds = math.max(0.04, controls.rangedCooldownSeconds or 0.38)
combat.rangedProjectileSpeed = math.max(80.0, controls.rangedProjectileSpeed or 620.0)
combat.rangedProjectileLifetime = math.max(0.12, controls.rangedProjectileLifetime or 1.15)
combat.rangedProjectileRadius = math.max(8.0, controls.rangedProjectileRadius or 34.0)
combat.rangedProjectileStartOffset = controls.rangedProjectileStartOffset or 58.0
combat.rangedProjectileVerticalOffset = controls.rangedProjectileVerticalOffset or 82.0
combat.rangedProjectileExtent = math.max(8.0, controls.rangedProjectileExtent or 86.0)

local function refresh_vitals_config()
    maxLives = math.max(1, math.floor(controls.maxLives or 5))
    maxHealth = math.max(1, math.floor(controls.maxHealth or 100))
    enemyDamage = math.max(1, math.floor(controls.enemyDamage or 5))
    lives = clamp(lives, 0, maxLives)
    health = clamp(health, 0, maxHealth)
end

local function request_exit()
    if corona.request_exit ~= nil then
        corona.request_exit()
    else
        corona.log("platformer exit requested")
    end
end

local function add_spawned(entity)
    if entity ~= nil then
        table.insert(spawned, entity)
    end
    return entity
end

local function set_respawn_point(x, y, facing, platform)
    local safeX = x or startX
    if platform ~= nil then
        safeX = clamp(safeX, platform.left + respawnPlatformInset, platform.right - respawnPlatformInset)
    end
    respawnX = clamp(safeX, worldMinX, levelEndX)
    respawnY = math.max(y or 18.0, 18.0)
    if facing ~= nil and math.abs(facing) > 0.001 then
        respawnFacing = facing >= 0.0 and 1.0 or -1.0
    end
end

local function update_safe_respawn_point()
    if player.dead or player.won or not player.grounded then
        return
    end
    set_respawn_point(player.x, player.y + 2.0, player.facing, player.groundPlatform)
end

local function configure_level(levelIndex)
    currentLevel = clamp(math.floor(levelIndex or 1), 1, levelCount)
    currentLayout = levelLayouts[currentLevel]
    currentLevelEnemyStart = (currentLevel - 1) * enemiesPerLevel + 1
    currentLevelEnemyEnd = math.min(totalEnemyAssets, currentLevel * enemiesPerLevel)
    if currentLevelEnemyStart > totalEnemyAssets then
        currentLevelEnemyStart = totalEnemyAssets + 1
        currentLevelEnemyEnd = totalEnemyAssets
    end
    currentLevelEnemyTotal = math.max(0, currentLevelEnemyEnd - currentLevelEnemyStart + 1)

    if currentLayout ~= nil then
        firstPlayableX = currentLayout.first_playable_x or -880.0
        startX = currentLayout.start_x or -620.0
        worldMinX = firstPlayableX + 70.0
        levelEndX = currentLayout.end_x or (firstPlayableX + 6200.0)
        goalX = currentLayout.goal_x or (levelEndX - 320.0)
        enemyStartX = currentLayout.enemy_start_x or (firstPlayableX + 650.0)
    else
        firstPlayableX = -880.0
        startX = -620.0
        worldMinX = firstPlayableX + 70.0
        local levelWidth = 6200.0 + (currentLevel - 1) * 260.0 + math.max(0, currentLevelEnemyTotal - 10) * 72.0
        goalX = firstPlayableX + levelWidth - 320.0
        levelEndX = firstPlayableX + levelWidth
        enemyStartX = firstPlayableX + 650.0
    end

    if currentLevelEnemyTotal > 1 then
        enemySpacing = (goalX - enemyStartX - 500.0) / (currentLevelEnemyTotal - 1)
        enemySpacing = clamp(enemySpacing, 210.0, 620.0)
    else
        enemySpacing = 260.0
    end
end

local function spawn_box(name, position, scale, color, roughness, rayTracing, ownerList)
    local entity = corona.Entity.create(name)
    local ok = corona.MeshComponent.add(entity, {
        primitive = "box",
        color = color,
        position = position,
        scale = scale,
        roughness = roughness or 0.86,
        metallic = 0.0,
        override_material = true,
        ray_tracing = false,
        physics = false,
    })
    if not ok then
        corona.Entity.destroy(entity)
        return nil
    end
    if ownerList ~= nil then
        table.insert(ownerList, entity)
        return entity
    end
    return add_spawned(entity)
end

local function spawn_platform(x, y, width, height, depth, color, ownerList, zone)
    local entity = spawn_box(
        "PlatformerBlock",
        { x, y, platformZ },
        { width, height, depth },
        color,
        0.88,
        false,
        ownerList)
    local platform = {
        x = x,
        y = y,
        w = width,
        h = height,
        left = x - width * 0.5,
        right = x + width * 0.5,
        top = y + height * 0.5,
        entity = entity,
        zone = zone,
    }
    table.insert(platformData, platform)
    if zone ~= nil then
        table.insert(zone.platforms, platform)
    end
end

local function spawn_layout_blocks(blocks, fallbackColor)
    if blocks == nil then
        return
    end

    for _, block in ipairs(blocks) do
        spawn_platform(
            block.x or 0.0,
            block.y or 0.0,
            block.w or 1.0,
            block.h or 1.0,
            block.depth or platformDepth,
            block.color or fallbackColor)
    end
end

local function reset_level_streaming()
    levelZones = {}
    loadedLevelZoneCount = 0
    levelStreamingLoadFrame = 0
end

local function clone_vec3(value, fallback)
    if value == nil then
        return { fallback[1], fallback[2], fallback[3] }
    end
    return { value[1] or fallback[1], value[2] or fallback[2], value[3] or fallback[3] }
end

local function expand_zone_bounds(zone, minX, maxX, minY, maxY, minZ, maxZ)
    zone.minX = math.min(zone.minX, minX)
    zone.maxX = math.max(zone.maxX, maxX)
    zone.minY = math.min(zone.minY, minY)
    zone.maxY = math.max(zone.maxY, maxY)
    zone.minZ = math.min(zone.minZ, minZ)
    zone.maxZ = math.max(zone.maxZ, maxZ)
end

local function get_level_zone_for_bounds(minX, maxX, minY, maxY, minZ, maxZ)
    local centerX = (minX + maxX) * 0.5
    local index = math.floor((centerX - firstPlayableX) / levelZoneWidth) + 1
    if index < 1 then
        index = 1
    end

    local zone = levelZones[index]
    if zone == nil then
        local zoneLeft = firstPlayableX + (index - 1) * levelZoneWidth
        zone = {
            index = index,
            left = zoneLeft,
            right = zoneLeft + levelZoneWidth,
            minX = zoneLeft,
            maxX = zoneLeft + levelZoneWidth,
            minY = -64.0,
            maxY = 320.0,
            minZ = -128.0,
            maxZ = 220.0,
            boxes = {},
            platforms = {},
            platformDescs = {},
            effects = {},
            effectDescs = {},
            entities = {},
            loading = false,
            loaded = false,
            loadBoxIndex = 1,
            loadPlatformIndex = 1,
            loadEffectIndex = 1,
        }
        levelZones[index] = zone
    end
    expand_zone_bounds(zone, minX, maxX, minY, maxY, minZ, maxZ)
    return zone
end

local function queue_streaming_box(name, position, scale, color, roughness, rayTracing, options)
    options = options or {}
    local px = position[1] or 0.0
    local py = position[2] or 0.0
    local pz = position[3] or 0.0
    local sx = math.max(0.001, scale[1] or 1.0)
    local sy = math.max(0.001, scale[2] or 1.0)
    local sz = math.max(0.001, scale[3] or 1.0)
    local zone = get_level_zone_for_bounds(
        px - sx * 0.5,
        px + sx * 0.5,
        py - sy * 0.5,
        py + sy * 0.5,
        pz - sz * 0.5,
        pz + sz * 0.5)

    table.insert(zone.boxes, {
        name = name,
        position = { px, py, pz },
        scale = { sx, sy, sz },
        color = clone_vec3(color, { 0.7, 0.7, 0.7 }),
        roughness = roughness,
        rayTracing = rayTracing,
        goalTrigger = options.goalTrigger == true,
        goalPart = options.goalPart,
    })
end

local function queue_streaming_platform(x, y, width, height, depth, color)
    local safeWidth = math.max(0.001, width or 1.0)
    local safeHeight = math.max(0.001, height or 1.0)
    local safeDepth = math.max(0.001, depth or platformDepth)
    local zone = get_level_zone_for_bounds(
        x - safeWidth * 0.5,
        x + safeWidth * 0.5,
        y - safeHeight * 0.5,
        y + safeHeight * 0.5,
        platformZ - safeDepth * 0.5,
        platformZ + safeDepth * 0.5)

    table.insert(zone.platformDescs, {
        x = x,
        y = y,
        w = safeWidth,
        h = safeHeight,
        depth = safeDepth,
        color = clone_vec3(color, { 0.34, 0.42, 0.36 }),
    })
end

local function queue_layout_blocks(blocks, fallbackColor)
    if blocks == nil then
        return
    end

    for _, block in ipairs(blocks) do
        queue_streaming_platform(
            block.x or 0.0,
            block.y or 0.0,
            block.w or 1.0,
            block.h or 1.0,
            block.depth or platformDepth,
            block.color or fallbackColor)
    end
end

local function queue_streaming_spine_effect(kind, x, y, options)
    local def = effectAssets[kind]
    if def == nil then
        return
    end

    options = options or {}
    local targetExtent = options.targetExtent or def.targetExtent or 96.0
    local z = options.z or spineEffectZ
    local zone = get_level_zone_for_bounds(
        x - targetExtent * 0.5,
        x + targetExtent * 0.5,
        y - targetExtent * 0.5,
        y + targetExtent * 0.5,
        z - 12.0,
        z + 12.0)

    table.insert(zone.effectDescs, {
        kind = kind,
        x = x,
        y = y,
        options = {
            z = z,
            facing = options.facing,
            targetExtent = targetExtent,
            duration = options.duration,
            phase = options.phase,
            loop = options.loop,
        },
    })
end

local function add_goal_flag_part(entity, offsetX, offsetY, offsetZ, wave, scale)
    if entity ~= nil then
        table.insert(goalFlagParts, {
            entity = entity,
            offsetX = offsetX or 0.0,
            offsetY = offsetY or 0.0,
            offsetZ = offsetZ or 0.0,
            wave = wave or 0.0,
            scale = scale,
        })
    end
    return entity
end

local function spawn_goal_flag()
    goalFlagParts = {}

    local z = characterZ + 12.0
    local poleX = goalX
    local poleBaseY = 18.0
    local poleHeight = 270.0
    local poleCenterY = poleBaseY + poleHeight * 0.5

    add_goal_flag_part(
        spawn_box("PlatformerGoalFlagBase", { poleX, poleBaseY + 8.0, z }, { 92.0, 16.0, 86.0 }, { 0.62, 0.55, 0.42 }, 0.72, true),
        0.0,
        poleBaseY + 8.0,
        0.0,
        0.0,
        { 92.0, 16.0, 86.0 })

    add_goal_flag_part(
        spawn_box("PlatformerGoalFlagPole", { poleX, poleCenterY, z }, { 16.0, poleHeight, 16.0 }, { 0.88, 0.86, 0.76 }, 0.46, true),
        0.0,
        poleCenterY,
        0.0,
        0.0,
        { 16.0, poleHeight, 16.0 })

    add_goal_flag_part(
        spawn_box("PlatformerGoalFlagCap", { poleX, poleBaseY + poleHeight + 12.0, z }, { 44.0, 24.0, 44.0 }, { 1.0, 0.80, 0.22 }, 0.36, true),
        0.0,
        poleBaseY + poleHeight + 12.0,
        0.0,
        0.2,
        { 44.0, 24.0, 44.0 })

    add_goal_flag_part(
        spawn_box("PlatformerGoalFlagClothA", { poleX + 76.0, poleBaseY + 224.0, z - 2.0 }, { 136.0, 74.0, 12.0 }, { 0.86, 0.12, 0.14 }, 0.68, true),
        76.0,
        poleBaseY + 224.0,
        -2.0,
        1.0,
        { 136.0, 74.0, 12.0 })

    add_goal_flag_part(
        spawn_box("PlatformerGoalFlagClothB", { poleX + 148.0, poleBaseY + 206.0, z - 2.0 }, { 92.0, 56.0, 12.0 }, { 0.96, 0.26, 0.20 }, 0.7, true),
        148.0,
        poleBaseY + 206.0,
        -2.0,
        1.7,
        { 92.0, 56.0, 12.0 })

    add_goal_flag_part(
        spawn_box("PlatformerGoalFlagStripe", { poleX + 108.0, poleBaseY + 224.0, z - 9.0 }, { 154.0, 12.0, 8.0 }, { 1.0, 0.88, 0.34 }, 0.5, true),
        108.0,
        poleBaseY + 224.0,
        -9.0,
        2.2,
        { 154.0, 12.0, 8.0 })
end

local function queue_goal_flag()
    local z = characterZ + 12.0
    local poleX = goalX
    local poleBaseY = 18.0
    local poleHeight = 270.0
    local poleCenterY = poleBaseY + poleHeight * 0.5

    queue_streaming_box("PlatformerGoalFlagBase", { poleX, poleBaseY + 8.0, z }, { 92.0, 16.0, 86.0 }, { 0.62, 0.55, 0.42 }, 0.72, true, {
        goalPart = { offsetX = 0.0, offsetY = poleBaseY + 8.0, offsetZ = 0.0, wave = 0.0, scale = { 92.0, 16.0, 86.0 } },
    })
    queue_streaming_box("PlatformerGoalFlagPole", { poleX, poleCenterY, z }, { 16.0, poleHeight, 16.0 }, { 0.88, 0.86, 0.76 }, 0.46, true, {
        goalPart = { offsetX = 0.0, offsetY = poleCenterY, offsetZ = 0.0, wave = 0.0, scale = { 16.0, poleHeight, 16.0 } },
    })
    queue_streaming_box("PlatformerGoalFlagCap", { poleX, poleBaseY + poleHeight + 12.0, z }, { 44.0, 24.0, 44.0 }, { 1.0, 0.80, 0.22 }, 0.36, true, {
        goalPart = { offsetX = 0.0, offsetY = poleBaseY + poleHeight + 12.0, offsetZ = 0.0, wave = 0.2, scale = { 44.0, 24.0, 44.0 } },
    })
    queue_streaming_box("PlatformerGoalFlagClothA", { poleX + 76.0, poleBaseY + 224.0, z - 2.0 }, { 136.0, 74.0, 12.0 }, { 0.86, 0.12, 0.14 }, 0.68, true, {
        goalPart = { offsetX = 76.0, offsetY = poleBaseY + 224.0, offsetZ = -2.0, wave = 1.0, scale = { 136.0, 74.0, 12.0 } },
    })
    queue_streaming_box("PlatformerGoalFlagClothB", { poleX + 148.0, poleBaseY + 206.0, z - 2.0 }, { 92.0, 56.0, 12.0 }, { 0.96, 0.26, 0.20 }, 0.7, true, {
        goalPart = { offsetX = 148.0, offsetY = poleBaseY + 206.0, offsetZ = -2.0, wave = 1.7, scale = { 92.0, 56.0, 12.0 } },
    })
    queue_streaming_box("PlatformerGoalFlagStripe", { poleX + 108.0, poleBaseY + 224.0, z - 9.0 }, { 154.0, 12.0, 8.0 }, { 1.0, 0.88, 0.34 }, 0.5, true, {
        goalPart = { offsetX = 108.0, offsetY = poleBaseY + 224.0, offsetZ = -9.0, wave = 2.2, scale = { 154.0, 12.0, 8.0 } },
    })
end

local function reset_player()
    player.x = startX
    player.y = 18.0
    player.vx = 0.0
    player.vy = 0.0
    player.facing = 1.0
    player.grounded = false
    player.dead = false
    player.deathReason = ""
    player.groundPlatform = nil
    player.won = false
    attackTimer = 0.0
    combat.rangedCooldownTimer = 0.0
    deathTimer = 0.0
    health = maxHealth
    hitInvulnerabilityTimer = 0.0
    winPulse = 0.0
    set_respawn_point(startX, 18.0, 1.0, nil)
    gamepadJumpHeldLast = false
    gamepadAttackTriggerHeldLast = false
    if combat.clear_projectiles ~= nil then
        combat.clear_projectiles()
    end
end

local function respawn_player()
    player.x = respawnX
    player.y = respawnY
    player.vx = 0.0
    player.vy = 0.0
    player.facing = respawnFacing
    player.grounded = false
    player.dead = false
    player.deathReason = ""
    player.groundPlatform = nil
    player.won = false
    deathTimer = 0.0
    attackTimer = 0.0
    combat.rangedCooldownTimer = 0.0
    health = maxHealth
    hitInvulnerabilityTimer = 0.0
    gamepadJumpHeldLast = false
    gamepadAttackTriggerHeldLast = false
    if combat.clear_projectiles ~= nil then
        combat.clear_projectiles()
    end
    spawn_spine_effect("spawn", player.x, player.y + playerHeight * 0.50, {
        z = spineEffectZ,
        facing = player.facing,
        targetExtent = 132.0,
        duration = 0.46,
    })
    lastFrame = -1
    lastAnimation = ""
    update_character(true)
end

local function enter_menu(reason)
    gameState = "menu"
    menuReason = reason or "title"
    player.dead = false
    player.deathReason = ""
    player.vx = 0.0
    player.vy = 0.0
    player.grounded = false
    player.groundPlatform = nil
    attackTimer = 0.0
    combat.rangedCooldownTimer = 0.0
    deathTimer = 0.0
    hitInvulnerabilityTimer = 0.0
    if destroy_level_entities ~= nil then
        destroy_level_entities()
    end
end

local function kill_player(reason)
    if gameState ~= "playing" or player.dead or player.won or gameCompleted then
        return
    end

    reason = reason or "fall"
    lives = math.max(0, lives - 1)
    health = 0
    player.dead = true
    player.deathReason = reason
    player.grounded = false
    player.groundPlatform = nil
    player.vx = 0.0
    player.vy = 0.0
    attackTimer = 0.0
    combat.rangedCooldownTimer = 0.0
    if combat.clear_projectiles ~= nil then
        combat.clear_projectiles()
    end
    deathTimer = math.max(0.1, controls.fallRespawnDelay or 0.85)
    deathCount = deathCount + 1
    if reason == "fall" then
        fallDeathCount = fallDeathCount + 1
    end

    local effectY = math.max(player.y + playerHeight * 0.35, worldMinY + 48.0)
    spawn_spine_effect("explosion", player.x, effectY, {
        z = spineEffectZ,
        facing = player.facing,
        targetExtent = 158.0,
        duration = 0.48,
    })
end

local function update_death(dt)
    deathTimer = math.max(0.0, deathTimer - dt)
    update_spine_effects(dt)
    update_character(false)
    update_camera()

    local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
    if deathTimer <= 0.0 or corona.was_key_pressed("R") or (gamepad.connected and (gamepad.start_pressed or gamepad.back_pressed)) then
        if lives <= 0 then
            enter_menu("game_over")
            update_camera()
            update_level_streaming(true)
        else
            respawn_player()
            update_camera()
            update_level_streaming(true)
        end
    end
end

local function get_spine_scene(assetPath, animation, sampleTime, fps, fallbackHandle, forceSample)
    local sampleRate = fps or playerSampleFps
    local frame = math.floor(sampleTime * sampleRate + 0.0001)
    local key = assetPath .. "|" .. animation .. "@" .. tostring(sampleRate) .. ":" .. tostring(frame)
    local handle = sceneCache[key]
    if handle ~= nil then
        return handle, frame, true
    end
    if failedSceneCache[key] then
        return fallbackHandle, frame, false
    end

    local maxSceneSamples = math.max(1, math.floor(controls.maxSceneSamplesPerFrame or 1))
    if sceneSamplesThisFrame >= maxSceneSamples then
        return fallbackHandle, frame, false
    end

    if native_spine_create_scene == nil then
        failedSceneCache[key] = true
        return fallbackHandle, frame, false
    end

    local ok, result = pcall(native_spine_create_scene, assetPath, animation, frame / sampleRate, 1.0)
    if not ok then
        failedSceneCache[key] = true
        if fallbackHandle ~= nil then
            return fallbackHandle, frame, false
        end
        corona.log("platformer spine sample failed: " .. tostring(result))
        return nil, frame, false
    end
    if result == nil then
        failedSceneCache[key] = true
        if fallbackHandle ~= nil then
            return fallbackHandle, frame, false
        end
        corona.log("platformer spine sample returned nil for " .. tostring(assetPath))
        return nil, frame, false
    end

    sceneSamplesThisFrame = sceneSamplesThisFrame + 1
    sceneCache[key] = result
    return result, frame, true
end

local function acquire_effect_entity(kind)
    local pool = effectEntityPools[kind]
    if pool ~= nil and #pool > 0 then
        local entity = pool[#pool]
        pool[#pool] = nil
        return entity
    end

    return corona.Entity.create("PlatformerSpineEffect_" .. tostring(kind))
end

local function release_effect_entity(effect)
    if effect == nil or effect.entity == nil then
        return
    end

    local entity = effect.entity
    effect.entity = nil
    set_effect_entity_visible(entity, false)

    if effectPoolLimitPerKind <= 0 then
        corona.Entity.destroy(entity)
        return
    end

    local kind = effect.kind or "unknown"
    local pool = effectEntityPools[kind]
    if pool == nil then
        pool = {}
        effectEntityPools[kind] = pool
    end

    if #pool < effectPoolLimitPerKind then
        table.insert(pool, entity)
    else
        corona.Entity.destroy(entity)
    end
end

local function discard_effect_entity(effect)
    if effect ~= nil and effect.entity ~= nil then
        corona.Entity.destroy(effect.entity)
        effect.entity = nil
    end
end

destroy_effect = function(effect)
    release_effect_entity(effect)
end

local function destroy_cached_effect_entities()
    for _, pool in pairs(effectEntityPools) do
        for _, entity in ipairs(pool) do
            corona.Entity.destroy(entity)
        end
    end
    effectEntityPools = {}
    effectPoolPrewarmed = false
end

local function clear_spine_effects()
    for _, effect in ipairs(effects) do
        destroy_effect(effect)
    end
    for _, effect in ipairs(loopEffects) do
        destroy_effect(effect)
    end
    effects = {}
    loopEffects = {}
end

local function apply_effect_pose(effect, force)
    local def = effect.def
    if def == nil then
        return false
    end

    local sampleTime = effect.elapsed + (effect.phase or 0.0)
    if effect.loop then
        sampleTime = sampleTime % def.duration
    else
        sampleTime = math.min(sampleTime, def.duration)
    end

    local sampleRate = def.fps or playerSampleFps
    local frame = math.floor(sampleTime * sampleRate + 0.0001)
    local mirrorX = (effect.facing or 1.0) < 0.0
    local yaw = 0.0
    local z = effect.z or spineEffectZ
    local targetExtent = effect.targetExtent or def.targetExtent
    if effect.entity ~= nil and not force and effect.lastHandle ~= nil and frame == effect.lastFrame then
        local ok = set_spine_pose(
            effect.entity,
            nil,
            effect.x,
            effect.y,
            z,
            yaw,
            targetExtent,
            0.72,
            0.0,
            mirrorX)
        if not ok then
            discard_effect_entity(effect)
            return false
        end
        return true
    end

    local handle, resolvedFrame, resolved = get_spine_scene(def.path, def.animation or "idle", sampleTime, sampleRate, effect.lastHandle, force)
    if handle == nil then
        return false
    end
    frame = resolvedFrame

    local useSceneHandle = force or effect.entity == nil or (resolved and frame ~= effect.lastFrame)
    if effect.entity == nil then
        effect.entity = acquire_effect_entity(effect.kind)
    end

    local ok = set_spine_pose(
        effect.entity,
        useSceneHandle and handle or nil,
        effect.x,
        effect.y,
        z,
        yaw,
        targetExtent,
        0.72,
        0.0,
        mirrorX)
    if not ok then
        discard_effect_entity(effect)
        return false
    end

    if resolved then
        effect.lastFrame = frame
        effect.lastHandle = handle
    end
    return true
end

function spawn_spine_effect(kind, x, y, options)
    local def = effectAssets[kind]
    if def == nil then
        return nil
    end

    options = options or {}
    local effect = {
        kind = kind,
        def = def,
        x = x,
        y = y,
        z = options.z or spineEffectZ,
        facing = options.facing or 1.0,
        targetExtent = options.targetExtent or def.targetExtent,
        duration = options.duration or def.duration,
        phase = options.phase or 0.0,
        elapsed = 0.0,
        entity = nil,
        lastFrame = -1,
        lastHandle = nil,
        loop = options.loop == true,
    }

    if not apply_effect_pose(effect, true) then
        discard_effect_entity(effect)
        return nil
    end

    if effect.loop then
        table.insert(loopEffects, effect)
    else
        table.insert(effects, effect)
    end
    return effect
end

function update_spine_effects(dt)
    for index = #effects, 1, -1 do
        local effect = effects[index]
        effect.elapsed = effect.elapsed + dt
        if effect.elapsed >= effect.duration then
            destroy_effect(effect)
            table.remove(effects, index)
        else
            apply_effect_pose(effect, false)
        end
    end

    for _, effect in ipairs(loopEffects) do
        effect.elapsed = effect.elapsed + dt
        apply_effect_pose(effect, false)
    end
end

local function prewarm_effect_cache()
    if effectPoolPrewarmed or effectPrewarmPerKind <= 0 or effectPoolLimitPerKind <= 0 then
        return
    end
    effectPoolPrewarmed = true

    local warmed = 0
    local savedSceneSamplesThisFrame = sceneSamplesThisFrame
    for kind, def in pairs(effectAssets) do
        sceneSamplesThisFrame = 0
        local sampleRate = def.fps or playerSampleFps
        local handle = select(1, get_spine_scene(def.path, def.animation or "idle", 0.0, sampleRate, nil, true))
        if handle ~= nil then
            for _ = 1, math.min(effectPrewarmPerKind, effectPoolLimitPerKind) do
                local entity = corona.Entity.create("PlatformerSpineEffect_" .. tostring(kind) .. "_Cached")
                local ok = set_spine_pose(
                    entity,
                    handle,
                    0.0,
                    worldMinY - 1200.0,
                    spineEffectZ,
                    0.0,
                    def.targetExtent or 96.0,
                    0.72,
                    0.0,
                    false)
                if ok then
                    set_effect_entity_visible(entity, false)
                    local pool = effectEntityPools[kind]
                    if pool == nil then
                        pool = {}
                        effectEntityPools[kind] = pool
                    end
                    table.insert(pool, entity)
                    warmed = warmed + 1
                else
                    corona.Entity.destroy(entity)
                end
            end
        end
    end
    sceneSamplesThisFrame = savedSceneSamplesThisFrame

    if warmed > 0 then
        corona.log("platformer effect entity cache prewarmed=" .. tostring(warmed))
    end
end

local function remove_effect_from_list(list, effect)
    for index = #list, 1, -1 do
        if list[index] == effect then
            table.remove(list, index)
        end
    end
end

local function destroy_streamed_effect(effect)
    if effect == nil then
        return
    end
    remove_effect_from_list(effects, effect)
    remove_effect_from_list(loopEffects, effect)
    destroy_effect(effect)
end

local function destroy_projectile_effect(projectile)
    if projectile ~= nil and projectile.effect ~= nil then
        destroy_streamed_effect(projectile.effect)
        projectile.effect = nil
    end
end

combat.clear_projectiles = function()
    for _, projectile in ipairs(combat.projectiles) do
        destroy_projectile_effect(projectile)
    end
    combat.projectiles = {}
end

local function remove_projectile_at(index)
    local projectile = combat.projectiles[index]
    if projectile ~= nil then
        destroy_projectile_effect(projectile)
    end
    table.remove(combat.projectiles, index)
end

combat.spawn_fire_projectile = function()
    if gameState ~= "playing" or player.dead or player.won or gameCompleted or combat.rangedCooldownTimer > 0.0 then
        return false
    end

    local worldDirection = screen_axis_to_world_axis(player.facing)
    if math.abs(worldDirection) < 0.001 then
        worldDirection = 1.0
    end

    local spawnX = player.x + worldDirection * combat.rangedProjectileStartOffset
    local spawnY = player.y + combat.rangedProjectileVerticalOffset
    local effect = spawn_spine_effect("fire", spawnX, spawnY, {
        z = spineEffectZ,
        facing = player.facing,
        targetExtent = combat.rangedProjectileExtent,
        duration = combat.rangedProjectileLifetime,
        loop = true,
    })
    if effect == nil then
        return false
    end

    combat.projectileCounter = combat.projectileCounter + 1
    table.insert(combat.projectiles, {
        id = combat.projectileCounter,
        x = spawnX,
        y = spawnY,
        vx = worldDirection * combat.rangedProjectileSpeed,
        vy = 0.0,
        radius = combat.rangedProjectileRadius,
        lifetime = combat.rangedProjectileLifetime,
        elapsed = 0.0,
        effect = effect,
        facing = player.facing,
    })

    combat.rangedCooldownTimer = combat.rangedCooldownSeconds
    attackTimer = math.max(attackTimer, loopSeconds.attack * 0.72)
    return true
end

local function remove_zone_goal_parts(zone)
    for index = #goalFlagParts, 1, -1 do
        if goalFlagParts[index].zone == zone then
            table.remove(goalFlagParts, index)
        end
    end
end

local function begin_level_zone_load(zone)
    if zone == nil or zone.loaded or zone.loading then
        return false
    end

    zone.entities = {}
    zone.platforms = {}
    zone.effects = {}
    zone.loadBoxIndex = 1
    zone.loadPlatformIndex = 1
    zone.loadEffectIndex = 1
    zone.loading = true
    return true
end

local function process_level_zone_load(zone, budget)
    if zone == nil or zone.loaded then
        return false, 0
    end
    if not zone.loading then
        begin_level_zone_load(zone)
    end

    local remaining = math.max(1, math.floor(budget or 1))
    local used = 0

    while remaining > 0 and zone.loadBoxIndex <= #zone.boxes do
        local desc = zone.boxes[zone.loadBoxIndex]
        zone.loadBoxIndex = zone.loadBoxIndex + 1
        remaining = remaining - 1
        used = used + 1

        local entity = spawn_box(
            desc.name,
            desc.position,
            desc.scale,
            desc.color,
            desc.roughness,
            desc.rayTracing,
            zone.entities)
        if entity ~= nil then
            if desc.goalTrigger then
                goalEntity = entity
            end
            if desc.goalPart ~= nil then
                local part = {
                    entity = entity,
                    offsetX = desc.goalPart.offsetX or 0.0,
                    offsetY = desc.goalPart.offsetY or 0.0,
                    offsetZ = desc.goalPart.offsetZ or 0.0,
                    wave = desc.goalPart.wave or 0.0,
                    scale = desc.goalPart.scale,
                    zone = zone,
                }
                table.insert(goalFlagParts, part)
            end
        end
    end

    while remaining > 0 and zone.loadPlatformIndex <= #zone.platformDescs do
        local desc = zone.platformDescs[zone.loadPlatformIndex]
        zone.loadPlatformIndex = zone.loadPlatformIndex + 1
        remaining = remaining - 1
        used = used + 1
        spawn_platform(desc.x, desc.y, desc.w, desc.h, desc.depth, desc.color, zone.entities, zone)
    end

    while remaining > 0 and zone.loadEffectIndex <= #zone.effectDescs do
        local desc = zone.effectDescs[zone.loadEffectIndex]
        zone.loadEffectIndex = zone.loadEffectIndex + 1
        remaining = remaining - 1
        used = used + 1
        local effect = spawn_spine_effect(desc.kind, desc.x, desc.y, desc.options)
        if effect ~= nil then
            effect.zone = zone
            table.insert(zone.effects, effect)
        end
    end

    if zone.loadBoxIndex > #zone.boxes and
        zone.loadPlatformIndex > #zone.platformDescs and
        zone.loadEffectIndex > #zone.effectDescs then
        zone.loading = false
        zone.loaded = true
        loadedLevelZoneCount = loadedLevelZoneCount + 1
        corona.log(
            "platformer zone loaded index=" .. tostring(zone.index) ..
            " boxes=" .. tostring(#zone.boxes) ..
            " platforms=" .. tostring(#zone.platformDescs) ..
            " effects=" .. tostring(#zone.effectDescs) ..
            " loaded=" .. tostring(loadedLevelZoneCount) .. "/" .. tostring(#levelZones))
        return true, used
    end

    return false, used
end

local function load_level_zone(zone, budget)
    if zone == nil or zone.loaded then
        return false
    end
    local loadBudget = budget or 1000000
    local completed = false
    while not completed and loadBudget > 0 do
        local used = 0
        completed, used = process_level_zone_load(zone, loadBudget)
        if used <= 0 then
            break
        end
        loadBudget = loadBudget - used
    end
    return completed
end

local function unload_level_zone(zone)
    if zone == nil or (not zone.loaded and not zone.loading) then
        return false
    end

    for _, effect in ipairs(zone.effects) do
        destroy_streamed_effect(effect)
    end
    zone.effects = {}

    for index = #platformData, 1, -1 do
        if platformData[index].zone == zone then
            table.remove(platformData, index)
        end
    end
    zone.platforms = {}

    remove_zone_goal_parts(zone)
    for _, entity in ipairs(zone.entities) do
        if entity == goalEntity then
            goalEntity = nil
        end
        corona.Entity.destroy(entity)
    end
    zone.entities = {}

    if zone.loaded then
        loadedLevelZoneCount = math.max(0, loadedLevelZoneCount - 1)
    end
    zone.loading = false
    zone.loaded = false
    zone.loadBoxIndex = 1
    zone.loadPlatformIndex = 1
    zone.loadEffectIndex = 1
    return true
end

local function zone_overlaps_frustum(zone, minX, maxX, minY, maxY, margin)
    return
        zone.maxX >= minX - margin and
        zone.minX <= maxX + margin and
        zone.maxY >= minY - margin and
        zone.minY <= maxY + margin
end

local function spawn_goal_spine_effects()
    queue_streaming_spine_effect("movingflag", goalX + 118.0, 236.0, {
        z = spineEffectZ,
        targetExtent = 138.0,
        loop = true,
        phase = 0.25,
    })
    queue_streaming_spine_effect("skill_ready", goalX, 72.0, {
        z = spineEffectZ,
        targetExtent = 190.0,
        loop = true,
        phase = 0.55,
    })
end

local function spawn_layout_spine_effects()
    if currentLayout == nil or currentLayout.obstacles == nil then
        return
    end

    local markerCount = 0
    for index, block in ipairs(currentLayout.obstacles) do
        if index % 6 == 0 and markerCount < 3 then
            markerCount = markerCount + 1
            queue_streaming_spine_effect("skill_ready", block.x, block.y + block.h * 0.5 + 44.0, {
                z = spineEffectZ,
                targetExtent = 92.0,
                loop = true,
                phase = index * 0.13,
            })
        end
    end
end

local function choose_animation()
    if attackTimer > 0.0 then
        return "attack"
    end
    if math.abs(player.vx) > 24.0 and player.grounded then
        return "walk"
    end
    return "idle"
end

local function player_spine_y()
    return player.y + playerVisualFootOffset
end

function update_character(force)
    if character == nil then
        return
    end

    local animation = choose_animation()
    local duration = loopSeconds[animation] or 1.0
    local sampleTime = time % duration
    local sampleFps = get_player_animation_sample_fps(animation)
    local sampleFrame = math.floor(sampleTime * sampleFps + 0.0001)
    local forceSample = force or lastCharacterHandle == nil or animation ~= lastAnimation
    local mirrorX = player.facing < 0.0
    local yaw = 0.0
    local visualY = player_spine_y()
    if not forceSample and sampleFrame == lastFrame then
        set_character_spine_pose(character, nil, player.x, visualY, characterZ, yaw, playerVisualExtent, 0.82, 0.0, mirrorX)
        return
    end

    local handle, frame, resolved = get_spine_scene(spineAsset, animation, sampleTime, sampleFps, lastCharacterHandle, forceSample)
    if handle == nil then
        return
    end

    if force or (resolved and (frame ~= lastFrame or animation ~= lastAnimation)) then
        set_character_spine_pose(character, handle, player.x, visualY, characterZ, yaw, playerVisualExtent, 0.82, 0.0, mirrorX)
        if resolved then
            lastFrame = frame
            lastAnimation = animation
            lastCharacterHandle = handle
        end
    else
        set_character_spine_pose(character, nil, player.x, visualY, characterZ, yaw, playerVisualExtent, 0.82, 0.0, mirrorX)
    end
end

local function build_level()
    reset_level_streaming()
    local backdropWidth = 900.0
    local backdropX = firstPlayableX + backdropWidth * 0.5
    local backdropIndex = 0
    while backdropX < levelEndX + backdropWidth do
        local shade = 0.12 + ((backdropIndex + currentLevel) % 4) * 0.018
        queue_streaming_box(
            "PlatformerBackdrop",
            { backdropX, 145.0, 150.0 },
            { backdropWidth + 24.0, 390.0, 20.0 },
            { shade, shade + 0.02, shade + 0.05 },
            0.92,
            false)

        if backdropIndex % 2 == 0 then
            queue_streaming_box(
                "PlatformerFarBlock",
                { backdropX - 190.0, 92.0 + ((backdropIndex + currentLevel) % 4) * 18.0, 190.0 },
                { 120.0 + ((backdropIndex + currentLevel) % 5) * 24.0, 160.0 + (backdropIndex % 3) * 40.0, 34.0 },
                { 0.18, 0.22, 0.26 },
                0.9,
                false)
        end

        backdropX = backdropX + backdropWidth
        backdropIndex = backdropIndex + 1
    end

    if currentLayout ~= nil then
        queue_layout_blocks(currentLayout.floors, { 0.34, 0.42, 0.36 })
        queue_layout_blocks(currentLayout.platforms, { 0.35, 0.43, 0.45 })
        queue_layout_blocks(currentLayout.obstacles, { 0.42, 0.35, 0.28 })
    else
        local floorWidth = 780.0 + (currentLevel % 4) * 34.0
        local floorGap = 52.0 + (currentLevel % 5) * 9.0
        local floorX = firstPlayableX + floorWidth * 0.5
        local floorIndex = 0
        while floorX < levelEndX + floorWidth do
            local tint = 0.31 + (floorIndex % 4) * 0.025
            queue_streaming_platform(
                floorX,
                0.0,
                floorWidth + 18.0,
                36.0,
                platformDepth,
                { tint, tint + 0.08, tint + 0.04 })
            floorX = floorX + floorWidth + floorGap
            floorIndex = floorIndex + 1
        end

        local raisedCount = math.max(8, math.floor((levelEndX - firstPlayableX) / 620.0))
        for i = 1, raisedCount do
            local span = math.max(1.0, levelEndX - firstPlayableX - 900.0)
            local x = firstPlayableX + 480.0 + (span * i / (raisedCount + 1))
            local y = 76.0 + ((i + currentLevel) % 4) * 31.0
            queue_streaming_platform(
                x,
                y,
                220.0 + ((i + currentLevel) % 3) * 42.0,
                30.0,
                platformDepth,
                { 0.35, 0.43, 0.45 })
        end
    end

    queue_streaming_box("PlatformerStartWall", { firstPlayableX + 50.0, 96.0, platformZ }, { 38.0, 190.0, platformDepth }, { 0.26, 0.32, 0.29 }, 0.84, true)
    queue_streaming_box("PlatformerStep", { startX + 410.0, 56.0, platformZ }, { 92.0, 74.0, platformDepth }, { 0.29, 0.37, 0.39 }, 0.86, true)
    queue_streaming_box("PlatformerGoalTrigger", { goalX, 28.0, characterZ + 12.0 }, { 170.0, 20.0, 92.0 }, { 0.92, 0.72, 0.20 }, 0.52, true, {
        goalTrigger = true,
    })
    queue_goal_flag()
    spawn_goal_spine_effects()
    spawn_layout_spine_effects()
    corona.log(
        "platformer streaming zones=" .. tostring(#levelZones) ..
        " zone_width=" .. tostring(levelZoneWidth) ..
        " streaming=" .. tostring(levelStreamingEnabled))
    if not levelStreamingEnabled then
        for _, zone in pairs(levelZones) do
            load_level_zone(zone)
        end
    end
end

local function spawn_character()
    local handle = select(1, get_spine_scene(spineAsset, "idle", 0.0, get_player_animation_sample_fps("idle"), nil, true))
    if handle == nil then
        return
    end
    lastCharacterHandle = handle
    set_character_scale_reference(handle)

    character = corona.Entity.create("Platformer" .. playerDisplayName)
    local ok = set_character_spine_pose(character, handle, player.x, player_spine_y(), characterZ, 0.0, playerVisualExtent, 0.82, 0.0, false)
    if not ok then
        corona.Entity.destroy(character)
        character = nil
        return
    end
    add_spawned(character)
end

local function spawn_camera_and_light()
    cameraEntity = add_spawned(corona.Entity.create("PlatformerCamera"))
    corona.CameraComponent.add(cameraEntity, {
        position = { player.x - 120.0, player.y + 235.0, -700.0 },
        look_at = { player.x - 70.0, player.y + 92.0, characterZ },
        up = { 0.0, 1.0, 0.0 },
        fov = 0.72,
        near_plane = 5.0,
        far_plane = 12000.0,
        active = true,
    })

    sunEntity = add_spawned(corona.Entity.create("PlatformerSun"))
    corona.LightComponent.add(sunEntity, {
        type = "directional",
        enabled = true,
        direction = { 0.32, 0.78, 0.54 },
        color = { 1.0, 0.94, 0.82 },
        intensity = 0.95,
        persistent = false,
    })
end

local function enemy_target_extent(name)
    return sharedCharacterVisualExtent
end

local function resolve_enemy_asset(index)
    local asset = enemyAssets[index]
    if asset == nil or asset.name ~= playerAssetName then
        return asset
    end

    for offset = 1, totalEnemyAssets do
        local forward = enemyAssets[index + offset]
        if forward ~= nil and forward.name ~= playerAssetName then
            return forward
        end
        local backward = enemyAssets[index - offset]
        if backward ~= nil and backward.name ~= playerAssetName then
            return backward
        end
    end
    return nil
end

local function build_enemy_slots()
    enemies = {}
    for index = currentLevelEnemyStart, currentLevelEnemyEnd do
        local asset = resolve_enemy_asset(index)
        if asset == nil then
            break
        end
        local slotIndex = index - currentLevelEnemyStart + 1
        local x = enemyStartX + (slotIndex - 1) * enemySpacing
        local direction = ((index + currentLevel) % 2 == 0) and -1.0 or 1.0
        local extent = enemy_target_extent(asset.name)
        enemies[slotIndex] = {
            asset = asset,
            assetIndex = index,
            baseX = x,
            x = x,
            y = 18.0,
            vx = direction * (38.0 + ((index + currentLevel) % 6) * 7.0),
            facing = direction,
            patrolLeft = x - 58.0 - (index % 3) * 18.0,
            patrolRight = x + 58.0 + (index % 4) * 16.0,
            phase = (index % 23) * 0.071,
            targetExtent = extent,
            hitRadius = math.max(34.0, extent * 0.34),
            hitHeight = math.max(72.0, extent * 0.84),
            alive = true,
            entity = nil,
            lastFrame = -1,
            lastAnimation = "",
            lastHandle = nil,
        }
    end
end

local function destroy_enemy_entity(enemy)
    if enemy.entity ~= nil then
        corona.Entity.destroy(enemy.entity)
        enemy.entity = nil
        enemy.lastFrame = -1
        enemy.lastAnimation = ""
        enemy.lastHandle = nil
    end
end

local function reset_all_enemies()
    defeatedEnemyCount = 0
    activeEnemyCount = 0
    for index, enemy in ipairs(enemies) do
        destroy_enemy_entity(enemy)
        enemy.x = enemy.baseX
        local assetIndex = enemy.assetIndex or index
        enemy.vx = (((assetIndex + currentLevel) % 2 == 0) and -1.0 or 1.0) * (38.0 + ((assetIndex + currentLevel) % 6) * 7.0)
        enemy.facing = enemy.vx >= 0.0 and 1.0 or -1.0
        enemy.alive = true
    end
end

local function sync_enemy_source_extent(enemy, handle)
    local extent = character_visual_extent_from_scene(handle)
    enemy.targetExtent = extent
    enemy.hitRadius = math.max(34.0, extent * 0.34)
    enemy.hitHeight = math.max(72.0, extent * 0.84)
end

local function spawn_enemy_entity(enemy)
    local handle = select(1, get_spine_scene(enemy.asset.path, "idle", enemy.phase, enemySampleFps, nil, true))
    if handle == nil then
        return false
    end
    sync_enemy_source_extent(enemy, handle)

    enemy.entity = corona.Entity.create("PlatformerEnemy")
    local ok = set_character_spine_pose(
        enemy.entity,
        handle,
        enemy.x,
        enemy.y + enemy.hitHeight * 0.48,
        enemyZ,
        0.0,
        enemy.targetExtent,
        0.84,
        0.0,
        enemy_mirror_x(enemy))
    if not ok then
        corona.Entity.destroy(enemy.entity)
        enemy.entity = nil
        enemy.alive = false
        return false
    else
        enemy.lastHandle = handle
        spawn_spine_effect("spawn", enemy.x, enemy.y + enemy.hitHeight * 0.48, {
            z = spineEffectZ,
            targetExtent = math.max(98.0, enemy.targetExtent * 1.05),
            phase = enemy.phase,
        })
    end
    return true
end

local function defeat_enemy(enemy, cause)
    if not enemy.alive then
        return
    end
    local effectKind = cause == "stomp" and "explosion" or "hit"
    spawn_spine_effect(effectKind, enemy.x, enemy.y + enemy.hitHeight * 0.52, {
        z = spineEffectZ,
        facing = player.facing,
        targetExtent = cause == "stomp" and math.max(138.0, enemy.targetExtent * 1.22) or math.max(88.0, enemy.targetExtent * 0.92),
    })
    destroy_enemy_entity(enemy)
    enemy.alive = false
    defeatedEnemyCount = defeatedEnemyCount + 1
end

local function is_melee_attack_active()
    return attackTimer > math.max(0.0, loopSeconds.attack - meleeAttackWindow)
end

local function enemy_center_y(enemy)
    return enemy.y + enemy.hitHeight * 0.48
end

local function projectile_hits_enemy(projectile, enemy)
    if projectile == nil or enemy == nil or not enemy.alive or enemy.entity == nil then
        return false
    end

    local dx = projectile.x - enemy.x
    local dy = projectile.y - enemy_center_y(enemy)
    local horizontalReach = projectile.radius + enemy.hitRadius
    local verticalReach = projectile.radius + enemy.hitHeight * 0.38
    return math.abs(dx) < horizontalReach and math.abs(dy) < verticalReach
end

combat.update_projectiles = function(dt)
    for index = #combat.projectiles, 1, -1 do
        local projectile = combat.projectiles[index]
        projectile.elapsed = projectile.elapsed + dt
        projectile.x = projectile.x + projectile.vx * dt
        projectile.y = projectile.y + projectile.vy * dt

        if projectile.effect ~= nil then
            projectile.effect.x = projectile.x
            projectile.effect.y = projectile.y
            projectile.effect.facing = projectile.facing
        end

        local removeProjectile = projectile.elapsed >= projectile.lifetime
        if not removeProjectile then
            for _, enemy in ipairs(enemies) do
                if projectile_hits_enemy(projectile, enemy) then
                    defeat_enemy(enemy, "fire")
                    removeProjectile = true
                    break
                end
            end
        end

        if removeProjectile then
            remove_projectile_at(index)
        end
    end
end

local function enemy_stomp_y(enemy)
    return enemy.y + enemy.hitHeight * 0.78
end

local function is_enemy_in_melee_arc(enemy)
    local forwardDistance = (enemy.x - player.x) * screen_axis_to_world_axis(player.facing)
    if forwardDistance < -meleeAttackBackReach then
        return false
    end
    if forwardDistance > meleeAttackRange + enemy.hitRadius then
        return false
    end

    local playerCenterY = player.y + playerHeight * 0.48
    local verticalDistance = math.abs(playerCenterY - enemy_center_y(enemy))
    return verticalDistance < meleeAttackVerticalReach + enemy.hitHeight * 0.25
end

local function can_stomp_enemy(enemy, previousY, fallingVelocity)
    if fallingVelocity > 0.0 then
        return false
    end

    local enemyTop = enemy_stomp_y(enemy)
    local crossedEnemyTop =
        previousY >= enemyTop - stompForgiveness and
        player.y <= enemyTop + stompForgiveness
    if not crossedEnemyTop then
        return false
    end

    local horizontalReach = enemy.hitRadius + playerRadius * 0.55
    return math.abs(player.x - enemy.x) < horizontalReach
end

local function spawn_player_hit_effect(enemy)
    local facing = player.facing
    if enemy ~= nil and enemy.x ~= nil and math.abs(enemy.x - player.x) > 0.001 then
        facing = enemy.x > player.x and 1.0 or -1.0
    end

    spawn_spine_effect("hit", player.x, player.y + playerHeight * 0.52, {
        z = spineEffectZ,
        facing = facing,
        targetExtent = 128.0,
        duration = 0.32,
    })
end

local function damage_player(enemy, amount)
    if gameState ~= "playing" or player.dead or player.won or gameCompleted then
        return "none"
    end
    if hitInvulnerabilityTimer > 0.0 then
        return "blocked"
    end

    local damage = math.max(1, math.floor(amount or enemyDamage))
    health = math.max(0, health - damage)
    hitInvulnerabilityTimer = math.max(0.05, controls.hitInvulnerabilitySeconds or 0.65)
    spawn_player_hit_effect(enemy)

    if enemy ~= nil and enemy.x ~= nil then
        local knockback = enemy.x >= player.x and -1.0 or 1.0
        player.vx = knockback * math.max(180.0, moveSpeed * 0.58)
        player.vy = math.max(player.vy, 215.0)
        player.facing = -knockback
        player.grounded = false
    end

    if health <= 0 then
        kill_player("health")
        return "death"
    end
    return "hit"
end

local function update_enemy_animation(enemy, force)
    if enemy.entity == nil then
        return
    end

    local animation = "walk"
    local duration = loopSeconds.walk
    local sampleTime = (time + enemy.phase) % duration
    local sampleFrame = math.floor(sampleTime * enemySampleFps + 0.0001)
    local forceSample = force or enemy.lastHandle == nil or animation ~= enemy.lastAnimation
    local mirrorX = enemy_mirror_x(enemy)
    local yaw = 0.0
    if not forceSample and sampleFrame == enemy.lastFrame then
        set_character_spine_pose(enemy.entity, nil, enemy.x, enemy_center_y(enemy), enemyZ, yaw, enemy.targetExtent, 0.84, 0.0, mirrorX)
        return
    end

    local handle, frame, resolved = get_spine_scene(enemy.asset.path, animation, sampleTime, enemySampleFps, enemy.lastHandle, forceSample)
    if handle == nil then
        return
    end

    local sceneHandle = nil
    if force or (resolved and (frame ~= enemy.lastFrame or animation ~= enemy.lastAnimation)) then
        sceneHandle = handle
        if resolved then
            enemy.lastFrame = frame
            enemy.lastAnimation = animation
            enemy.lastHandle = handle
        end
    end
    set_character_spine_pose(enemy.entity, sceneHandle, enemy.x, enemy_center_y(enemy), enemyZ, yaw, enemy.targetExtent, 0.84, 0.0, mirrorX)
end

local function update_enemy_contact(enemy, previousY, fallingVelocity)
    if not enemy.alive then
        return "none"
    end

    if can_stomp_enemy(enemy, previousY, fallingVelocity) then
        local bounceY = enemy_stomp_y(enemy) + 2.0
        defeat_enemy(enemy, "stomp")
        player.y = math.max(player.y, bounceY)
        player.vy = stompBounceVelocity
        player.grounded = false
        return "stomp"
    end

    if is_melee_attack_active() and is_enemy_in_melee_arc(enemy) then
        defeat_enemy(enemy, "melee")
        return "melee"
    end

    local dx = player.x - enemy.x
    local dy = (player.y + playerHeight * 0.48) - enemy_center_y(enemy)
    local touching = math.abs(dx) < (playerRadius + enemy.hitRadius) and math.abs(dy) < (playerHeight * 0.44 + enemy.hitHeight * 0.44)
    if not touching then
        return "none"
    end

    return damage_player(enemy, enemyDamage)
end

local function update_enemies(dt, previousY, fallingVelocity)
    activeEnemyCount = 0
    if totalEnemyAssets <= 0 then
        return
    end

    for _, enemy in ipairs(enemies) do
        if enemy.alive then
            local distance = math.abs(enemy.baseX - player.x)
            local shouldBeActive = (not controls.enemyStreaming) or distance <= enemySpawnDistance
            if shouldBeActive then
                activeEnemyCount = activeEnemyCount + 1
                if enemy.entity == nil then
                    local maxEnemySpawns = math.max(1, math.floor(controls.maxEnemySpawnsPerFrame or 1))
                    if enemySpawnsThisFrame < maxEnemySpawns then
                        enemySpawnsThisFrame = enemySpawnsThisFrame + 1
                        spawn_enemy_entity(enemy)
                    end
                end

                if enemy.entity ~= nil and enemy.alive then
                    enemy.x = enemy.x + enemy.vx * dt
                    if enemy.x < enemy.patrolLeft then
                        enemy.x = enemy.patrolLeft
                        enemy.vx = math.abs(enemy.vx)
                        enemy.facing = 1.0
                    elseif enemy.x > enemy.patrolRight then
                        enemy.x = enemy.patrolRight
                        enemy.vx = -math.abs(enemy.vx)
                        enemy.facing = -1.0
                    end

                    local contactResult = update_enemy_contact(enemy, previousY, fallingVelocity)
                    if contactResult == "death" then
                        return
                    end
                    update_enemy_animation(enemy, false)
                end
            elseif enemy.entity ~= nil and distance > enemyDespawnDistance then
                destroy_enemy_entity(enemy)
            end
        elseif enemy.entity ~= nil then
            destroy_enemy_entity(enemy)
        end
    end
end

local function update_input(dt)
    local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
    local axis = 0.0
    if corona.is_key_down("A") or corona.is_key_down("LEFT") then
        axis = axis - 1.0
    end
    if corona.is_key_down("D") or corona.is_key_down("RIGHT") then
        axis = axis + 1.0
    end
    if gamepad.connected then
        axis = axis + (gamepad.left_x or 0.0)
        if gamepad.dpad_left then
            axis = axis - 1.0
        end
        if gamepad.dpad_right then
            axis = axis + 1.0
        end
        axis = clamp(axis, -1.0, 1.0)
    end

    if axis ~= 0.0 then
        local worldAxis = screen_axis_to_world_axis(axis)
        player.vx = approach(player.vx, worldAxis * moveSpeed, acceleration * dt)
        if math.abs(axis) > 0.18 then
            player.facing = axis
        end
    else
        player.vx = approach(player.vx, 0.0, friction * dt)
    end

    local gamepadJumpHeld =
        gamepad.connected and (
            gamepad.a == true or
            gamepad.x == true or
            gamepad.dpad_up == true or
            ((gamepad.left_y or 0.0) > 0.55))
    local jumpPressed =
        corona.was_key_pressed("SPACE") or
        corona.was_key_pressed("W") or
        corona.was_key_pressed("UP") or
        (gamepad.connected and (gamepad.a_pressed or gamepad.x_pressed or gamepad.dpad_up_pressed)) or
        (gamepadJumpHeld and not gamepadJumpHeldLast)
    gamepadJumpHeldLast = gamepadJumpHeld

    if jumpPressed and player.grounded then
        spawn_spine_effect("dust", player.x, player.y + 22.0, {
            z = spineEffectZ,
            facing = player.facing,
            targetExtent = 88.0,
        })
        player.vy = jumpVelocity
        player.grounded = false
    end

    local gamepadRangedTriggerHeld = gamepad.connected and ((gamepad.right_trigger or 0.0) > 0.55)
    local meleePressed =
        corona.was_key_pressed("J") or
        (gamepad.connected and gamepad.b_pressed)
    local rangedPressed =
        corona.was_key_pressed("K") or
        corona.was_key_pressed("F") or
        (gamepad.connected and (
            gamepad.y_pressed or
            (gamepadRangedTriggerHeld and not gamepadAttackTriggerHeldLast)))
    gamepadAttackTriggerHeldLast = gamepadRangedTriggerHeld

    if meleePressed then
        attackTimer = loopSeconds.attack
    end
    if rangedPressed and combat.spawn_fire_projectile ~= nil then
        combat.spawn_fire_projectile()
    end

    if corona.was_key_pressed("R") or (gamepad.connected and (gamepad.start_pressed or gamepad.back_pressed)) then
        if gameCompleted and start_new_game ~= nil then
            start_new_game()
        elseif load_level ~= nil then
            load_level(gameCompleted and 1 or currentLevel)
        else
            reset_player()
            reset_all_enemies()
        end
    end
end

local function collide_platforms(previousY)
    player.grounded = false
    player.groundPlatform = nil
    if player.vy > 0.0 then
        return
    end

    for _, platform in ipairs(platformData) do
        local withinX =
            player.x + playerRadius > platform.left and
            player.x - playerRadius < platform.right
        if withinX and previousY >= platform.top and player.y <= platform.top then
            player.y = platform.top
            player.vy = 0.0
            player.grounded = true
            player.groundPlatform = platform
            return
        end
    end
end

local function update_goal(dt)
    if goalEntity == nil then
        return
    end
    local dx = player.x - goalX
    local dy = player.y - 112.0
    if not player.won and dx * dx + dy * dy < 95.0 * 95.0 then
        player.won = true
        player.vx = 0.0
        levelAdvanceTimer = levelAdvanceDelay
        spawn_spine_effect("lightning", goalX, 158.0, {
            z = spineEffectZ,
            targetExtent = 210.0,
        })
        spawn_spine_effect("skill_ready", goalX, 82.0, {
            z = spineEffectZ,
            targetExtent = 230.0,
            duration = 0.78,
        })
    end

    winPulse = winPulse + dt * (player.won and 5.5 or 2.0)
    local triggerPulse = math.sin(winPulse) * (player.won and 5.0 or 2.0)
    corona.MeshComponent.set(goalEntity, {
        position = { goalX, 28.0 + triggerPulse * 0.2, characterZ + 12.0 },
        scale = { 170.0 + triggerPulse * 2.0, 20.0 + math.abs(triggerPulse), 92.0 },
    })

    for _, part in ipairs(goalFlagParts) do
        if part.entity ~= nil then
            local clothWave = 0.0
            local clothLift = 0.0
            if part.wave >= 1.0 then
                clothWave = math.sin(winPulse * 1.45 + part.wave) * (player.won and 14.0 or 7.0)
                clothLift = math.sin(winPulse * 1.15 + part.wave) * (player.won and 7.0 or 3.0)
            elseif player.won and part.wave > 0.0 then
                clothLift = math.sin(winPulse + part.wave) * 4.0
            end
            corona.MeshComponent.set(part.entity, {
                position = {
                    goalX + part.offsetX + clothWave,
                    part.offsetY + clothLift,
                    characterZ + 12.0 + part.offsetZ,
                },
                scale = part.scale,
            })
        end
    end
end

local function compute_camera_focus()
    local lead = clamp(player.vx * 0.28, -85.0, 110.0)
    local focusX = clamp(player.x + lead, worldMinX + 170.0, goalX - 130.0)
    local focusY = clamp(player.y + 92.0, 80.0, 290.0)
    return focusX, focusY
end

local function camera_frustum_bounds()
    local focusX, focusY = compute_camera_focus()
    cameraFocusX = focusX
    cameraFocusY = focusY

    local cameraDepth = 660.0
    local fov = 0.72
    local aspect = 16.0 / 9.0
    local halfY = math.tan(fov * 0.5) * cameraDepth
    local halfX = halfY * aspect
    return focusX - halfX, focusX + halfX, focusY - halfY, focusY + halfY
end

update_level_streaming = function(force)
    if not levelStreamingEnabled then
        return
    end

    local minX, maxX, minY, maxY = camera_frustum_bounds()
    local unloadMargin = math.max(0.0, controls.streamingUnloadMargin or 980.0)
    for _, zone in pairs(levelZones) do
        if (zone.loaded or zone.loading) and not zone_overlaps_frustum(zone, minX, maxX, minY, maxY, unloadMargin) then
            unload_level_zone(zone)
        end
    end

    local objectBudget = force and 1000000 or math.max(1, math.floor(controls.maxLevelZoneObjectsPerFrame or 8))
    local remainingObjects = objectBudget
    local activeLoads = {}
    for _, zone in pairs(levelZones) do
        if zone.loading then
            table.insert(activeLoads, zone)
        end
    end
    table.sort(activeLoads, function(a, b)
        local ac = (a.minX + a.maxX) * 0.5
        local bc = (b.minX + b.maxX) * 0.5
        return math.abs(ac - cameraFocusX) < math.abs(bc - cameraFocusX)
    end)

    for _, zone in ipairs(activeLoads) do
        if remainingObjects <= 0 then
            break
        end
        local completed, used = process_level_zone_load(zone, remainingObjects)
        remainingObjects = math.max(0, remainingObjects - used)
        if completed then
            levelStreamingLoadFrame = levelStreamingLoadFrame + 1
        end
    end

    local preloadMargin = math.max(0.0, controls.streamingPreloadMargin or 460.0)
    local candidates = {}
    for _, zone in pairs(levelZones) do
        if not zone.loaded and not zone.loading and zone_overlaps_frustum(zone, minX, maxX, minY, maxY, preloadMargin) then
            table.insert(candidates, zone)
        end
    end
    table.sort(candidates, function(a, b)
        local ac = (a.minX + a.maxX) * 0.5
        local bc = (b.minX + b.maxX) * 0.5
        return math.abs(ac - cameraFocusX) < math.abs(bc - cameraFocusX)
    end)

    local initialZoneLoads = math.max(1, math.floor(controls.maxInitialZoneLoads or 3))
    local loadBudget = force and math.min(initialZoneLoads, #candidates) or math.max(1, math.floor(controls.maxLevelZoneLoadsPerFrame or 1))
    local startedThisFrame = 0
    for _, zone in ipairs(candidates) do
        if startedThisFrame >= loadBudget or remainingObjects <= 0 then
            break
        end
        begin_level_zone_load(zone)
        startedThisFrame = startedThisFrame + 1
        local completed, used = process_level_zone_load(zone, remainingObjects)
        remainingObjects = math.max(0, remainingObjects - used)
        if completed then
            levelStreamingLoadFrame = levelStreamingLoadFrame + 1
        end
    end
    if startedThisFrame > 0 then
        levelStreamingLoadFrame = levelStreamingLoadFrame + 1
    end
end

function update_camera()
    if cameraEntity == nil then
        return
    end
    local focusX, focusY = compute_camera_focus()
    cameraFocusX = focusX
    cameraFocusY = focusY
    corona.CameraComponent.set(cameraEntity, {
        position = { focusX - 70.0, focusY + 140.0, -720.0 },
        look_at = { focusX - 35.0, focusY + 20.0, characterZ },
        up = { 0.0, 1.0, 0.0 },
        active = true,
    })
end

destroy_level_entities = function()
    for _, zone in pairs(levelZones) do
        unload_level_zone(zone)
    end
    reset_level_streaming()
    combat.clear_projectiles()
    clear_spine_effects()
    for _, enemy in ipairs(enemies) do
        destroy_enemy_entity(enemy)
    end
    for _, entity in ipairs(spawned) do
        corona.Entity.destroy(entity)
    end

    spawned = {}
    platformData = {}
    enemies = {}
    goalFlagParts = {}
    character = nil
    cameraEntity = nil
    sunEntity = nil
    goalEntity = nil
    activeEnemyCount = 0
end

local function advance_level()
    if currentLevel >= levelCount then
        gameCompleted = true
        player.won = true
        player.vx = 0.0
        player.vy = 0.0
        levelAdvanceTimer = 0.0
        return
    end

    load_level(currentLevel + 1)
end

load_level = function(levelIndex)
    refresh_vitals_config()
    destroy_level_entities()
    configure_level(levelIndex)
    defeatedEnemyCount = 0
    if currentLevel == 1 then
        deathCount = 0
        fallDeathCount = 0
    end
    levelAdvanceTimer = 0.0
    gameCompleted = false
    lastFrame = -1
    lastAnimation = ""
    lastCharacterHandle = nil

    reset_player()
    build_level()
    build_enemy_slots()
    spawn_camera_and_light()
    spawn_character()
    update_character(true)
    update_camera()
    update_level_streaming(true)

    local layoutText = "procedural"
    if currentLayout ~= nil then
        layoutText = "json:" .. tostring(currentLayout.id or currentLevel)
        if currentLayout.analysis ~= nil then
            layoutText = layoutText ..
                " passable=" .. tostring(currentLayout.analysis.passable) ..
                " max_gap=" .. tostring(currentLayout.analysis.max_floor_gap) ..
                " max_step=" .. tostring(currentLayout.analysis.max_platform_top_delta)
        end
    end

    corona.log(
        "platformer level " .. tostring(currentLevel) .. "/" .. tostring(levelCount) ..
        " loaded enemies=" .. tostring(currentLevelEnemyTotal) ..
        " asset_range=" .. tostring(currentLevelEnemyStart) .. "-" .. tostring(currentLevelEnemyEnd) ..
        " layout=" .. layoutText)
end

start_new_game = function()
    refresh_vitals_config()
    lives = maxLives
    health = maxHealth
    deathCount = 0
    fallDeathCount = 0
    hitInvulnerabilityTimer = 0.0
    menuReason = ""
    gameState = "playing"
    gameCompleted = false
    load_level(1)
end

if gameState == "menu" then
    configure_level(1)
    prewarm_effect_cache()
else
    prewarm_effect_cache()
    load_level(1)
end
corona.log(
    "startup spine " .. playerDisplayName .. " platformer script loaded with " .. tostring(totalEnemyAssets) ..
    " enemy Spine assets across " .. tostring(levelCount) ..
    " levels, jump_velocity=" .. tostring(jumpVelocity))

function script.update(dt)
    time = time + dt
    attackTimer = math.max(0.0, attackTimer - dt)
    combat.rangedCooldownTimer = math.max(0.0, combat.rangedCooldownTimer - dt)
    hitInvulnerabilityTimer = math.max(0.0, hitInvulnerabilityTimer - dt)
    sceneSamplesThisFrame = 0
    enemySpawnsThisFrame = 0

    if gameState == "menu" then
        local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
        if corona.was_key_pressed("SPACE") or corona.was_key_pressed("ENTER") or (gamepad.connected and (gamepad.start_pressed or gamepad.a_pressed)) then
            start_new_game()
            return
        end
        if corona.was_key_pressed("ESCAPE") or corona.was_key_pressed("Q") or (gamepad.connected and gamepad.back_pressed) then
            request_exit()
            return
        end

        if controls.menuPreviewAnimation == true then
            combat.update_projectiles(dt)
            update_spine_effects(dt)
            update_character(false)
            update_camera()
            update_level_streaming(false)
        end
        return
    end

    if player.dead then
        update_death(dt)
        return
    end

    update_input(dt)
    update_level_streaming(false)
    if gameCompleted then
        combat.update_projectiles(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        return
    end

    if player.won and levelAdvanceTimer > 0.0 then
        levelAdvanceTimer = math.max(0.0, levelAdvanceTimer - dt)
        update_goal(dt)
        combat.update_projectiles(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        if levelAdvanceTimer <= 0.0 then
            advance_level()
        end
        return
    end

    local wasGrounded = player.grounded
    local previousY = player.y
    player.vy = math.max(player.vy + gravity * dt, maxFallSpeed)
    local fallingVelocity = player.vy
    player.x = clamp(player.x + player.vx * dt, worldMinX, levelEndX)
    player.y = player.y + player.vy * dt

    collide_platforms(previousY)
    if player.grounded and not wasGrounded and fallingVelocity < -170.0 then
        spawn_spine_effect("dust", player.x, player.y + 20.0, {
            z = spineEffectZ,
            facing = player.facing,
            targetExtent = 104.0,
        })
    end
    update_safe_respawn_point()
    if player.y < worldMinY then
        if controls.fallDeathEnabled then
            kill_player("fall")
        else
            reset_player()
        end
        combat.update_projectiles(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        return
    end

    update_goal(dt)
    if player.won and levelAdvanceTimer > 0.0 then
        combat.update_projectiles(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        return
    end

    combat.update_projectiles(dt)
    update_enemies(dt, previousY, fallingVelocity)
    update_spine_effects(dt)
    update_character(false)
    update_camera()
    update_level_streaming(false)
end

local function draw_menu()
    local title = "PLATFORMER"
    if menuReason == "game_over" then
        title = "GAME OVER"
    elseif menuReason == "clear" then
        title = "ALL CLEAR"
    end

    local uiState = corona.get_ui_state ~= nil and corona.get_ui_state("compact") or {}
    local screenWidth = uiState.display_width or uiState.window_width or 1280.0
    local screenHeight = uiState.display_height or uiState.window_height or 720.0
    local panelWidth = math.max(340.0, math.min(560.0, screenWidth - 48.0))
    local panelHeight = math.max(320.0, math.min(370.0, screenHeight - 48.0))
    local panelX = math.floor((screenWidth - panelWidth) * 0.5 + 0.5)
    local panelY = math.floor((screenHeight - panelHeight) * 0.5 + 0.5)
    local centerX = panelX + panelWidth * 0.5

    local function draw_centered_text(text, y, glyphWidth)
        local approxWidth = string.len(text) * (glyphWidth or 7.0)
        ui.overlay_text(text, centerX - approxWidth * 0.5, y)
    end

    ui.overlay_rect_filled(0.0, 0.0, screenWidth, screenHeight, { 0.0, 0.0, 0.0, 0.46 }, 0.0)
    ui.overlay_rect_filled(panelX, panelY, panelWidth, panelHeight, { 0.035, 0.045, 0.052, 0.92 }, 8.0)
    ui.overlay_rect(panelX, panelY, panelWidth, panelHeight, { 0.86, 0.70, 0.34, 0.82 }, 1.5, 8.0)
    ui.overlay_rect_filled(panelX + 24.0, panelY + 80.0, panelWidth - 48.0, 1.0, { 0.86, 0.70, 0.34, 0.42 }, 0.0)

    draw_centered_text(title, panelY + 34.0, 8.2)
    draw_centered_text("Lives " .. tostring(lives) .. "/" .. tostring(maxLives) .. "    HP " .. tostring(health) .. "/" .. tostring(maxHealth), panelY + 100.0, 6.6)
    draw_centered_text("Level " .. tostring(currentLevel) .. " / " .. tostring(levelCount), panelY + 126.0, 6.6)

    local buttonWidth = math.min(panelWidth - 96.0, 330.0)
    local buttonHeight = 52.0
    local buttonX = centerX - buttonWidth * 0.5
    local startButtonY = panelY + 174.0
    if ui.overlay_button ~= nil then
        if ui.overlay_button(
            "platformer.menu.start",
            ">  START GAME",
            buttonX,
            startButtonY,
            buttonWidth,
            buttonHeight,
            { 0.07, 0.27, 0.24, 0.96 },
            { 0.10, 0.41, 0.35, 0.98 },
            { 0.72, 0.96, 0.84, 0.90 },
            8.0) then
            start_new_game()
            return
        end
        if ui.overlay_button(
            "platformer.menu.exit",
            "X  EXIT",
            buttonX,
            startButtonY + 72.0,
            buttonWidth,
            buttonHeight,
            { 0.28, 0.11, 0.11, 0.95 },
            { 0.44, 0.15, 0.13, 0.98 },
            { 0.96, 0.78, 0.56, 0.90 },
            8.0) then
            request_exit()
            return
        end
        return
    end

    ui.begin_window("Platformer Menu")
    ui.text(title)
    ui.text("Lives " .. tostring(lives) .. "/" .. tostring(maxLives) .. "    HP " .. tostring(health) .. "/" .. tostring(maxHealth))
    ui.text("Level " .. tostring(currentLevel) .. "/" .. tostring(levelCount))
    if ui.button("START GAME", "platformer.menu.start") then
        ui.end_window()
        start_new_game()
        return
    end
    if ui.button("EXIT", "platformer.menu.exit") then
        ui.end_window()
        request_exit()
        return
    end
    ui.end_window()
end

function script.ui()
    if gameState == "menu" then
        draw_menu()
        return
    end

    if not controls.showHud then
        return
    end

    ui.world_text(
        "platformer.title",
        { player.x - 245.0, player.y + 280.0, characterZ },
        gameCompleted and "ALL CLEAR - press R to restart" or (player.dead and (lives <= 0 and "GAME OVER" or "DOWN - respawning") or (player.won and "LEVEL CLEAR" or (playerDisplayName .. " platformer  Level " .. tostring(currentLevel) .. "/" .. tostring(levelCount)))),
        0.0,
        0.0,
        { 0.92, 0.96, 1.0, 0.96 })
    ui.world_text(
        "platformer.help",
        { player.x - 245.0, player.y + 252.0, characterZ },
        "A/D or LS move  Space/A/X/up jump-stomp  J/B melee  K/F/Y/RT fire  R/Start reset",
        0.0,
        0.0,
        { 0.70, 0.84, 0.92, 0.92 })
    ui.world_health_bar(
        "platformer.health",
        { player.x - 245.0, player.y + 226.0, characterZ },
        health / maxHealth,
        "HP " .. tostring(health) .. "/" .. tostring(maxHealth),
        188.0,
        12.0)
    ui.world_text(
        "platformer.vitals",
        { player.x - 245.0, player.y + 206.0, characterZ },
        "Lives " .. tostring(lives) .. "/" .. tostring(maxLives) .. "  deaths " .. tostring(deathCount) .. "  falls " .. tostring(fallDeathCount),
        0.0,
        0.0,
        { 0.94, 0.82, 0.58, 0.94 })
    ui.world_text(
        "platformer.enemies",
        { player.x - 245.0, player.y + 176.0, characterZ },
        "Enemies " .. tostring(defeatedEnemyCount) .. "/" .. tostring(currentLevelEnemyTotal) .. "  active " .. tostring(activeEnemyCount) .. "  zones " .. tostring(loadedLevelZoneCount) .. "/" .. tostring(#levelZones),
        0.0,
        0.0,
        { 0.82, 0.90, 0.72, 0.92 })
end

function script.shutdown()
    destroy_level_entities()
    destroy_cached_effect_entities()
    sceneCache = {}
    failedSceneCache = {}
    lastCharacterHandle = nil
    corona.set_default_world_visible(true)
end

return script
