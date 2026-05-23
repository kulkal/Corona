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
controls.levelStreaming = false
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
    controls.maxLevelZoneObjectsPerFrame = 2
end
if controls.maxForcedLevelZoneObjectsPerFrame == nil then
    controls.maxForcedLevelZoneObjectsPerFrame = 3
end
if controls.maxInitialZoneLoads == nil then
    controls.maxInitialZoneLoads = 1
end
if controls.maxEffectPrewarmEntitiesPerFrame == nil then
    controls.maxEffectPrewarmEntitiesPerFrame = 1
end
if controls.effectPrewarmDelaySeconds == nil then
    controls.effectPrewarmDelaySeconds = 5.0
end
if controls.effectPrewarmIntervalSeconds == nil then
    controls.effectPrewarmIntervalSeconds = 0.75
end
if controls.effectPrewarmSceneHandles == nil then
    controls.effectPrewarmSceneHandles = false
end
if controls.deferSpineSamplingWhileStreamingSeconds == nil then
    controls.deferSpineSamplingWhileStreamingSeconds = 1.35
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
if controls.platformBrickTexture == nil then
    controls.platformBrickTexture = false
end
if controls.platformerCastShadows == nil then
    controls.platformerCastShadows = false
end
if controls.playerSpineCastShadows == nil then
    controls.playerSpineCastShadows = false
end
controls.platformRayTracing = controls.platformerCastShadows == true
controls.playerSpineRayTracing = controls.playerSpineCastShadows == true
if controls.backdropBrickTexture == nil then
    controls.backdropBrickTexture = false
end
if controls.platformTrim == nil then
    controls.platformTrim = true
end
if controls.backgroundDetail == nil then
    controls.backgroundDetail = "medium"
end
if controls.platformerBackgroundGeometry == nil then
    controls.platformerBackgroundGeometry = false
end
if controls.useTmxTileTextures == nil then
    controls.useTmxTileTextures = true
end
if controls.platformMaterialDetails == nil then
    controls.platformMaterialDetails = true
end
if controls.platformFloorTileHeight == nil then
    controls.platformFloorTileHeight = 100.0
end
if controls.platformRaisedTileHeight == nil then
    controls.platformRaisedTileHeight = 49.0
end
if controls.platformObstacleTileHeight == nil then
    controls.platformObstacleTileHeight = 108.0
end
if controls.spriteRenderDepthStep == nil then
    controls.spriteRenderDepthStep = 0.55
end
if controls.spriteEntityDepthStep == nil then
    controls.spriteEntityDepthStep = 0.04
end
if controls.platformSurfaceDepthOffset == nil then
    controls.platformSurfaceDepthOffset = 2.0
end
if controls.startPlatformWidth == nil then
    controls.startPlatformWidth = 540.0
end
if controls.backgroundCameraDepthZ == nil then
    controls.backgroundCameraDepthZ = 220.0
end
if controls.backgroundCameraDepthScale == nil then
    controls.backgroundCameraDepthScale = 1.10
end
if controls.backgroundPanelDepth == nil then
    controls.backgroundPanelDepth = 0.35
end
if controls.backgroundLayerDepthScale == nil then
    controls.backgroundLayerDepthScale = 0.80
end
if controls.cameraDistance == nil then
    controls.cameraDistance = 2200.0
end
if controls.cameraFov == nil then
    controls.cameraFov = 0.24
end
if controls.cameraVerticalOffset == nil then
    controls.cameraVerticalOffset = 112.0
end
if controls.rangedAmmoMax == nil then
    controls.rangedAmmoMax = 9
end
if controls.startingRangedAmmo == nil then
    controls.startingRangedAmmo = 0
end
if controls.rangedAmmoDropAmount == nil then
    controls.rangedAmmoDropAmount = 1
end
if controls.rangedAmmoDropChance == nil then
    controls.rangedAmmoDropChance = 1.0
end
if controls.healthPickupHealAmount == nil then
    controls.healthPickupHealAmount = 20
end
if controls.healthPickupDropChance == nil then
    controls.healthPickupDropChance = 0.35
end
if controls.healthPickupRadius == nil then
    controls.healthPickupRadius = 46.0
end
if controls.healthPickupLifetime == nil then
    controls.healthPickupLifetime = 22.0
end

if not controls.enabled then
    return script
end

if corona.set_persistent_control ~= nil then
    corona.set_persistent_control("dungeon.enabled", false)
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
corona.set_ui_value("enable_direct_diffuse", true)
corona.set_ui_value("enable_direct_specular", true)
corona_platformer_sun_direction = controls.sunDirection or { -0.08, 0.22, -0.97 }
corona_platformer_sun_color = controls.sunColor or { 1.0, 0.98, 0.86 }
corona_platformer_sun_intensity = controls.sunIntensity or 1.35
corona.set_ui_value("light_dir", corona_platformer_sun_direction)
corona.set_ui_value("light_intensity", corona_platformer_sun_intensity)
corona.set_ui_value("sky_color_top", controls.skyColorTop or { 0.58, 0.84, 1.10 })
corona.set_ui_value("sky_color_bottom", controls.skyColorBottom or { 0.88, 0.98, 1.12 })
corona.set_ui_value("sky_lighting_strength", controls.skyLightingStrength or 0.42)
corona.set_ui_value("sky_intensity", controls.skyIntensity or 0.78)
corona.set_ui_value("surface_bounce_strength", controls.surfaceBounceStrength or 0.08)
corona.set_ui_value("sun_angular_radius", controls.sunAngularRadius or 0.03)
corona.set_ui_value("tone_map_mode", 1)

local enemyAssets = corona_platformer_enemy_assets or {}
local totalEnemyAssets = #enemyAssets
local levelLayoutDocument = corona_platformer_level_layouts or {}
local levelLayouts = levelLayoutDocument.levels or {}
local levelPhysics = levelLayoutDocument.physics or {}

local spawned = {}
local platformData = {}
local levelZones = {}
local levelZoneWidth = math.max(120.0, controls.streamingZoneWidth or 780.0)
local levelStreamingEnabled = controls.levelStreaming == true
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
corona_platformer_effect_prewarm_queue = {}
corona_platformer_effect_prewarm_index = 1
corona_platformer_effect_prewarm_warmed = 0
corona_platformer_effect_prewarm_logged = false
corona_platformer_effect_prewarm_next_time = 0.0
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
local playerSampleFps = controls.playerSampleFps or 8.0
local enemySampleFps = controls.enemySampleFps or 2.0
local loopSeconds = {
    idle = 2.0,
    walk = 0.95,
    attack = 0.65,
}
local playerAnimationSampleFps = {
    idle = controls.playerIdleSampleFps or 3.0,
    walk = controls.playerWalkSampleFps or 8.0,
    attack = controls.playerAttackSampleFps or 8.0,
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
local platformDepth = controls.platformDepth or 18.0
local spritePlaneZ = controls.spritePlaneZ or 0.0
corona_platformer_platform_surface_z = spritePlaneZ + math.max(0.0, controls.platformSurfaceDepthOffset or 2.0)
local spriteRenderDepthStep = math.max(0.0, controls.spriteRenderDepthStep or 0.55)
local spriteEntityDepthStep = math.max(0.0, controls.spriteEntityDepthStep or 0.04)
local characterZ = spritePlaneZ - spriteRenderDepthStep
local enemyZ = spritePlaneZ - spriteRenderDepthStep * 0.82
local spineEffectZ = spritePlaneZ - spriteRenderDepthStep * 1.55
function corona_platformer_platform_center_z(depth)
    return corona_platformer_platform_surface_z + math.max(0.001, depth or platformDepth) * 0.5
end
local platformZ = corona_platformer_platform_center_z(platformDepth)
corona_platformer_background_camera_z = controls.backgroundCameraDepthZ or 220.0
corona_platformer_background_camera_scale = math.max(1.0, controls.backgroundCameraDepthScale or 1.10)
corona_platformer_background_panel_depth = math.max(0.001, controls.backgroundPanelDepth or 0.35)
corona_platformer_background_layer_depth_scale = math.max(0.05, controls.backgroundLayerDepthScale or 0.80)
function corona_platformer_background_z(layer)
    return corona_platformer_background_camera_z + (layer or 0.0) * corona_platformer_background_layer_depth_scale
end
corona_platformer_collision_debug_enabled = controls.showCollisionDebug == true
corona_platformer_collision_debug_entities = {}
corona_platformer_collision_debug_dirty = true
corona_platformer_collision_debug_line_thickness = math.max(1.0, controls.collisionDebugLineThickness or 3.0)
corona_platformer_collision_debug_color = controls.collisionDebugColor or { 0.20, 1.0, 0.42 }
corona_platformer_layout_offset_x = 0.0
corona_platformer_layout_mirror_x = false
corona_platformer_layout_mirror_center_x = 0.0
local moveSpeed = levelPhysics.move_speed or 330.0
local acceleration = levelPhysics.acceleration or 1780.0
local friction = levelPhysics.friction or 1420.0
local jumpVelocity = levelPhysics.jump_velocity or 780.0
local stompBounceVelocity = levelPhysics.stomp_bounce_velocity or 560.0
local gravity = levelPhysics.gravity or -1500.0
local maxFallSpeed = levelPhysics.max_fall_speed or -940.0
local worldMinY = levelPhysics.world_min_y or -260.0
local enemySpawnDistance = controls.enemySpawnDistance or 980.0
local enemyDespawnDistance = controls.enemyDespawnDistance or 1240.0
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
        fps = 12.0,
        targetExtent = 92.0,
    },
    hit = {
        path = "assets/spine/effects/hit/hit.json",
        animation = "idle",
        duration = 0.28,
        fps = 16.0,
        targetExtent = 96.0,
    },
    explosion = {
        path = "assets/spine/effects/explosion/explosion.json",
        animation = "idle",
        duration = 0.52,
        fps = 14.0,
        targetExtent = 150.0,
    },
    spawn = {
        path = "assets/spine/effects/spawn/spawn.json",
        animation = "idle",
        duration = 0.50,
        fps = 12.0,
        targetExtent = 118.0,
    },
    skill_ready = {
        path = "assets/spine/effects/skill_ready/skill_ready.json",
        animation = "idle",
        duration = 1.05,
        fps = 12.0,
        targetExtent = 178.0,
    },
    movingflag = {
        path = "assets/spine/effects/movingflag/movingflag.json",
        animation = "idle",
        duration = 1.0,
        fps = 8.0,
        targetExtent = 132.0,
    },
    fire = {
        path = "assets/spine/effects/fire/fire.json",
        animation = "idle",
        duration = 0.86,
        fps = 12.0,
        targetExtent = 78.0,
    },
    lightning = {
        path = "assets/spine/effects/lightning/lightning.json",
        animation = "idle",
        duration = 0.62,
        fps = 12.0,
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

local function set_spine_pose(entity, sceneHandle, x, y, z, yaw, targetExtent, roughness, metallic, mirrorX, useWorldScale, rayTracing)
    local castRayTracingShadow = rayTracing == true
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
            useWorldScale == true,
            castRayTracingShadow)
    end

    local patch = {
        position = { x, y, z },
        rotation = { 0.0, yaw, 0.0 },
        ray_tracing = castRayTracingShadow,
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

local function set_character_spine_pose(entity, sceneHandle, x, y, z, yaw, visualExtent, roughness, metallic, mirrorX, rayTracing)
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
        false,
        rayTracing == true)
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

