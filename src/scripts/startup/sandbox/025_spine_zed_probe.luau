local script = {}

corona_script_controls = corona_script_controls or {}
if corona_script_controls.platformer ~= nil and corona_script_controls.platformer.enabled then
    corona.log("startup spine zed probe skipped for platformer mode")
    return {}
end
corona_script_controls.spine = corona_script_controls.spine or {}

local controls = corona_script_controls.spine

local function persistent_default(name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control("spine." .. name, value)
    end
    return value
end

local function set_default(name, value)
    if controls[name] == nil then
        controls[name] = persistent_default(name, value)
    end
end

set_default("enabled", true)
set_default("asset", "assets/spine/zed/zed.json")
set_default("animation", "idle")
set_default("loopSeconds", 2.0)
set_default("sampleFps", 12.0)
set_default("position", { -150.0, -12.0, -72.0 })
set_default("yaw", 0.0)
set_default("extent", 150.0)
set_default("sourceScale", 1.0)
set_default("spawned", false)
set_default("showLabel", true)

local entity = nil
local sceneCache = {}
local cacheKey = ""
local lastFrame = -1
local lastUpdateKey = ""
local time = 0.0
local loggedBindingMissing = false

local function make_cache_key()
    return table.concat({
        controls.asset or "",
        controls.animation or "",
        tostring(controls.loopSeconds or 0.0),
        tostring(controls.sampleFps or 0.0),
        tostring(controls.sourceScale or 1.0),
    }, "|")
end

local function reset_cache_if_needed()
    local nextKey = make_cache_key()
    if nextKey ~= cacheKey then
        sceneCache = {}
        cacheKey = nextKey
        lastFrame = -1
    end
end

local function create_scene(sampleTime)
    if corona.SpineComponent == nil or corona.SpineComponent.create_scene == nil then
        if not loggedBindingMissing then
            corona.log("SpineComponent binding is not available")
            loggedBindingMissing = true
        end
        return nil
    end

    local ok, handle = pcall(corona.SpineComponent.create_scene, {
        path = controls.asset,
        animation = controls.animation,
        time = sampleTime,
        source_scale = controls.sourceScale,
    })
    if not ok then
        corona.log("SpineComponent.create_scene failed: " .. tostring(handle))
        return nil
    end
    return handle
end

local function get_frame_scene(frameIndex)
    reset_cache_if_needed()
    local handle = sceneCache[frameIndex]
    if handle ~= nil then
        return handle
    end

    local sampleFps = math.max(1.0, controls.sampleFps or 12.0)
    handle = create_scene((frameIndex - 1) / sampleFps)
    sceneCache[frameIndex] = handle
    return handle
end

local function current_frame()
    local loopSeconds = math.max(0.1, controls.loopSeconds or 2.0)
    local sampleFps = math.max(1.0, controls.sampleFps or 12.0)
    local frameCount = math.max(1, math.floor(loopSeconds * sampleFps + 0.5))
    return math.floor((time % loopSeconds) * sampleFps) % frameCount + 1
end

local function spawn_spine()
    if entity ~= nil then
        return
    end

    local handle = get_frame_scene(1)
    if handle == nil then
        controls.spawned = false
        return
    end

    entity = corona.Entity.create("SpineZed")
    local ok = corona.MeshComponent.add(entity, {
        scene_handle = handle,
        position = controls.position,
        rotation = { 0.0, controls.yaw or 0.0, 0.0 },
        target_extent = controls.extent,
        roughness = 0.82,
        metallic = 0.0,
        ray_tracing = false,
        physics = false,
    })
    if not ok then
        corona.Entity.destroy(entity)
        entity = nil
    end
    controls.spawned = entity ~= nil
end

local function remove_spine()
    if entity ~= nil then
        corona.Entity.destroy(entity)
        entity = nil
    end
    controls.spawned = false
    lastFrame = -1
end

local function update_spine()
    if controls.enabled then
        spawn_spine()
    else
        remove_spine()
        return
    end

    if entity == nil then
        return
    end

    local frame = current_frame()
    local transformKey = table.concat({
        tostring((controls.position or {})[1] or 0.0),
        tostring((controls.position or {})[2] or 0.0),
        tostring((controls.position or {})[3] or 0.0),
        tostring(controls.yaw or 0.0),
        tostring(controls.extent or 150.0),
    }, "|")

    if frame ~= lastFrame or transformKey ~= lastUpdateKey then
        local handle = get_frame_scene(frame)
        if handle ~= nil then
            corona.MeshComponent.set(entity, {
                scene_handle = handle,
                position = controls.position,
                rotation = { 0.0, controls.yaw or 0.0, 0.0 },
                target_extent = controls.extent,
                ray_tracing = false,
                physics = false,
            })
            lastFrame = frame
            lastUpdateKey = transformKey
        end
    end
end

update_spine()
corona.log("startup spine zed probe script loaded")

function script.update(dt)
    time = time + dt
    update_spine()
end

function script.ui()
    if controls.showLabel and entity ~= nil and controls.position ~= nil then
        ui.world_text(
            "spine.zed.label",
            { controls.position[1] or 0.0, (controls.position[2] or 0.0) + (controls.extent or 150.0) + 12.0, controls.position[3] or 0.0 },
            "Spine zed / " .. tostring(controls.animation or "idle"),
            0.0,
            0.0,
            { 0.78, 0.93, 1.0, 0.95 })
    end
end

return script
