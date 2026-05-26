-- Standalone Spine skinning benchmark scene.
--
-- Loaded only when startup_mode = "spine_benchmark". Pure benchmark setup:
-- one camera, a directional sun, and N spine characters packed into a
-- grid that fits inside the camera frustum. No platformer mechanics, no
-- level streaming, no enemies, no input.
--
-- Selected by the `--spine-benchmark` CLI flag, which also pushes the
-- character count override `spine_benchmark.characterCount`.

corona_script_controls = corona_script_controls or {}
corona_script_controls.spine_benchmark = corona_script_controls.spine_benchmark or {}
local controls = corona_script_controls.spine_benchmark

local function read_number(name, fallback)
    local override = nil
    if corona.get_persistent_control ~= nil then
        override = corona.get_persistent_control("spine_benchmark." .. name, fallback)
    end
    if type(override) == "number" then
        controls[name] = override
        return override
    end
    local v = controls[name]
    if type(v) == "number" then return v end
    return fallback
end

local requestedCount = math.max(1, math.min(4000, math.floor(read_number("characterCount", 50))))
local cameraDistance = read_number("cameraDistance", 2200.0)
local cameraFov = read_number("cameraFov", 0.4)
local sampleFps = read_number("sampleFps", 12.0)
local cameraOriginX = read_number("cameraOriginX", 0.0)
local cameraOriginY = read_number("cameraOriginY", 0.0)
local loopSeconds = read_number("loopSeconds", 1.0)
local characterZ = 0.0

local visibleHalfHeight = cameraDistance * math.tan(cameraFov * 0.5)
local visibleAreaH = visibleHalfHeight * 1.8
local visibleAreaW = visibleAreaH * (16.0 / 9.0)

local autoColumns = math.max(1, math.ceil(math.sqrt(requestedCount * (visibleAreaW / visibleAreaH))))
local columns = math.max(1, math.min(requestedCount, autoColumns))
local rows = math.max(1, math.ceil(requestedCount / columns))
local spacingX = visibleAreaW / math.max(1, columns - 1)
local spacingY = visibleAreaH / math.max(1, rows - 1)
local legacySpacingX = 138.0
local legacySpacingY = 132.0
local scaleShrink = math.min(1.0, math.min(spacingX / legacySpacingX, spacingY / legacySpacingY))
local worldScale = 0.42 * scaleShrink

local gridWidth = (columns - 1) * spacingX
local gridHeight = (rows - 1) * spacingY
local baseX = cameraOriginX - gridWidth * 0.5
local baseY = cameraOriginY - gridHeight * 0.5

local benchmarkAssets = {
    { path = "assets/spine/chars/cody/cody.json" },
    { path = "assets/spine/chars/cosmo/cosmo.json" },
    { path = "assets/spine/chars/markus/markus.json" },
    { path = "assets/spine/chars/mason/mason.json" },
    { path = "assets/spine/chars/henry/henry.json" },
    { path = "assets/spine/chars/scott/scott.json" },
    { path = "assets/spine/chars/peach/peach.json" },
    { path = "assets/spine/chars/peter/peter.json" },
    { path = "assets/spine/chars/skylar/skylar.json" },
}

local cameraEntity = corona.Entity.create("SpineBenchCamera")
corona.CameraComponent.add(cameraEntity, {
    position = { cameraOriginX, cameraOriginY, -cameraDistance },
    look_at = { cameraOriginX, cameraOriginY, characterZ },
    up = { 0.0, 1.0, 0.0 },
    fov = cameraFov,
    near_plane = 5.0,
    far_plane = 12000.0,
    active = true,
})

local sunEntity = corona.Entity.create("SpineBenchSun")
corona.LightComponent.add(sunEntity, {
    type = "directional",
    enabled = true,
    direction = { -0.4, -1.0, 0.3 },
    color = { 1.0, 0.96, 0.88 },
    intensity = 1.4,
    persistent = false,
})

local create_scene = nil
local set_pose = nil
if corona.SpineComponent ~= nil then
    create_scene = corona.SpineComponent.create_scene
    set_pose = corona.SpineComponent.set_pose
end

if create_scene == nil or set_pose == nil then
    corona.log("[spine_benchmark] corona.SpineComponent.create_scene / set_pose unavailable")
    return
end

-- Pre-cache animation frames per asset. The Spine scene is keyed by
-- (path, animation, sampleTime), so call create_scene at fps quantized
-- timestamps once up front; every tick the update loop just picks the
-- nearest cached handle. This matches how the platformer benchmark
-- works (snapshot cache hit on every frame).
local frameCount = math.max(2, math.floor(loopSeconds * sampleFps + 0.5))
local function pre_cache_frames(assetPath, animation)
    local frames = {}
    for frameIndex = 0, frameCount - 1 do
        local sampleTime = frameIndex / sampleFps
        local ok, handle = pcall(create_scene, assetPath, animation, sampleTime, 1.0)
        frames[frameIndex] = (ok and handle) or nil
    end
    return frames
end

local cacheByAsset = {}
for _, asset in ipairs(benchmarkAssets) do
    local frames = pre_cache_frames(asset.path, "walk")
    if frames[0] == nil then
        frames = pre_cache_frames(asset.path, "idle")
    end
    cacheByAsset[asset.path] = frames
end

local actors = {}
local spawnedCount = 0
for index = 1, requestedCount do
    local asset = benchmarkAssets[((index - 1) % #benchmarkAssets) + 1]
    local frames = cacheByAsset[asset.path]
    if frames ~= nil and frames[0] ~= nil then
        local row = math.floor((index - 1) / columns)
        local column = (index - 1) % columns
        local phase = (index - 1) * 0.037
        local entity = corona.Entity.create("SpineBenchActor_" .. tostring(index))
        local x = baseX + column * spacingX
        local y = baseY + (rows - row - 1) * spacingY
        set_pose(entity, frames[0], x, y, characterZ, 0.0, worldScale, 0.72, 0.0, false, true, false)
        table.insert(actors, {
            entity = entity,
            frames = frames,
            phase = phase,
            x = x,
            y = y,
        })
        spawnedCount = spawnedCount + 1
    end
end

corona.log(string.format(
    "[spine_benchmark] spawned=%d requested=%d cols=%d rows=%d spacingX=%.2f spacingY=%.2f scale=%.4f gridW=%.1f gridH=%.1f cam=(%.1f,%.1f,%.1f) fov=%.3f frameCount=%d",
    spawnedCount, requestedCount, columns, rows, spacingX, spacingY, worldScale,
    gridWidth, gridHeight, cameraOriginX, cameraOriginY, -cameraDistance, cameraFov, frameCount))

local elapsedSeconds = 0.0
local function on_update(dt)
    elapsedSeconds = elapsedSeconds + dt
    for _, actor in ipairs(actors) do
        local t = (elapsedSeconds + actor.phase) % loopSeconds
        local frameIndex = math.floor(t * sampleFps + 0.0001) % frameCount
        local handle = actor.frames[frameIndex]
        if handle ~= nil then
            set_pose(actor.entity, handle, actor.x, actor.y, characterZ, 0.0, worldScale, 0.72, 0.0, false, true, false)
        end
    end
end

return { update = on_update }