function corona_platformer_color_component(color, index, fallback)
    if color == nil then
        return fallback
    end
    return color[index] or fallback
end

function corona_platformer_mix_color(color, tint, amount)
    local t = clamp(amount or 0.5, 0.0, 1.0)
    return {
        corona_platformer_color_component(color, 1, tint[1]) * (1.0 - t) + tint[1] * t,
        corona_platformer_color_component(color, 2, tint[2]) * (1.0 - t) + tint[2] * t,
        corona_platformer_color_component(color, 3, tint[3]) * (1.0 - t) + tint[3] * t,
    }
end

corona_platformer_tmx_texture_root = "assets/tiles/maple_style/"
corona_platformer_tmx_textures = {
    platform_grass = corona_platformer_tmx_texture_root .. "grass_block.png",
    platform_stone = corona_platformer_tmx_texture_root .. "stone_platform.png",
    platform_dirt = corona_platformer_tmx_texture_root .. "dirt_block.png",
    meadow_grass = corona_platformer_tmx_texture_root .. "grass_tuft.png",
    leaf_bush = corona_platformer_tmx_texture_root .. "bush.png",
    rock_moss = corona_platformer_tmx_texture_root .. "rock_moss.png",
    sky_soft = corona_platformer_tmx_texture_root .. "cloud.png",
    flower_field = corona_platformer_tmx_texture_root .. "flower.png",
    platform_cap_grass = corona_platformer_tmx_texture_root .. "grass_cap.png",
    backdrop_rock = corona_platformer_tmx_texture_root .. "stone_block.png",
    grass_tuft = corona_platformer_tmx_texture_root .. "grass_tuft.png",
    grass_blade = corona_platformer_tmx_texture_root .. "grass_blade.png",
    grass_sprig = corona_platformer_tmx_texture_root .. "grass_sprig.png",
}

function corona_platformer_texture_kind(name, fallback)
    if controls.useTmxTileTextures == false then
        return fallback
    end
    return corona_platformer_tmx_textures[name] or fallback
end

corona_platformer_platform_visual_styles = {
    floor = {
        base = { 0.50, 0.68, 0.40 },
        cap = { 0.58, 0.78, 0.42 },
        under = { 0.36, 0.28, 0.18 },
        glow = { 0.62, 0.92, 0.46 },
        texture = corona_platformer_texture_kind("platform_grass", "grass"),
        capTexture = corona_platformer_texture_kind("platform_cap_grass", "grass"),
        underTexture = corona_platformer_texture_kind("platform_dirt", "dirt"),
        tileWidth = 107.0,
        tileHeight = controls.platformFloorTileHeight,
        capTileWidth = 198.0,
        capHeight = 46.0,
        underTileWidth = 98.0,
        uv = 8.0,
    },
    platform = {
        base = { 0.52, 0.62, 0.52 },
        cap = { 0.56, 0.76, 0.44 },
        under = { 0.31, 0.33, 0.28 },
        glow = { 0.70, 0.94, 0.52 },
        texture = corona_platformer_texture_kind("platform_stone", "rock"),
        capTexture = corona_platformer_texture_kind("platform_cap_grass", "grass"),
        underTexture = corona_platformer_texture_kind("platform_dirt", "dirt"),
        tileWidth = 96.0,
        tileHeight = controls.platformRaisedTileHeight,
        capTileWidth = 198.0,
        capHeight = 24.0,
        underTileWidth = 98.0,
        uv = 3.0,
    },
    obstacle = {
        base = { 0.58, 0.54, 0.45 },
        cap = { 0.54, 0.70, 0.40 },
        under = { 0.34, 0.30, 0.25 },
        glow = { 0.96, 0.80, 0.36 },
        texture = corona_platformer_texture_kind("rock_moss", "rock"),
        capTexture = corona_platformer_texture_kind("leaf_bush", "leaf"),
        underTexture = corona_platformer_texture_kind("platform_dirt", "dirt"),
        tileWidth = 96.0,
        tileHeight = controls.platformObstacleTileHeight,
        capTileWidth = 108.0,
        capHeight = 28.0,
        underTileWidth = 98.0,
        uv = 2.0,
    },
}

function corona_platformer_get_platform_visual_style(styleKind)
    return corona_platformer_platform_visual_styles[styleKind or ""] or corona_platformer_platform_visual_styles.platform
end

function corona_platformer_quantize_uv_repeat(value)
    return clamp(math.floor((value or 1.0) + 0.5), 1.0, 64.0)
end

function corona_platformer_platform_uv_repeat(width, height, style)
    if style ~= nil and style.tileWidth ~= nil and style.tileWidth > 0.001 then
        return corona_platformer_quantize_uv_repeat((width or 1.0) / style.tileWidth)
    end
    local longest = math.max(width or 1.0, height or 1.0)
    local base = style.uv or 8.0
    if longest > 700.0 then
        return corona_platformer_quantize_uv_repeat(base + 5.0)
    end
    if longest > 420.0 then
        return corona_platformer_quantize_uv_repeat(base + 3.0)
    end
    if longest > 220.0 then
        return corona_platformer_quantize_uv_repeat(base + 1.0)
    end
    return corona_platformer_quantize_uv_repeat(base)
end

function corona_platformer_texture_uv_repeat_for_width(width, tileWidth, fallbackRepeat)
    if tileWidth ~= nil and tileWidth > 0.001 then
        return corona_platformer_quantize_uv_repeat((width or 1.0) / tileWidth)
    end
    return fallbackRepeat or 1.0
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

function corona_platformer_progress_world_direction()
    local direction = screen_axis_to_world_axis(1.0)
    return direction >= 0.0 and 1.0 or -1.0
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
combat.rangedAmmoMax = math.max(1, math.floor(controls.rangedAmmoMax or 9))
combat.rangedAmmo = clamp(math.floor(controls.startingRangedAmmo or 0), 0, combat.rangedAmmoMax)
combat.rangedAmmoDropAmount = math.max(0, math.floor(controls.rangedAmmoDropAmount or 1))
combat.rangedAmmoDropChance = clamp(controls.rangedAmmoDropChance or 1.0, 0.0, 1.0)
combat.rangedPickupRadius = math.max(12.0, controls.rangedPickupRadius or 46.0)
combat.rangedPickupLifetime = math.max(1.0, controls.rangedPickupLifetime or 22.0)
combat.rangedAmmoPickups = {}
combat.healthPickupHealAmount = math.max(1, math.floor(controls.healthPickupHealAmount or 20))
combat.healthPickupDropChance = clamp(controls.healthPickupDropChance or 0.35, 0.0, 1.0)
combat.healthPickupRadius = math.max(12.0, controls.healthPickupRadius or 46.0)
combat.healthPickupLifetime = math.max(1.0, controls.healthPickupLifetime or 22.0)
combat.healthPickups = {}

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

    local leftAnchor = controls.levelLeftAnchor or -880.0
    local startInset = controls.levelStartInset or 260.0
    local goalInset = controls.levelGoalInset or 320.0
    local enemyStartInset = controls.levelEnemyStartInset or 650.0
    local progressDirection = corona_platformer_progress_world_direction()
    corona_platformer_layout_offset_x = 0.0
    corona_platformer_layout_mirror_x = false
    corona_platformer_layout_mirror_center_x = 0.0

    if currentLayout ~= nil then
        local rawFirstPlayableX = currentLayout.first_playable_x or leftAnchor
        local rawEndX = currentLayout.end_x or (rawFirstPlayableX + 6200.0)
        local rawStartX = currentLayout.start_x or (rawFirstPlayableX + startInset)
        local rawGoalX = currentLayout.goal_x or (rawEndX - goalInset)
        local levelWidth = math.max(1400.0, math.abs(rawEndX - rawFirstPlayableX))
        local rawDirection = rawGoalX >= rawStartX and 1.0 or -1.0

        corona_platformer_layout_mirror_x = rawDirection ~= progressDirection
        corona_platformer_layout_mirror_center_x = rawFirstPlayableX + levelWidth * 0.5
        corona_platformer_layout_offset_x = leftAnchor - rawFirstPlayableX

        firstPlayableX = leftAnchor
        worldMinX = firstPlayableX + 70.0
        levelEndX = firstPlayableX + levelWidth
    else
        firstPlayableX = leftAnchor
        worldMinX = firstPlayableX + 70.0
        local levelWidth = 6200.0 + (currentLevel - 1) * 260.0 + math.max(0, currentLevelEnemyTotal - 10) * 72.0
        levelEndX = firstPlayableX + levelWidth
    end

    if progressDirection >= 0.0 then
        startX = firstPlayableX + startInset
        goalX = levelEndX - goalInset
    else
        startX = levelEndX - startInset
        goalX = firstPlayableX + goalInset
    end
    enemyStartX = startX + progressDirection * enemyStartInset

    if (goalX - startX) * progressDirection <= 0.0 then
        local minimumSpan = math.max(1200.0, controls.minimumLeftToRightLevelSpan or 3600.0)
        goalX = startX + progressDirection * minimumSpan
        worldMinX = math.min(worldMinX, goalX - goalInset)
        levelEndX = math.max(levelEndX, goalX + goalInset)
    end

    if currentLevelEnemyTotal > 1 then
        local spacing = ((goalX - enemyStartX) * progressDirection - 500.0) / (currentLevelEnemyTotal - 1)
        enemySpacing = progressDirection * clamp(spacing, 210.0, 620.0)
    else
        enemySpacing = progressDirection * 260.0
    end
end

