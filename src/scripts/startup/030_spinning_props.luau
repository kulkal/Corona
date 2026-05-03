local script = {}

local buddhaScene = corona.load_scene("assets/buddha/buddha.obj")
local shaderBallScene = corona.load_scene("assets/shaderBall/shaderBall.fbx")

corona_script_controls = corona_script_controls or {}
corona_script_controls.props = corona_script_controls.props or {}

local propsControls = corona_script_controls.props

local function persistent_default(name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control("props." .. name, value)
    end
    return value
end

local function set_default(name, value)
    if propsControls[name] == nil then
        propsControls[name] = persistent_default(name, value)
    end
end

set_default("buddhaPosition", { 273.0, -12.0, -6.0 })
set_default("shaderBallPosition", { 0.0, -4.0, -72.0 })
set_default("buddhaBaseYaw", -12.0)
set_default("shaderBallBaseYaw", -120.0)
set_default("buddhaExtent", 260.0)
set_default("shaderBallExtent", 220.0)
set_default("buddhaBobHeight", 38.0)
set_default("buddhaBobSpeed", 1.35)
set_default("spinSpeed", 35.0)
set_default("buddhaEnabled", true)
set_default("buddhaSpawned", false)
set_default("shaderBallEnabled", true)
set_default("shaderBallSpawned", false)

local angle = 0.0
local time = 0.0

local buddha = nil

local function spawn_buddha()
    if buddha ~= nil then
        return
    end

    buddha = corona.spawn(buddhaScene, {
        position = propsControls.buddhaPosition,
        rotation = { 0.0, propsControls.buddhaBaseYaw, 0.0 },
        target_extent = propsControls.buddhaExtent,
        roughness = 0.65,
        metallic = 0.0,
        ray_tracing = true,
        physics = false,
    })
    propsControls.buddhaSpawned = buddha ~= nil
end

local function remove_buddha()
    if buddha == nil then
        propsControls.buddhaSpawned = false
        return
    end

    corona.destroy(buddha)
    buddha = nil
    propsControls.buddhaSpawned = false
end

local shaderBall = nil

local function spawn_shader_ball()
    if shaderBall ~= nil then
        return
    end

    shaderBall = corona.spawn(shaderBallScene, {
        position = propsControls.shaderBallPosition,
        rotation = { 0.0, propsControls.shaderBallBaseYaw, 0.0 },
        target_extent = propsControls.shaderBallExtent,
        roughness = 0.15,
        metallic = 1.0,
        ray_tracing = true,
        physics = false,
    })
    propsControls.shaderBallSpawned = shaderBall ~= nil
end

local function remove_shader_ball()
    if shaderBall == nil then
        propsControls.shaderBallSpawned = false
        return
    end

    corona.destroy(shaderBall)
    shaderBall = nil
    propsControls.shaderBallSpawned = false
end

local function update_props()
    if propsControls.buddhaEnabled and buddha == nil then
        spawn_buddha()
    elseif not propsControls.buddhaEnabled and buddha ~= nil then
        remove_buddha()
    end
    if propsControls.shaderBallEnabled and shaderBall == nil then
        spawn_shader_ball()
    elseif not propsControls.shaderBallEnabled and shaderBall ~= nil then
        remove_shader_ball()
    end

    local buddhaPosition = propsControls.buddhaPosition
    local shaderBallPosition = propsControls.shaderBallPosition
    local buddhaBobPosition = {
        buddhaPosition[1],
        buddhaPosition[2] + math.sin(time * propsControls.buddhaBobSpeed) * propsControls.buddhaBobHeight,
        buddhaPosition[3],
    }

    if buddha ~= nil then
        corona.set_transform(buddha, {
            position = buddhaBobPosition,
            rotation = { 0.0, propsControls.buddhaBaseYaw + angle, 0.0 },
            target_extent = propsControls.buddhaExtent,
        })
    end

    if shaderBall ~= nil then
        corona.set_transform(shaderBall, {
            position = shaderBallPosition,
            rotation = { 0.0, propsControls.shaderBallBaseYaw - angle * 1.35, 0.0 },
            target_extent = propsControls.shaderBallExtent,
        })
    end
end

if propsControls.buddhaEnabled then
    spawn_buddha()
end
if propsControls.shaderBallEnabled then
    spawn_shader_ball()
end
update_props()
corona.log("startup spinning props script loaded")

function script.update(dt)
    time = time + dt
    angle = (angle + dt * propsControls.spinSpeed) % 360.0
    update_props()
end

return script
