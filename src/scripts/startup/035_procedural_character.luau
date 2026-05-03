local script = {}

corona_script_controls = corona_script_controls or {}
corona_script_controls.blockCharacter = corona_script_controls.blockCharacter or {}

local controls = corona_script_controls.blockCharacter

local function persistent_default(name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control("block_character." .. name, value)
    end
    return value
end

local function persist_control(name, value)
    if corona.set_persistent_control ~= nil then
        corona.set_persistent_control("block_character." .. name, value)
    end
    return value
end

local function set_default(name, value)
    if controls[name] == nil then
        controls[name] = persistent_default(name, value)
    end
end

set_default("enabled", true)
set_default("position", { -28.0, -12.0, -48.0 })
set_default("targetExtent", 105.0)
set_default("baseYaw", 18.0)
set_default("pitch", 0.0)
set_default("roll", 0.0)
set_default("spinSpeed", 12.0)
set_default("bobHeight", 3.5)
set_default("bobSpeed", 1.15)
set_default("shapeSeed", 20260503)
if controls.spawned == nil then
    controls.spawned = false
end
if controls.forceRespawn == nil then
    controls.forceRespawn = false
end
if controls.wasDisabled == nil then
    controls.wasDisabled = not controls.enabled
end

local characterScene = nil
local characterSceneSeed = nil
local character = nil
local time = 0.0

local function ensure_character_scene()
    local seed = math.max(1, math.floor(controls.shapeSeed or 20260503))
    if characterScene ~= nil and characterSceneSeed == seed then
        return characterScene
    end

    characterScene = corona.create_block_character_scene({
        seed = seed,
    })
    characterSceneSeed = seed
    return characterScene
end

local function next_shape_seed()
    local seed = math.floor(controls.shapeSeed or 20260503) + 1
    if seed > 2147480000 then
        seed = 20260503
    end
    controls.shapeSeed = persist_control("shapeSeed", seed)
    return seed
end

local function current_rotation()
    local yaw = (controls.baseYaw + time * controls.spinSpeed) % 360.0
    return { controls.pitch or 0.0, yaw, controls.roll or 0.0 }
end

local function spawn_character()
    if character ~= nil or not controls.enabled then
        return
    end

    local scene = ensure_character_scene()
    character = corona.spawn(scene, {
        position = controls.position,
        rotation = current_rotation(),
        target_extent = controls.targetExtent,
        roughness = 0.72,
        metallic = 0.0,
        override_material = true,
        ray_tracing = true,
        physics = false,
    })
    controls.spawned = character ~= nil
end

local function remove_character()
    if character == nil then
        controls.spawned = false
        return
    end

    corona.destroy(character)
    character = nil
    controls.spawned = false
end

local function update_character()
    if controls.forceRespawn then
        controls.forceRespawn = false
        remove_character()
        if controls.enabled then
            spawn_character()
        end
    end

    if controls.enabled and character == nil then
        if controls.wasDisabled then
            next_shape_seed()
            controls.wasDisabled = false
        end
        spawn_character()
    elseif not controls.enabled and character ~= nil then
        remove_character()
    end
    if not controls.enabled then
        controls.wasDisabled = true
    end

    if character == nil then
        return
    end

    local position = controls.position
    local bob = math.sin(time * controls.bobSpeed) * controls.bobHeight
    corona.set_transform(character, {
        position = { position[1], position[2] + bob, position[3] },
        rotation = current_rotation(),
        target_extent = controls.targetExtent,
    })
end

spawn_character()
update_character()
corona.log("startup procedural block character script loaded")

function script.update(dt)
    time = time + dt
    update_character()
end

function script.shutdown()
    remove_character()
end

return script