local function spawn_box(name, position, scale, color, roughness, rayTracing, ownerList, options)
    options = options or {}
    local entity = corona.Entity.create(name)
    local ok = corona.MeshComponent.add(entity, {
        primitive = "box",
        color = color,
        brick_texture = options.brickTexture == true or options.brick_texture == true,
        texture_kind = options.textureKind or options.texture_kind or options.texture,
        uv_repeat = options.uvRepeat or options.uv_repeat or options.textureRepeat or options.texture_repeat or 1.0,
        position = position,
        scale = scale,
        roughness = roughness or 0.86,
        metallic = 0.0,
        override_material = true,
        ray_tracing = rayTracing == true,
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

function combat.destroy_ranged_ammo_pickup(pickup)
    if pickup == nil then
        return
    end
    for _, entity in ipairs(pickup.entities or {}) do
        corona.Entity.destroy(entity)
    end
    pickup.entities = {}
end

function combat.remove_ranged_ammo_pickup(index)
    local pickup = combat.rangedAmmoPickups[index]
    if pickup ~= nil then
        combat.destroy_ranged_ammo_pickup(pickup)
    end
    table.remove(combat.rangedAmmoPickups, index)
end

combat.clear_ranged_ammo_pickups = function()
    for _, pickup in ipairs(combat.rangedAmmoPickups) do
        combat.destroy_ranged_ammo_pickup(pickup)
    end
    combat.rangedAmmoPickups = {}
end

function combat.set_pickup_part(entity, x, y, z, scale)
    if entity == nil then
        return
    end
    corona.MeshComponent.set(entity, {
        position = { x, y, z },
        scale = scale,
    })
end

function combat.spawn_ranged_ammo_pickup(x, y, amount)
    amount = math.max(0, math.floor(amount or combat.rangedAmmoDropAmount or 1))
    if amount <= 0 then
        return
    end

    local pickup = {
        x = x,
        y = y + 34.0,
        baseY = y + 34.0,
        z = spineEffectZ - 0.42,
        amount = amount,
        age = 0.0,
        phase = time + (#combat.rangedAmmoPickups + 1) * 0.37,
        radius = combat.rangedPickupRadius,
        entities = {},
    }

    spawn_box("PlatformerRangedAmmoCore", { pickup.x, pickup.y, pickup.z }, { 20.0, 24.0, 8.0 }, { 1.0, 0.38, 0.10 }, 0.36, false, pickup.entities)
    spawn_box("PlatformerRangedAmmoFlame", { pickup.x, pickup.y + 18.0, pickup.z - 0.4 }, { 12.0, 20.0, 7.0 }, { 1.0, 0.74, 0.18 }, 0.28, false, pickup.entities)
    spawn_box("PlatformerRangedAmmoGlow", { pickup.x, pickup.y + 2.0, pickup.z - 0.8 }, { 34.0, 8.0, 6.0 }, { 1.0, 0.88, 0.38 }, 0.52, false, pickup.entities)

    if #pickup.entities > 0 then
        table.insert(combat.rangedAmmoPickups, pickup)
    end
end

function combat.maybe_spawn_ranged_ammo_drop(enemy)
    if enemy == nil or combat.rangedAmmoDropAmount <= 0 then
        return
    end
    if combat.rangedAmmoDropChance < 1.0 and math.random() > combat.rangedAmmoDropChance then
        return
    end
    combat.spawn_ranged_ammo_pickup(enemy.x, enemy.y + enemy.hitHeight * 0.16, combat.rangedAmmoDropAmount)
end

combat.update_ranged_ammo_pickups = function(dt)
    for index = #combat.rangedAmmoPickups, 1, -1 do
        local pickup = combat.rangedAmmoPickups[index]
        pickup.age = pickup.age + dt
        local bob = math.sin(time * 5.2 + pickup.phase) * 5.0
        local pulse = 1.0 + math.sin(time * 6.8 + pickup.phase) * 0.10
        local y = pickup.baseY + bob

        combat.set_pickup_part(pickup.entities[1], pickup.x, y, pickup.z, { 20.0 * pulse, 24.0 * pulse, 8.0 })
        combat.set_pickup_part(pickup.entities[2], pickup.x, y + 18.0 + bob * 0.15, pickup.z - 0.4, { 12.0 * pulse, 20.0 * pulse, 7.0 })
        combat.set_pickup_part(pickup.entities[3], pickup.x, y + 2.0, pickup.z - 0.8, { 34.0 * pulse, 8.0, 6.0 })

        local collectable = gameState == "playing" and not player.dead and combat.rangedAmmo < combat.rangedAmmoMax
        local dx = pickup.x - player.x
        local dy = y - (player.y + playerHeight * 0.50)
        if collectable and math.abs(dx) < (playerRadius + pickup.radius) and math.abs(dy) < (playerHeight * 0.50 + pickup.radius) then
            combat.rangedAmmo = clamp(combat.rangedAmmo + pickup.amount, 0, combat.rangedAmmoMax)
            spawn_spine_effect("skill_ready", pickup.x, y, {
                z = spineEffectZ,
                facing = player.facing,
                targetExtent = 94.0,
                duration = 0.34,
            })
            combat.remove_ranged_ammo_pickup(index)
        elseif pickup.age >= combat.rangedPickupLifetime then
            combat.remove_ranged_ammo_pickup(index)
        end
    end
end

function combat.destroy_health_pickup(pickup)
    if pickup == nil then
        return
    end
    for _, entity in ipairs(pickup.entities or {}) do
        corona.Entity.destroy(entity)
    end
    pickup.entities = {}
end

function combat.remove_health_pickup(index)
    local pickup = combat.healthPickups[index]
    if pickup ~= nil then
        combat.destroy_health_pickup(pickup)
    end
    table.remove(combat.healthPickups, index)
end

combat.clear_health_pickups = function()
    for _, pickup in ipairs(combat.healthPickups) do
        combat.destroy_health_pickup(pickup)
    end
    combat.healthPickups = {}
end

function combat.spawn_health_pickup(x, y, amount)
    amount = math.max(1, math.floor(amount or combat.healthPickupHealAmount or 20))
    local pickup = {
        x = x,
        y = y + 40.0,
        baseY = y + 40.0,
        z = spineEffectZ - 0.46,
        amount = amount,
        age = 0.0,
        phase = time + (#combat.healthPickups + 1) * 0.41,
        radius = combat.healthPickupRadius,
        entities = {},
    }

    spawn_box("PlatformerHealthPickupCore", { pickup.x, pickup.y, pickup.z }, { 24.0, 24.0, 8.0 }, { 0.12, 0.94, 0.38 }, 0.34, false, pickup.entities)
    spawn_box("PlatformerHealthPickupBarH", { pickup.x, pickup.y, pickup.z - 0.5 }, { 36.0, 10.0, 7.0 }, { 0.76, 1.0, 0.78 }, 0.30, false, pickup.entities)
    spawn_box("PlatformerHealthPickupBarV", { pickup.x, pickup.y, pickup.z - 0.9 }, { 10.0, 36.0, 7.0 }, { 0.76, 1.0, 0.78 }, 0.30, false, pickup.entities)

    if #pickup.entities > 0 then
        table.insert(combat.healthPickups, pickup)
    end
end

function combat.maybe_spawn_health_drop(enemy)
    if enemy == nil or combat.healthPickupHealAmount <= 0 then
        return
    end
    if combat.healthPickupDropChance < 1.0 and math.random() > combat.healthPickupDropChance then
        return
    end

    local offset = screen_axis_to_world_axis(player.facing)
    if math.abs(offset) < 0.001 then
        offset = 1.0
    end
    combat.spawn_health_pickup(enemy.x - offset * 34.0, enemy.y + enemy.hitHeight * 0.14, combat.healthPickupHealAmount)
end

combat.update_health_pickups = function(dt)
    for index = #combat.healthPickups, 1, -1 do
        local pickup = combat.healthPickups[index]
        pickup.age = pickup.age + dt
        local bob = math.sin(time * 4.8 + pickup.phase) * 5.0
        local pulse = 1.0 + math.sin(time * 6.2 + pickup.phase) * 0.09
        local y = pickup.baseY + bob

        combat.set_pickup_part(pickup.entities[1], pickup.x, y, pickup.z, { 24.0 * pulse, 24.0 * pulse, 8.0 })
        combat.set_pickup_part(pickup.entities[2], pickup.x, y, pickup.z - 0.5, { 36.0 * pulse, 10.0 * pulse, 7.0 })
        combat.set_pickup_part(pickup.entities[3], pickup.x, y, pickup.z - 0.9, { 10.0 * pulse, 36.0 * pulse, 7.0 })

        local collectable = gameState == "playing" and not player.dead and health < maxHealth
        local dx = pickup.x - player.x
        local dy = y - (player.y + playerHeight * 0.50)
        if collectable and math.abs(dx) < (playerRadius + pickup.radius) and math.abs(dy) < (playerHeight * 0.50 + pickup.radius) then
            health = clamp(health + pickup.amount, 0, maxHealth)
            spawn_spine_effect("skill_ready", pickup.x, y, {
                z = spineEffectZ,
                facing = player.facing,
                targetExtent = 104.0,
                duration = 0.36,
            })
            combat.remove_health_pickup(index)
        elseif pickup.age >= combat.healthPickupLifetime then
            combat.remove_health_pickup(index)
        end
    end
end

combat.clear_pickups = function()
    combat.clear_ranged_ammo_pickups()
    combat.clear_health_pickups()
end

combat.update_pickups = function(dt)
    combat.update_ranged_ammo_pickups(dt)
    combat.update_health_pickups(dt)
end

function corona_platformer_destroy_collision_debug_entities()
    for _, entity in ipairs(corona_platformer_collision_debug_entities) do
        corona.Entity.destroy(entity)
    end
    corona_platformer_collision_debug_entities = {}
end

function corona_platformer_mark_collision_debug_dirty()
    corona_platformer_collision_debug_dirty = true
end

function corona_platformer_spawn_collision_debug_edge(name, position, scale)
    spawn_box(name, position, scale, corona_platformer_collision_debug_color, 0.42, false, corona_platformer_collision_debug_entities)
end

function corona_platformer_spawn_collision_wire_box(platform)
    local width = math.max(0.001, platform.w or 1.0)
    local height = math.max(0.001, platform.h or 1.0)
    local depth = math.max(0.001, platform.depth or platformDepth)
    local thickness = corona_platformer_collision_debug_line_thickness
    local x = platform.x or 0.0
    local y = platform.y or 0.0
    local z = platform.z or platformZ
    local left = platform.left or (x - width * 0.5)
    local right = platform.right or (x + width * 0.5)
    local bottom = platform.bottom or (y - height * 0.5)
    local top = platform.top or (y + height * 0.5)
    local front = platform.front or (z - depth * 0.5)
    local back = platform.back or (z + depth * 0.5)

    corona_platformer_spawn_collision_debug_edge("CollisionWireXTopFront", { x, top, front }, { width, thickness, thickness })
    corona_platformer_spawn_collision_debug_edge("CollisionWireXTopBack", { x, top, back }, { width, thickness, thickness })
    corona_platformer_spawn_collision_debug_edge("CollisionWireXBottomFront", { x, bottom, front }, { width, thickness, thickness })
    corona_platformer_spawn_collision_debug_edge("CollisionWireXBottomBack", { x, bottom, back }, { width, thickness, thickness })

    corona_platformer_spawn_collision_debug_edge("CollisionWireYLeftFront", { left, y, front }, { thickness, height, thickness })
    corona_platformer_spawn_collision_debug_edge("CollisionWireYLeftBack", { left, y, back }, { thickness, height, thickness })
    corona_platformer_spawn_collision_debug_edge("CollisionWireYRightFront", { right, y, front }, { thickness, height, thickness })
    corona_platformer_spawn_collision_debug_edge("CollisionWireYRightBack", { right, y, back }, { thickness, height, thickness })

    corona_platformer_spawn_collision_debug_edge("CollisionWireZTopLeft", { left, top, z }, { thickness, thickness, depth })
    corona_platformer_spawn_collision_debug_edge("CollisionWireZTopRight", { right, top, z }, { thickness, thickness, depth })
    corona_platformer_spawn_collision_debug_edge("CollisionWireZBottomLeft", { left, bottom, z }, { thickness, thickness, depth })
    corona_platformer_spawn_collision_debug_edge("CollisionWireZBottomRight", { right, bottom, z }, { thickness, thickness, depth })
end

function corona_platformer_refresh_collision_debug()
    if not corona_platformer_collision_debug_dirty then
        return
    end

    corona_platformer_destroy_collision_debug_entities()
    if corona_platformer_collision_debug_enabled then
        for _, platform in ipairs(platformData) do
            corona_platformer_spawn_collision_wire_box(platform)
        end
    end
    corona_platformer_collision_debug_dirty = false
end

corona_platformer_grass_tuft_tiles = { "grass_tuft", "grass_blade", "grass_sprig" }

function corona_platformer_spawn_grass_tufts(x, y, width, height, depth, centerZ, ownerList, styleKind)
    if controls.platformGrassDecoration == false then
        return
    end
    if styleKind ~= "floor" and styleKind ~= "platform" then
        return
    end
    if width < 56.0 or height < 12.0 then
        return
    end
    local seed = math.floor((x * 13.17 + y * 7.31) % 6000.0)
    local tuftCount = clamp(math.floor(width / 110.0), 1, 4)
    local spacing = width / (tuftCount + 1)
    for i = 1, tuftCount do
        local pseudo = (seed + i * 173) % 100
        if pseudo >= 35 then -- ~65% chance per slot, sparse feel
            local tile = corona_platformer_grass_tuft_tiles[((seed + i) % #corona_platformer_grass_tuft_tiles) + 1]
            local jitter = ((seed * 0.31 + i * 11.7) % 24.0) - 12.0
            local tx = x - width * 0.5 + spacing * i + jitter
            local th = 14.0 + ((seed + i * 5) % 8)
            local tw = th * 1.1
            local ty = y + height * 0.5 + th * 0.5 - 1.0
            spawn_box(
                "PlatformerGrassTuft",
                { tx, ty, centerZ - 0.6 },
                { tw, th, math.max(2.0, depth * 0.45) },
                { 0.46, 0.84, 0.36 },
                0.82,
                false,
                ownerList,
                {
                    brickTexture = false,
                    textureKind = corona_platformer_texture_kind(tile, "grass"),
                    uvRepeat = 1.0,
                })
        end
    end
end

local function spawn_platform(x, y, width, height, depth, color, ownerList, zone, styleKind)
    local safeDepth = math.max(0.001, depth or platformDepth)
    local centerZ = corona_platformer_platform_center_z(safeDepth)
    local visualStyle = corona_platformer_get_platform_visual_style(styleKind)
    local visualColor = corona_platformer_mix_color(color, visualStyle.base, 0.72)
    local uvRepeat = corona_platformer_platform_uv_repeat(width, height, visualStyle)
    local entity = spawn_box(
        "PlatformerBlock",
        { x, y, centerZ },
        { width, height, safeDepth },
        visualColor,
        0.88,
        controls.platformRayTracing == true,
        ownerList,
        {
            brickTexture = controls.platformBrickTexture ~= false,
            textureKind = visualStyle.texture,
            uvRepeat = uvRepeat,
        })

    if controls.platformMaterialDetails ~= false and width >= 72.0 and height >= 18.0 then
        local capHeight = visualStyle.capHeight or clamp(height * 0.18, 4.0, 8.0)
        capHeight = clamp(capHeight, 4.0, math.max(4.0, height * 0.72))
        local capUvRepeat = corona_platformer_texture_uv_repeat_for_width(
            width,
            visualStyle.capTileWidth or visualStyle.tileWidth,
            math.max(1.0, uvRepeat * 0.65))
        spawn_box(
            "PlatformerBlockGrassCap",
            { x, y + height * 0.5 - capHeight * 0.5, centerZ - 0.35 },
            { width * 1.012, capHeight, safeDepth + 0.7 },
            visualStyle.cap or visualStyle.base,
            0.78,
            false,
            ownerList,
            {
                brickTexture = false,
                textureKind = visualStyle.capTexture or visualStyle.texture,
                uvRepeat = capUvRepeat,
            })

        if styleKind == "platform" and y > 52.0 and width >= 160.0 then
            local supportHeight = clamp((y - height * 0.5) - 18.0, 18.0, 110.0)
            local supportY = 18.0 + supportHeight * 0.5
            local supportWidth = clamp(width * 0.055, 10.0, 18.0)
            local inset = clamp(width * 0.30, 44.0, 118.0)
            for support = -1, 1, 2 do
                spawn_box(
                    "PlatformerBlockVineSupport",
                    { x + inset * support, supportY, centerZ + 0.22 },
                    { supportWidth, supportHeight, safeDepth * 0.68 },
                    visualStyle.under,
                    0.92,
                    false,
                    ownerList,
                    {
                        brickTexture = false,
                        textureKind = visualStyle.underTexture or visualStyle.texture,
                        uvRepeat = 2.0,
                    })
            end
        end
    end

    if controls.platformTrim ~= false and width >= 72.0 and height >= 18.0 then
        local underHeight = clamp(height * 0.12, 4.0, 9.0)
        local glowHeight = clamp(height * 0.045, 2.0, 4.0)
        local underUvRepeat = corona_platformer_texture_uv_repeat_for_width(
            width,
            visualStyle.underTileWidth or visualStyle.tileWidth,
            math.max(1.0, uvRepeat * 0.45))
        spawn_box(
            "PlatformerBlockShadowLip",
            { x, y - height * 0.5 - underHeight * 0.5, centerZ },
            { width, underHeight, safeDepth },
            visualStyle.under,
            0.94,
            false,
            ownerList,
            {
                brickTexture = false,
                textureKind = visualStyle.underTexture or visualStyle.texture,
                uvRepeat = underUvRepeat,
            })
        spawn_box(
            "PlatformerBlockMagicSeam",
            { x, y - height * 0.5 - underHeight - glowHeight * 0.5, centerZ },
            { width * 0.94, glowHeight, safeDepth },
            visualStyle.glow,
            0.5,
            false,
            ownerList,
            { brickTexture = false, uvRepeat = 1.0 })
    end

    corona_platformer_spawn_grass_tufts(x, y, width, height, safeDepth, centerZ, ownerList, styleKind)

    local platform = {
        x = x,
        y = y,
        z = centerZ,
        w = width,
        h = height,
        depth = safeDepth,
        left = x - width * 0.5,
        right = x + width * 0.5,
        bottom = y - height * 0.5,
        top = y + height * 0.5,
        front = centerZ - safeDepth * 0.5,
        back = centerZ + safeDepth * 0.5,
        entity = entity,
        zone = zone,
    }
    table.insert(platformData, platform)
    if zone ~= nil then
        table.insert(zone.platforms, platform)
    end
    corona_platformer_mark_collision_debug_dirty()
end

local function spawn_layout_blocks(blocks, fallbackColor, styleKind)
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
            block.color or fallbackColor,
            nil,
            nil,
            block.style or styleKind)
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
    if not levelStreamingEnabled then
        index = 1
    end
    if index < 1 then
        index = 1
    end

    local zone = levelZones[index]
    if zone == nil then
        local zoneLeft = levelStreamingEnabled and (firstPlayableX + (index - 1) * levelZoneWidth) or firstPlayableX
        local zoneRight = levelStreamingEnabled and (zoneLeft + levelZoneWidth) or levelEndX
        zone = {
            index = index,
            left = zoneLeft,
            right = zoneRight,
            minX = zoneLeft,
            maxX = zoneRight,
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
        brickTexture = options.brickTexture == true or options.brick_texture == true,
        textureKind = options.textureKind or options.texture_kind or options.texture,
        uvRepeat = options.uvRepeat or options.uv_repeat or options.textureRepeat or options.texture_repeat or 1.0,
        background = options.background == true,
        goalTrigger = options.goalTrigger == true,
        goalPart = options.goalPart,
    })
end

function corona_platformer_queue_background_box(name, position, scale, color, roughness, textureKind, uvRepeat)
    local backgroundScale = corona_platformer_background_camera_scale
    spawn_box(
        name,
        {
            position[1] or 0.0,
            position[2] or 0.0,
            corona_platformer_background_z(position[3] or 0.0),
        },
            {
                math.max(0.001, scale[1] or 1.0) * backgroundScale,
                math.max(0.001, scale[2] or 1.0) * backgroundScale,
                corona_platformer_background_panel_depth,
            },
        color,
        roughness,
        false,
        nil,
        {
            brickTexture = false,
            textureKind = textureKind,
            uvRepeat = uvRepeat or 1.0,
        })
end

local function queue_streaming_platform(x, y, width, height, depth, color, styleKind)
    local safeWidth = math.max(0.001, width or 1.0)
    local safeHeight = math.max(0.001, height or 1.0)
    local resolvedStyleKind = styleKind or "platform"
    local visualStyle = corona_platformer_get_platform_visual_style(resolvedStyleKind)
    if visualStyle.tileHeight ~= nil and visualStyle.tileHeight > safeHeight then
        local top = (y or 0.0) + safeHeight * 0.5
        safeHeight = visualStyle.tileHeight
        y = top - safeHeight * 0.5
    end
    local safeDepth = math.max(0.001, depth or platformDepth)
    local centerZ = corona_platformer_platform_center_z(safeDepth)
    local zone = get_level_zone_for_bounds(
        x - safeWidth * 0.5,
        x + safeWidth * 0.5,
        y - safeHeight * 0.5,
        y + safeHeight * 0.5,
        centerZ - safeDepth * 0.5,
        centerZ + safeDepth * 0.5)

    table.insert(zone.platformDescs, {
        x = x,
        y = y,
        z = centerZ,
        w = safeWidth,
        h = safeHeight,
        depth = safeDepth,
        color = clone_vec3(color, { 0.34, 0.42, 0.36 }),
        styleKind = resolvedStyleKind,
    })
end

function corona_platformer_transform_layout_x(x)
    local result = x or 0.0
    if corona_platformer_layout_mirror_x then
        result = corona_platformer_layout_mirror_center_x * 2.0 - result
    end
    return result + corona_platformer_layout_offset_x
end

function corona_platformer_actor_intersects_any_platform(x, y, halfWidth, height, ignoredPlatform)
    for _, platform in ipairs(platformData) do
        if platform ~= ignoredPlatform and corona_platformer_actor_overlaps_platform(x, y, halfWidth, height, platform) then
            return true
        end
    end
    return false
end

function corona_platformer_rebuild_enemy_patrol(enemy, centerX, platform)
    local assetIndex = enemy.assetIndex or 1
    local leftExtent = 58.0 + (assetIndex % 3) * 18.0
    local rightExtent = 58.0 + (assetIndex % 4) * 16.0
    if platform ~= nil then
        local inset = math.max(enemy.hitRadius + 8.0, 40.0)
        local minPatrol = platform.left + inset
        local maxPatrol = platform.right - inset
        if maxPatrol > minPatrol then
            enemy.patrolLeft = math.max(minPatrol, centerX - leftExtent)
            enemy.patrolRight = math.min(maxPatrol, centerX + rightExtent)
            if enemy.patrolRight <= enemy.patrolLeft then
                enemy.patrolLeft = minPatrol
                enemy.patrolRight = maxPatrol
            end
            return
        end
    end

    enemy.patrolLeft = centerX - leftExtent
    enemy.patrolRight = centerX + rightExtent
end

function corona_platformer_find_safe_enemy_spawn(enemy, desiredX)
    local halfWidth = math.max(10.0, enemy.hitRadius * 0.82)
    local height = math.max(32.0, enemy.hitHeight * 0.92)
    local inset = math.max(halfWidth + 6.0, 34.0)
    local best = nil
    local sampleSpacing = math.max(64.0, halfWidth * 1.35)
    local searchSteps = math.max(4, math.floor(controls.enemySpawnSearchSteps or 8))

    for step = 0, searchSteps do
        local offsets
        if step == 0 then
            offsets = { 0.0 }
        else
            local offset = sampleSpacing * step
            offsets = { -offset, offset }
        end

        for _, offset in ipairs(offsets) do
            local sampleX = clamp(desiredX + offset, worldMinX + inset, levelEndX - inset)
            for _, platform in ipairs(platformData) do
                if platform.w >= inset * 2.0 and sampleX >= platform.left + inset and sampleX <= platform.right - inset then
                    local spawnY = platform.top + math.max(0.0, controls.enemySpawnLift or 4.0)
                    local blocked = corona_platformer_actor_intersects_any_platform(sampleX, spawnY + 0.01, halfWidth, height, platform)
                    if not blocked then
                        local score = math.abs(sampleX - desiredX) - platform.top * 0.015
                        if best == nil or score < best.score then
                            best = {
                                x = sampleX,
                                y = spawnY,
                                platform = platform,
                                score = score,
                            }
                        end
                    end
                end
            end
        end

        if best ~= nil and math.abs(best.x - desiredX) <= sampleSpacing * math.max(1, step) then
            return best
        end
    end

    return best
end

function corona_platformer_snap_enemy_to_safe_spawn(enemy)
    local safeSpawn = corona_platformer_find_safe_enemy_spawn(enemy, enemy.baseX or enemy.x)
    if safeSpawn == nil then
        return false
    end

    enemy.baseX = safeSpawn.x
    enemy.x = safeSpawn.x
    enemy.y = safeSpawn.y
    enemy.spawnPlatform = safeSpawn.platform
    corona_platformer_rebuild_enemy_patrol(enemy, safeSpawn.x, safeSpawn.platform)
    return true
end

local function queue_layout_blocks(blocks, fallbackColor, styleKind)
    if blocks == nil then
        return
    end

    for _, block in ipairs(blocks) do
        queue_streaming_platform(
            corona_platformer_transform_layout_x(block.x),
            block.y or 0.0,
            block.w or 1.0,
            block.h or 1.0,
            block.depth or platformDepth,
            block.color or fallbackColor,
            block.style or styleKind)
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

function corona_platformer_get_scene_cache_bucket(root, assetPath, animation, sampleRate, create)
    local byAsset = root[assetPath]
    if byAsset == nil then
        if not create then
            return nil
        end
        byAsset = {}
        root[assetPath] = byAsset
    end

    local byAnimation = byAsset[animation]
    if byAnimation == nil then
        if not create then
            return nil
        end
        byAnimation = {}
        byAsset[animation] = byAnimation
    end

    local byRate = byAnimation[sampleRate]
    if byRate == nil then
        if not create then
            return nil
        end
        byRate = {}
        byAnimation[sampleRate] = byRate
    end
    return byRate
end

local function get_spine_scene(assetPath, animation, sampleTime, fps, fallbackHandle, forceSample)
    local sampleRate = fps or playerSampleFps
    local frame = math.floor(sampleTime * sampleRate + 0.0001)
    local cachedFrames = corona_platformer_get_scene_cache_bucket(sceneCache, assetPath, animation, sampleRate, false)
    if cachedFrames ~= nil then
        local handle = cachedFrames[frame]
        if handle ~= nil then
            return handle, frame, true
        end
    end
    local failedFrames = corona_platformer_get_scene_cache_bucket(failedSceneCache, assetPath, animation, sampleRate, false)
    if failedFrames ~= nil and failedFrames[frame] then
        return fallbackHandle, frame, false
    end

    local deferSamplingSeconds = controls.deferSpineSamplingWhileStreamingSeconds or 0.0
    if not forceSample and deferSamplingSeconds > 0.0 and time < deferSamplingSeconds and corona_platformer_has_active_zone_loads ~= nil and corona_platformer_has_active_zone_loads() then
        return fallbackHandle, frame, false
    end

    local maxSceneSamples = math.max(1, math.floor(controls.maxSceneSamplesPerFrame or 1))
    if not forceSample and sceneSamplesThisFrame >= maxSceneSamples then
        return fallbackHandle, frame, false
    end

    if native_spine_create_scene == nil then
        corona_platformer_get_scene_cache_bucket(failedSceneCache, assetPath, animation, sampleRate, true)[frame] = true
        return fallbackHandle, frame, false
    end

    local ok, result = pcall(native_spine_create_scene, assetPath, animation, frame / sampleRate, 1.0)
    if not ok then
        corona_platformer_get_scene_cache_bucket(failedSceneCache, assetPath, animation, sampleRate, true)[frame] = true
        if fallbackHandle ~= nil then
            return fallbackHandle, frame, false
        end
        corona.log("platformer spine sample failed: " .. tostring(result))
        return nil, frame, false
    end
    if result == nil then
        corona_platformer_get_scene_cache_bucket(failedSceneCache, assetPath, animation, sampleRate, true)[frame] = true
        if fallbackHandle ~= nil then
            return fallbackHandle, frame, false
        end
        corona.log("platformer spine sample returned nil for " .. tostring(assetPath))
        return nil, frame, false
    end

    sceneSamplesThisFrame = sceneSamplesThisFrame + 1
    corona_platformer_get_scene_cache_bucket(sceneCache, assetPath, animation, sampleRate, true)[frame] = result
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
    corona_platformer_effect_prewarm_queue = {}
    corona_platformer_effect_prewarm_index = 1
    corona_platformer_effect_prewarm_warmed = 0
    corona_platformer_effect_prewarm_logged = false
    corona_platformer_effect_prewarm_next_time = 0.0
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

    corona_platformer_effect_prewarm_queue = {}
    corona_platformer_effect_prewarm_index = 1
    corona_platformer_effect_prewarm_warmed = 0
    corona_platformer_effect_prewarm_logged = false
    corona_platformer_effect_prewarm_next_time = math.max(0.0, controls.effectPrewarmDelaySeconds or 1.0)
    local prewarmCount = math.min(effectPrewarmPerKind, effectPoolLimitPerKind)
    for kind, def in pairs(effectAssets) do
        for _ = 1, prewarmCount do
            table.insert(corona_platformer_effect_prewarm_queue, {
                kind = kind,
                def = def,
            })
        end
    end

    if #corona_platformer_effect_prewarm_queue > 0 then
        corona.log("platformer effect entity cache prewarm queued=" .. tostring(#corona_platformer_effect_prewarm_queue))
    end
end

function corona_platformer_has_active_zone_loads()
    for _, zone in pairs(levelZones) do
        if zone.loading then
            return true
        end
    end
    return false
end

function corona_platformer_process_effect_prewarm_queue()
    local queue = corona_platformer_effect_prewarm_queue
    if queue == nil then
        return
    end

    local queueCount = #queue
    local index = corona_platformer_effect_prewarm_index or 1
    if index > queueCount then
        if queueCount > 0 and not corona_platformer_effect_prewarm_logged then
            corona_platformer_effect_prewarm_logged = true
            corona.log("platformer effect entity cache prewarmed_async=" .. tostring(corona_platformer_effect_prewarm_warmed or 0))
        end
        return
    end

    if time < math.max(0.0, controls.effectPrewarmDelaySeconds or 1.0) then
        return
    end
    if corona_platformer_has_active_zone_loads() then
        return
    end
    local nextPrewarmTime = corona_platformer_effect_prewarm_next_time or 0.0
    if time < nextPrewarmTime then
        return
    end
    corona_platformer_effect_prewarm_next_time = time + math.max(0.02, controls.effectPrewarmIntervalSeconds or 0.25)

    local maxSceneSamples = math.max(1, math.floor(controls.maxSceneSamplesPerFrame or 1))
    local maxEntities = math.max(1, math.floor(controls.maxEffectPrewarmEntitiesPerFrame or 1))
    local prewarmSceneHandles = controls.effectPrewarmSceneHandles == true
    local created = 0
    while index <= queueCount and created < maxEntities and (not prewarmSceneHandles or sceneSamplesThisFrame < maxSceneSamples) do
        local item = queue[index]
        local kind = item.kind
        local def = item.def
        local entity = nil
        if prewarmSceneHandles then
            local sampleRate = def.fps or playerSampleFps
            local handle = select(1, get_spine_scene(def.path, def.animation or "idle", 0.0, sampleRate, nil, false))
            if handle ~= nil then
                entity = corona.Entity.create("PlatformerSpineEffect_" .. tostring(kind) .. "_Cached")
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
                if not ok then
                    corona.Entity.destroy(entity)
                    entity = nil
                end
            end
        else
            entity = corona.Entity.create("PlatformerSpineEffect_" .. tostring(kind) .. "_Cached")
            set_effect_entity_visible(entity, false)
        end

        if entity ~= nil then
            set_effect_entity_visible(entity, false)
            local pool = effectEntityPools[kind]
            if pool == nil then
                pool = {}
                effectEntityPools[kind] = pool
            end
            if #pool < effectPoolLimitPerKind then
                table.insert(pool, entity)
                corona_platformer_effect_prewarm_warmed = (corona_platformer_effect_prewarm_warmed or 0) + 1
            else
                corona.Entity.destroy(entity)
            end
            created = created + 1
        end
        index = index + 1
    end

    corona_platformer_effect_prewarm_index = index
    if index > queueCount and not corona_platformer_effect_prewarm_logged then
        corona_platformer_effect_prewarm_logged = true
        corona.log("platformer effect entity cache prewarmed_async=" .. tostring(corona_platformer_effect_prewarm_warmed or 0))
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
    if combat.rangedAmmo <= 0 then
        combat.rangedCooldownTimer = math.max(combat.rangedCooldownTimer, 0.10)
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

    combat.rangedAmmo = math.max(0, combat.rangedAmmo - 1)
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

    while remaining > 0 and zone.loadPlatformIndex <= #zone.platformDescs do
        local desc = zone.platformDescs[zone.loadPlatformIndex]
        zone.loadPlatformIndex = zone.loadPlatformIndex + 1
        remaining = remaining - 1
        used = used + 1
        spawn_platform(desc.x, desc.y, desc.w, desc.h, desc.depth, desc.color, zone.entities, zone, desc.styleKind)
    end

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
            zone.entities,
            {
                brickTexture = desc.brickTexture,
                textureKind = desc.textureKind,
                uvRepeat = desc.uvRepeat,
            })
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
    corona_platformer_mark_collision_debug_dirty()

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
            queue_streaming_spine_effect("skill_ready", corona_platformer_transform_layout_x(block.x), block.y + block.h * 0.5 + 44.0, {
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
        set_character_spine_pose(character, nil, player.x, visualY, characterZ, yaw, playerVisualExtent, 0.82, 0.0, mirrorX, controls.playerSpineRayTracing == true)
        return
    end

    local handle, frame, resolved = get_spine_scene(spineAsset, animation, sampleTime, sampleFps, lastCharacterHandle, forceSample)
    if handle == nil then
        return
    end

    if force or (resolved and (frame ~= lastFrame or animation ~= lastAnimation)) then
        set_character_spine_pose(character, handle, player.x, visualY, characterZ, yaw, playerVisualExtent, 0.82, 0.0, mirrorX, controls.playerSpineRayTracing == true)
        if resolved then
            lastFrame = frame
            lastAnimation = animation
            lastCharacterHandle = handle
        end
    else
        set_character_spine_pose(character, nil, player.x, visualY, characterZ, yaw, playerVisualExtent, 0.82, 0.0, mirrorX, controls.playerSpineRayTracing == true)
    end
end

corona_platformer_fantasy_flower_colors = {
    { 1.00, 0.42, 0.58 },
    { 1.00, 0.78, 0.28 },
    { 0.66, 0.52, 1.00 },
    { 0.98, 0.96, 0.58 },
}

function corona_platformer_queue_fantasy_cloud(x, y, z, width, variant)
    local shade = 0.90 + ((variant or 0) % 3) * 0.025
    local color = { shade, shade + 0.03, 1.0 }
    local cloudTexture = corona_platformer_texture_kind("sky_soft", "cloud")
    corona_platformer_queue_background_box("PlatformerCloudPuffA", { x, y, z }, { width, 26.0, 8.0 }, color, 0.96, cloudTexture, 2.0)
    corona_platformer_queue_background_box("PlatformerCloudPuffB", { x - width * 0.24, y + 13.0, z - 1.0 }, { width * 0.52, 32.0, 8.0 }, color, 0.96, cloudTexture, 1.6)
    corona_platformer_queue_background_box("PlatformerCloudPuffC", { x + width * 0.22, y + 9.0, z - 1.0 }, { width * 0.46, 28.0, 8.0 }, color, 0.96, cloudTexture, 1.6)
end

function corona_platformer_queue_fantasy_grass_clump(x, y, z, variant)
    local bladeCount = controls.backgroundDetail == "high" and (3 + ((variant or 0) % 2)) or 2
    for blade = 1, bladeCount do
        local offset = (blade - (bladeCount + 1) * 0.5) * 7.0
        local height = 16.0 + ((variant + blade) % 4) * 4.0
        local color = blade % 2 == 0 and { 0.34, 0.72, 0.26 } or { 0.48, 0.82, 0.34 }
        corona_platformer_queue_background_box("PlatformerGrassBlade", { x + offset, y + height * 0.5, z - blade * 0.25 }, { 4.0, height, 6.0 }, color, 0.84, corona_platformer_texture_kind("meadow_grass", "grass"), 1.0)
    end
end

function corona_platformer_queue_fantasy_flower(x, y, z, variant)
    local stemHeight = 18.0 + ((variant or 0) % 4) * 3.0
    local petalColor = corona_platformer_fantasy_flower_colors[((variant or 0) % #corona_platformer_fantasy_flower_colors) + 1]
    corona_platformer_queue_background_box("PlatformerFlowerStem", { x, y + stemHeight * 0.5, z }, { 3.5, stemHeight, 5.0 }, { 0.24, 0.60, 0.20 }, 0.82, corona_platformer_texture_kind("leaf_bush", "leaf"), 1.0)
    corona_platformer_queue_background_box("PlatformerFlowerPetals", { x, y + stemHeight + 5.0, z - 0.5 }, { 15.0, 10.0, 5.0 }, petalColor, 0.72, corona_platformer_texture_kind("flower_field", "flower"), 1.0)
    if controls.backgroundDetail == "high" then
        corona_platformer_queue_background_box("PlatformerFlowerCenter", { x, y + stemHeight + 5.0, z - 1.0 }, { 5.0, 5.0, 4.0 }, { 1.0, 0.86, 0.26 }, 0.62, corona_platformer_texture_kind("flower_field", "flower"), 1.0)
    end
end

function corona_platformer_queue_fantasy_rock(x, y, z, size, variant)
    local tint = 0.42 + ((variant or 0) % 3) * 0.045
    local base = { tint, tint + 0.035, tint + 0.045 }
    local rockTexture = corona_platformer_texture_kind("backdrop_rock", "rock")
    corona_platformer_queue_background_box("PlatformerMeadowRockA", { x, y + size * 0.25, z }, { size * 1.1, size * 0.50, 16.0 }, base, 0.92, rockTexture, 2.2)
    if controls.backgroundDetail == "high" then
        corona_platformer_queue_background_box("PlatformerMeadowRockB", { x + size * 0.24, y + size * 0.47, z - 1.0 }, { size * 0.55, size * 0.34, 14.0 }, { base[1] + 0.10, base[2] + 0.10, base[3] + 0.10 }, 0.86, rockTexture, 1.4)
    end
end

function corona_platformer_queue_fantasy_bush(x, y, z, width, variant)
    local colorA = variant % 2 == 0 and { 0.24, 0.58, 0.25 } or { 0.28, 0.64, 0.30 }
    local colorB = { colorA[1] + 0.08, colorA[2] + 0.10, colorA[3] + 0.06 }
    local leafTexture = corona_platformer_texture_kind("leaf_bush", "leaf")
    corona_platformer_queue_background_box("PlatformerBushBase", { x, y + 13.0, z }, { width, 26.0, 12.0 }, colorA, 0.88, leafTexture, 2.4)
    corona_platformer_queue_background_box("PlatformerBushTopA", { x - width * 0.18, y + 28.0, z - 1.0 }, { width * 0.52, 24.0, 11.0 }, colorB, 0.84, leafTexture, 1.8)
    if controls.backgroundDetail ~= "mobile" then
        corona_platformer_queue_background_box("PlatformerBushTopB", { x + width * 0.22, y + 24.0, z - 1.0 }, { width * 0.48, 22.0, 11.0 }, colorB, 0.84, leafTexture, 1.8)
    end
end

function corona_platformer_queue_fantasy_background_segment(backdropX, backdropWidth, backdropIndex)
    local variant = backdropIndex + currentLevel
    local highDetail = controls.backgroundDetail == "high"
    local mediumDetail = controls.backgroundDetail == "medium" or highDetail
    -- Backdrop: only sky-colored panels (no grass fill behind platforms).
    -- Far hills and bushes were perceived as visual noise — strip them out.
    local skyColor = { 0.62, 0.84, 1.0 }
    local skyTextureKey = "" -- solid sky, no texture
    corona_platformer_queue_background_box("PlatformerFantasySky", { backdropX, 178.0, 184.0 }, { backdropWidth + 28.0, 420.0, 16.0 }, skyColor, 0.98, skyTextureKey, 1.0)
    corona_platformer_queue_background_box("PlatformerFantasyHorizonGlow", { backdropX, -10.0, 172.0 }, { backdropWidth + 30.0, 220.0, 14.0 }, { 0.86, 0.92, 1.0 }, 0.96, skyTextureKey, 1.0)

    -- Sprinkle clouds across the backdrop.
    corona_platformer_queue_fantasy_cloud(backdropX - 255.0, 280.0 + (variant % 2) * 18.0, 126.0, 154.0, variant)
    corona_platformer_queue_fantasy_cloud(backdropX + 95.0, 220.0 + (variant % 3) * 10.0, 130.0, 132.0, variant + 1)
    corona_platformer_queue_fantasy_cloud(backdropX + 305.0, 320.0 + (variant % 4) * 14.0, 128.0, 138.0, variant + 5)
    if mediumDetail then
        corona_platformer_queue_fantasy_cloud(backdropX - 95.0, 360.0 + (variant % 2) * 12.0, 132.0, 96.0, variant + 7)
    end
    if highDetail then
        corona_platformer_queue_fantasy_cloud(backdropX + 180.0, 380.0 + (variant % 3) * 8.0, 134.0, 112.0, variant + 9)
    end
end

local function build_level()
    reset_level_streaming()
    if controls.platformerBackgroundGeometry == true then
        local backdropWidth = 900.0
        local backdropX = firstPlayableX + backdropWidth * 0.5
        local backdropIndex = 0
        while backdropX < levelEndX + backdropWidth do
            corona_platformer_queue_fantasy_background_segment(backdropX, backdropWidth, backdropIndex)
            backdropX = backdropX + backdropWidth
            backdropIndex = backdropIndex + 1
        end
    end

    if currentLayout ~= nil then
        queue_layout_blocks(currentLayout.floors, { 0.27, 0.38, 0.32 }, "floor")
        queue_layout_blocks(currentLayout.platforms, { 0.28, 0.39, 0.43 }, "platform")
        queue_layout_blocks(currentLayout.obstacles, { 0.42, 0.32, 0.24 }, "obstacle")
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
                { tint, tint + 0.08, tint + 0.04 },
                "floor")
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
                { 0.35, 0.43, 0.45 },
                "platform")
        end
    end

    queue_streaming_platform(
        startX,
        0.0,
        math.max(260.0, controls.startPlatformWidth or 540.0),
        36.0,
        platformDepth,
        { 0.34, 0.46, 0.36 },
        "floor")
    queue_streaming_platform(firstPlayableX + 50.0, 96.0, 38.0, 190.0, platformDepth, { 0.26, 0.32, 0.29 }, "obstacle")
    queue_streaming_platform(startX + 410.0, 56.0, 92.0, 74.0, platformDepth, { 0.29, 0.37, 0.39 }, "obstacle")
    queue_streaming_box("PlatformerGoalTrigger", { goalX, 28.0, characterZ + 12.0 }, { 170.0, 20.0, 92.0 }, { 0.92, 0.72, 0.20 }, 0.52, true, {
        goalTrigger = true,
    })
    queue_goal_flag()
    spawn_goal_spine_effects()
    spawn_layout_spine_effects()
    corona.log(
        "platformer level zones=" .. tostring(#levelZones) ..
        " zone_width=" .. tostring(levelZoneWidth) ..
        " load_mode=" .. (levelStreamingEnabled and "camera_streaming" or "full_level"))
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
    local ok = set_character_spine_pose(character, handle, player.x, player_spine_y(), characterZ, 0.0, playerVisualExtent, 0.82, 0.0, false, controls.playerSpineRayTracing == true)
    if not ok then
        corona.Entity.destroy(character)
        character = nil
        return
    end
    add_spawned(character)
end

local function spawn_camera_and_light()
    cameraEntity = add_spawned(corona.Entity.create("PlatformerCamera"))
    local focusX = player.x
    local focusY = player.y + (controls.cameraVerticalOffset or 112.0)
    local cameraDistance = controls.cameraDistance or 2200.0
    corona.CameraComponent.add(cameraEntity, {
        position = { focusX, focusY, -cameraDistance },
        look_at = { focusX, focusY, characterZ },
        up = { 0.0, 1.0, 0.0 },
        fov = controls.cameraFov or 0.24,
        near_plane = 5.0,
        far_plane = 12000.0,
        active = true,
    })

    sunEntity = add_spawned(corona.Entity.create("PlatformerSun"))
    corona.LightComponent.add(sunEntity, {
        type = "directional",
        enabled = true,
        direction = corona_platformer_sun_direction,
        color = corona_platformer_sun_color,
        intensity = corona_platformer_sun_intensity,
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
            desiredX = x,
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
        enemy.baseX = enemy.desiredX or enemy.baseX
        enemy.x = enemy.baseX
        enemy.y = 18.0
        enemy.spawnPlatform = nil
        local assetIndex = enemy.assetIndex or index
        enemy.vx = (((assetIndex + currentLevel) % 2 == 0) and -1.0 or 1.0) * (38.0 + ((assetIndex + currentLevel) % 6) * 7.0)
        enemy.facing = enemy.vx >= 0.0 and 1.0 or -1.0
        corona_platformer_rebuild_enemy_patrol(enemy, enemy.baseX, nil)
        enemy.alive = true
    end
end

local function sync_enemy_source_extent(enemy, handle)
    local extent = character_visual_extent_from_scene(handle)
    enemy.targetExtent = extent
    enemy.hitRadius = math.max(34.0, extent * 0.34)
    enemy.hitHeight = math.max(72.0, extent * 0.84)
end

function corona_platformer_enemy_spine_y(enemy)
    return enemy.y
end

function corona_platformer_enemy_render_z(enemy)
    local assetIndex = enemy ~= nil and (enemy.assetIndex or 0) or 0
    return enemyZ - (assetIndex % 9) * spriteEntityDepthStep
end

local function spawn_enemy_entity(enemy)
    local handle = select(1, get_spine_scene(enemy.asset.path, "idle", enemy.phase, enemySampleFps, nil, true))
    if handle == nil then
        return false
    end
    sync_enemy_source_extent(enemy, handle)
    corona_platformer_snap_enemy_to_safe_spawn(enemy)

    enemy.entity = corona.Entity.create("PlatformerEnemy")
    local ok = set_character_spine_pose(
        enemy.entity,
        handle,
        enemy.x,
        corona_platformer_enemy_spine_y(enemy),
        corona_platformer_enemy_render_z(enemy),
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
    combat.maybe_spawn_ranged_ammo_drop(enemy)
    combat.maybe_spawn_health_drop(enemy)
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

local function projectile_hits_platform(projectile, previousX, previousY)
    if projectile == nil then
        return nil
    end

    local radius = math.max(1.0, projectile.radius or 1.0)
    local currentX = projectile.x
    local currentY = projectile.y
    local deltaX = currentX - previousX
    local deltaY = currentY - previousY
    local bestHit = nil

    local function clip_axis(startValue, deltaValue, minValue, maxValue, range)
        if math.abs(deltaValue) < 0.0001 then
            return startValue >= minValue and startValue <= maxValue
        end

        local invDelta = 1.0 / deltaValue
        local enter = (minValue - startValue) * invDelta
        local exit = (maxValue - startValue) * invDelta
        if enter > exit then
            local temp = enter
            enter = exit
            exit = temp
        end

        if enter > range.enter then
            range.enter = enter
        end
        if exit < range.exit then
            range.exit = exit
        end
        return range.enter <= range.exit
    end

    for _, platform in ipairs(platformData) do
        local minX = platform.left - radius
        local maxX = platform.right + radius
        local minY = platform.bottom - radius
        local maxY = platform.top + radius
        local range = { enter = 0.0, exit = 1.0 }

        if clip_axis(previousX, deltaX, minX, maxX, range) and clip_axis(previousY, deltaY, minY, maxY, range) then
            if range.exit >= 0.0 and range.enter <= 1.0 then
                local hitT = clamp(range.enter, 0.0, 1.0)
                if bestHit == nil or hitT < bestHit.t then
                    bestHit = {
                        t = hitT,
                        x = previousX + deltaX * hitT,
                        y = previousY + deltaY * hitT,
                        platform = platform,
                    }
                end
            end
        end
    end

    return bestHit
end

combat.update_projectiles = function(dt)
    for index = #combat.projectiles, 1, -1 do
        local projectile = combat.projectiles[index]
        projectile.elapsed = projectile.elapsed + dt
        local previousX = projectile.x
        local previousY = projectile.y
        projectile.x = projectile.x + projectile.vx * dt
        projectile.y = projectile.y + projectile.vy * dt

        if projectile.effect ~= nil then
            projectile.effect.x = projectile.x
            projectile.effect.y = projectile.y
            projectile.effect.facing = projectile.facing
        end

        local removeProjectile = projectile.elapsed >= projectile.lifetime
        if not removeProjectile then
            local platformHit = projectile_hits_platform(projectile, previousX, previousY)
            if platformHit ~= nil then
                spawn_spine_effect("hit", platformHit.x, platformHit.y, {
                    z = spineEffectZ,
                    facing = projectile.facing,
                    targetExtent = math.max(72.0, projectile.radius * 2.2),
                    duration = 0.22,
                })
                removeProjectile = true
            end
        end

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
        set_character_spine_pose(enemy.entity, nil, enemy.x, corona_platformer_enemy_spine_y(enemy), corona_platformer_enemy_render_z(enemy), yaw, enemy.targetExtent, 0.84, 0.0, mirrorX)
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
    set_character_spine_pose(enemy.entity, sceneHandle, enemy.x, corona_platformer_enemy_spine_y(enemy), corona_platformer_enemy_render_z(enemy), yaw, enemy.targetExtent, 0.84, 0.0, mirrorX)
end

function corona_platformer_actor_overlaps_platform(x, y, halfWidth, height, platform)
    return
        x + halfWidth > platform.left and
        x - halfWidth < platform.right and
        y + height > platform.bottom and
        y < platform.top
end

function corona_platformer_resolve_actor_platform_collision(x, y, previousX, previousY, halfWidth, height, platform)
    if not corona_platformer_actor_overlaps_platform(x, y, halfWidth, height, platform) then
        return nil, x, y
    end

    local previousLeft = previousX - halfWidth
    local previousRight = previousX + halfWidth
    local previousBottom = previousY
    local previousTop = previousY + height

    if previousBottom >= platform.top and y <= platform.top then
        return "top", x, platform.top
    end
    if previousTop <= platform.bottom and y + height >= platform.bottom then
        return "bottom", x, platform.bottom - height
    end
    if previousRight <= platform.left and x + halfWidth > platform.left then
        return "left", platform.left - halfWidth, y
    end
    if previousLeft >= platform.right and x - halfWidth < platform.right then
        return "right", platform.right + halfWidth, y
    end

    local leftPenetration = (x + halfWidth) - platform.left
    local rightPenetration = platform.right - (x - halfWidth)
    local bottomPenetration = (y + height) - platform.bottom
    local topPenetration = platform.top - y
    local face = "left"
    local penetration = leftPenetration

    if rightPenetration < penetration then
        face = "right"
        penetration = rightPenetration
    end
    if bottomPenetration < penetration then
        face = "bottom"
        penetration = bottomPenetration
    end
    if topPenetration < penetration then
        face = "top"
        penetration = topPenetration
    end

    if face == "left" then
        return face, platform.left - halfWidth, y
    elseif face == "right" then
        return face, platform.right + halfWidth, y
    elseif face == "bottom" then
        return face, x, platform.bottom - height
    end
    return face, x, platform.top
end

function corona_platformer_resolve_player_platform_collisions(previousX, previousY)
    player.grounded = false
    player.groundPlatform = nil

    for _ = 1, 4 do
        local resolvedAny = false
        for _, platform in ipairs(platformData) do
            local face, resolvedX, resolvedY = corona_platformer_resolve_actor_platform_collision(
                player.x,
                player.y,
                previousX,
                previousY,
                playerRadius,
                playerHeight,
                platform)
            if face ~= nil then
                player.x = clamp(resolvedX, worldMinX, levelEndX)
                player.y = resolvedY
                resolvedAny = true

                if face == "top" then
                    if player.vy <= 0.0 then
                        player.vy = 0.0
                    end
                    player.grounded = true
                    player.groundPlatform = platform
                elseif face == "bottom" then
                    if player.vy > 0.0 then
                        player.vy = 0.0
                    end
                elseif face == "left" then
                    if player.vx > 0.0 then
                        player.vx = 0.0
                    end
                elseif face == "right" and player.vx < 0.0 then
                    player.vx = 0.0
                end
            end
        end

        if not resolvedAny then
            break
        end
    end
end

function corona_platformer_resolve_enemy_platform_collisions(enemy, previousX, previousY)
    local halfWidth = math.max(10.0, enemy.hitRadius * 0.82)
    local height = math.max(32.0, enemy.hitHeight * 0.92)
    local hitSide = false

    for _ = 1, 3 do
        local resolvedAny = false
        for _, platform in ipairs(platformData) do
            local face, resolvedX, resolvedY = corona_platformer_resolve_actor_platform_collision(
                enemy.x,
                enemy.y,
                previousX,
                previousY,
                halfWidth,
                height,
                platform)
            if face ~= nil then
                enemy.x = resolvedX
                enemy.y = resolvedY
                resolvedAny = true

                if face == "left" then
                    enemy.vx = -math.abs(enemy.vx)
                    enemy.facing = -1.0
                    hitSide = true
                elseif face == "right" then
                    enemy.vx = math.abs(enemy.vx)
                    enemy.facing = 1.0
                    hitSide = true
                end
            end
        end

        if not resolvedAny then
            break
        end
    end

    return hitSide
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
                    local enemyPreviousX = enemy.x
                    local enemyPreviousY = enemy.y
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
                    corona_platformer_resolve_enemy_platform_collisions(enemy, enemyPreviousX, enemyPreviousY)

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

function corona_platformer_update_input(dt)
    local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
    if corona.was_key_pressed("C") then
        corona_platformer_collision_debug_enabled = not corona_platformer_collision_debug_enabled
        corona_platformer_mark_collision_debug_dirty()
    end

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

    -- Jump triggers: keyboard (SPACE/W/UP), gamepad A/X face buttons, and
    -- the dedicated bottom-right virtual jump pad on mobile (mapped to the
    -- gamepad A face button by the mobile touch handler). Upward stick
    -- input no longer doubles as jump per request.
    local gamepadJumpHeld =
        gamepad.connected and (
            gamepad.a == true or
            gamepad.x == true)
    local jumpPressed =
        corona.was_key_pressed("SPACE") or
        corona.was_key_pressed("W") or
        corona.was_key_pressed("UP") or
        (gamepad.connected and (gamepad.a_pressed or gamepad.x_pressed)) or
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

function corona_platformer_collide_platforms(previousX, previousY)
    corona_platformer_resolve_player_platform_collisions(previousX, previousY)
end

function corona_platformer_update_goal(dt)
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

function corona_platformer_compute_camera_focus()
    local lead = clamp(player.vx * 0.28, -85.0, 110.0)
    local minFocusX = math.min(worldMinX + 170.0, levelEndX - 170.0)
    local maxFocusX = math.max(worldMinX + 170.0, levelEndX - 170.0)
    local focusX = clamp(player.x + lead, minFocusX, maxFocusX)
    local focusY = clamp(player.y + (controls.cameraVerticalOffset or 112.0), 94.0, 292.0)
    return focusX, focusY
end

function corona_platformer_camera_frustum_bounds()
    local focusX, focusY = corona_platformer_compute_camera_focus()
    cameraFocusX = focusX
    cameraFocusY = focusY

    local cameraDepth = controls.cameraDistance or 2200.0
    local fov = controls.cameraFov or 0.24
    local aspect = 16.0 / 9.0
    local halfY = math.tan(fov * 0.5) * cameraDepth
    local halfX = halfY * aspect
    return focusX - halfX, focusX + halfX, focusY - halfY, focusY + halfY
end

update_level_streaming = function(force)
    if not levelStreamingEnabled then
        corona_platformer_refresh_collision_debug()
        return
    end

    local minX, maxX, minY, maxY = corona_platformer_camera_frustum_bounds()
    local unloadMargin = math.max(0.0, controls.streamingUnloadMargin or 980.0)
    for _, zone in pairs(levelZones) do
        if (zone.loaded or zone.loading) and not zone_overlaps_frustum(zone, minX, maxX, minY, maxY, unloadMargin) then
            unload_level_zone(zone)
        end
    end

    local objectBudget = math.max(1, math.floor(force and (controls.maxForcedLevelZoneObjectsPerFrame or 18) or (controls.maxLevelZoneObjectsPerFrame or 5)))
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

    local initialZoneLoads = math.max(1, math.floor(controls.maxInitialZoneLoads or 2))
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
    corona_platformer_refresh_collision_debug()
end

function update_camera()
    if cameraEntity == nil then
        return
    end
    local focusX, focusY = corona_platformer_compute_camera_focus()
    cameraFocusX = focusX
    cameraFocusY = focusY
    corona.CameraComponent.set(cameraEntity, {
        position = { focusX, focusY, -(controls.cameraDistance or 2200.0) },
        look_at = { focusX, focusY, characterZ },
        up = { 0.0, 1.0, 0.0 },
        active = true,
    })
end

destroy_level_entities = function()
    corona_platformer_destroy_collision_debug_entities()
    corona_platformer_mark_collision_debug_dirty()
    for _, zone in pairs(levelZones) do
        unload_level_zone(zone)
    end
    reset_level_streaming()
    combat.clear_projectiles()
    combat.clear_pickups()
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

function corona_platformer_advance_level()
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
        " start_x=" .. tostring(startX) ..
        " goal_x=" .. tostring(goalX) ..
        " direction=screen_left_to_right" ..
        " world_progress=" .. tostring(corona_platformer_progress_world_direction()) ..
        " layout=" .. layoutText)
end

start_new_game = function()
    refresh_vitals_config()
    lives = maxLives
    health = maxHealth
    combat.rangedAmmo = clamp(math.floor(controls.startingRangedAmmo or 0), 0, combat.rangedAmmoMax)
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

    corona_platformer_update_input(dt)
    update_level_streaming(false)
    if gameCompleted then
        combat.update_projectiles(dt)
        combat.update_pickups(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        return
    end

    if player.won and levelAdvanceTimer > 0.0 then
        levelAdvanceTimer = math.max(0.0, levelAdvanceTimer - dt)
        corona_platformer_update_goal(dt)
        combat.update_projectiles(dt)
        combat.update_pickups(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        if levelAdvanceTimer <= 0.0 then
            corona_platformer_advance_level()
        end
        return
    end

    local wasGrounded = player.grounded
    local previousX = player.x
    local previousY = player.y
    player.vy = math.max(player.vy + gravity * dt, maxFallSpeed)
    local fallingVelocity = player.vy
    player.x = clamp(player.x + player.vx * dt, worldMinX, levelEndX)
    player.y = player.y + player.vy * dt

    corona_platformer_collide_platforms(previousX, previousY)
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
        combat.update_pickups(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        return
    end

    corona_platformer_update_goal(dt)
    if player.won and levelAdvanceTimer > 0.0 then
        combat.update_projectiles(dt)
        combat.update_pickups(dt)
        update_spine_effects(dt)
        update_character(false)
        update_camera()
        update_level_streaming(false)
        return
    end

    combat.update_projectiles(dt)
    combat.update_pickups(dt)
    update_enemies(dt, previousY, fallingVelocity)
    update_spine_effects(dt)
    update_character(false)
    update_camera()
    update_level_streaming(false)
    corona_platformer_process_effect_prewarm_queue()
end

function corona_platformer_draw_menu()
    local title = "PLATFORMER"
    if menuReason == "game_over" then
        title = "GAME OVER"
    elseif menuReason == "clear" then
        title = "ALL CLEAR"
    end

    local uiState = corona.get_ui_state ~= nil and corona.get_ui_state() or {}
    local screenWidth = uiState.window_width or uiState.display_width or 1280.0
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

function corona_platformer_draw_top_hud()
    if ui.overlay_rect_filled == nil or ui.overlay_text == nil then
        return
    end

    local uiState = corona.get_ui_state ~= nil and corona.get_ui_state() or {}
    local screenWidth = uiState.display_width or uiState.window_width or 1280.0
    local panelWidth = math.max(560.0, math.min(screenWidth - 32.0, 1040.0))
    local panelX = (screenWidth - panelWidth) * 0.5
    local panelY = 14.0
    local panelHeight = screenWidth < 860.0 and 128.0 or 86.0
    local progressDirection = corona_platformer_progress_world_direction()
    local travel = (goalX - startX) * progressDirection
    local progress = 0.0
    if math.abs(travel) > 0.001 then
        progress = clamp(((player.x - startX) * progressDirection) / travel, 0.0, 1.0)
    end

    ui.overlay_rect_filled(panelX + 3.0, panelY + 4.0, panelWidth, panelHeight, { 0.0, 0.0, 0.0, 0.30 }, 8.0)
    ui.overlay_rect_filled(panelX, panelY, panelWidth, panelHeight, { 0.030, 0.042, 0.050, 0.82 }, 8.0)
    ui.overlay_rect(panelX, panelY, panelWidth, panelHeight, { 0.88, 0.72, 0.36, 0.78 }, 1.4, 8.0)
    ui.overlay_rect_filled(panelX + 12.0, panelY + 9.0, panelWidth - 24.0, 3.0, { 1.0, 0.84, 0.38, 0.50 }, 2.0)

    local leftX = panelX + 22.0
    local topY = panelY + 21.0
    ui.overlay_text("ARES", leftX, topY - 5.0)
    ui.overlay_progress_bar(
        "platformer.hud.hp",
        leftX + 62.0,
        topY - 2.0,
        208.0,
        18.0,
        health / maxHealth,
        "HP " .. tostring(health) .. "/" .. tostring(maxHealth),
        { 0.15, 0.88, 0.28, 0.96 },
        { 0.12, 0.16, 0.15, 0.78 },
        { 0.72, 0.96, 0.74, 0.86 })

    ui.overlay_text("LIVES", leftX, topY + 29.0)
    for index = 1, maxLives do
        local slotX = leftX + 66.0 + (index - 1) * 25.0
        local alive = index <= lives
        ui.overlay_rect_filled(slotX, topY + 30.0, 18.0, 14.0, alive and { 0.90, 0.18, 0.18, 0.94 } or { 0.20, 0.20, 0.22, 0.80 }, 3.0)
        ui.overlay_rect(slotX, topY + 30.0, 18.0, 14.0, alive and { 1.0, 0.70, 0.42, 0.86 } or { 0.45, 0.45, 0.48, 0.62 }, 1.0, 3.0)
    end
    ui.overlay_text(tostring(lives) .. "/" .. tostring(maxLives), leftX + 202.0, topY + 27.0)

    local centerX = panelX + panelWidth * 0.5 - 165.0
    local centerY = topY - 3.0
    if screenWidth < 860.0 then
        centerX = panelX + 22.0
        centerY = panelY + 73.0
    end
    ui.overlay_text("LEVEL " .. tostring(currentLevel) .. "/" .. tostring(levelCount), centerX, centerY - 2.0)
    ui.overlay_progress_bar(
        "platformer.hud.progress",
        centerX,
        centerY + 23.0,
        330.0,
        16.0,
        progress,
        "GOAL " .. tostring(math.floor(progress * 100.0 + 0.5)) .. "%",
        { 0.94, 0.68, 0.22, 0.95 },
        { 0.12, 0.13, 0.16, 0.78 },
        { 0.94, 0.88, 0.62, 0.84 })
    ui.overlay_rect_filled(centerX + 330.0 * progress - 3.0, centerY + 19.0, 6.0, 24.0, { 1.0, 0.96, 0.50, 0.94 }, 2.0)
    ui.overlay_text("ENEMIES " .. tostring(defeatedEnemyCount) .. "/" .. tostring(currentLevelEnemyTotal), centerX + 118.0, centerY + 44.0)

    local ammoCardWidth = 158.0
    local ammoX = panelX + panelWidth - ammoCardWidth - 22.0
    local ammoY = topY - 5.0
    local ammo = combat.rangedAmmo or 0
    local ready = ammo > 0
    local frameColor = ready and { 0.95, 0.56, 0.18, 0.88 } or { 0.46, 0.46, 0.48, 0.72 }
    local coreColor = ready and { 1.0, 0.34, 0.08, 0.92 } or { 0.28, 0.28, 0.30, 0.82 }
    local flameColor = ready and { 1.0, 0.78, 0.20, 0.94 } or { 0.40, 0.40, 0.42, 0.82 }

    ui.overlay_rect_filled(ammoX, ammoY, ammoCardWidth, 52.0, { 0.025, 0.027, 0.030, 0.70 }, 8.0)
    ui.overlay_rect(ammoX, ammoY, ammoCardWidth, 52.0, frameColor, 1.4, 8.0)
    ui.overlay_rect_filled(ammoX + 16.0, ammoY + 21.0, 22.0, 18.0, coreColor, 3.0)
    ui.overlay_rect_filled(ammoX + 23.0, ammoY + 10.0, 10.0, 22.0, flameColor, 3.0)
    ui.overlay_rect_filled(ammoX + 27.0, ammoY + 28.0, 8.0, 8.0, { 1.0, 0.94, 0.50, ready and 0.92 or 0.32 }, 2.0)
    ui.overlay_text("FIRE", ammoX + 54.0, ammoY + 7.0)
    ui.overlay_text("x " .. tostring(ammo), ammoX + 54.0, ammoY + 27.0)

    if corona_platformer_collision_debug_enabled then
        ui.overlay_text("COLLISION DEBUG", panelX + panelWidth - 170.0, panelY + panelHeight + 8.0)
    end
end

function script.ui()
    if gameState == "menu" then
        corona_platformer_draw_menu()
        return
    end

    if not controls.showHud then
        return
    end

    corona_platformer_draw_top_hud()

    if gameCompleted or player.dead or player.won then
        local uiState = corona.get_ui_state ~= nil and corona.get_ui_state() or {}
        local screenWidth = uiState.display_width or uiState.window_width or 1280.0
        local status = gameCompleted and "ALL CLEAR" or (player.dead and (lives <= 0 and "GAME OVER" or "DOWN") or "LEVEL CLEAR")
        local approxWidth = string.len(status) * 8.0
        ui.overlay_rect_filled(screenWidth * 0.5 - approxWidth * 0.5 - 22.0, 112.0, approxWidth + 44.0, 34.0, { 0.03, 0.035, 0.04, 0.78 }, 8.0)
        ui.overlay_rect(screenWidth * 0.5 - approxWidth * 0.5 - 22.0, 112.0, approxWidth + 44.0, 34.0, { 0.94, 0.74, 0.32, 0.78 }, 1.2, 8.0)
        ui.overlay_text(status, screenWidth * 0.5 - approxWidth * 0.5, 121.0)
    end
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
