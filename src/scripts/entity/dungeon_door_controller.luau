local script = {}

local function clamp(value, minValue, maxValue)
    if value < minValue then
        return minValue
    end
    if value > maxValue then
        return maxValue
    end
    return value
end

local function distance2d(aX, aZ, bX, bZ)
    local dx = aX - bX
    local dz = aZ - bZ
    return math.sqrt(dx * dx + dz * dz)
end

local function update_interaction(runtime)
    local player = runtime.player
    local doorWorld = runtime.doorWorld
    if player == nil or doorWorld == nil then
        return
    end

    local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
    local pressed = corona.was_key_pressed("SPACE") or (gamepad.connected and (gamepad.a_pressed or gamepad.x_pressed))
    if not pressed then
        return
    end

    local interactDistance = runtime.doorInteractDistance or ((runtime.tile or 120.0) * 1.35)
    if distance2d(player.x, player.z, doorWorld[1], doorWorld[2]) < interactDistance then
        runtime.doorOpen = not runtime.doorOpen
    end
end

local function update_mesh(runtime, entity, dt)
    if entity == nil or runtime.door == nil or runtime.doorWorld == nil or runtime.doorScale == nil then
        return
    end

    local target = runtime.doorOpen == true and 1.0 or 0.0
    runtime.doorOpenAmount = (runtime.doorOpenAmount or 0.0) + (target - (runtime.doorOpenAmount or 0.0)) * clamp(dt * 9.0, 0.0, 1.0)
    local yaw = runtime.doorOpenAmount * 88.0
    if runtime.door.axis == "x" then
        yaw = -yaw
    end

    corona.MeshComponent.set(entity, {
        position = { runtime.doorWorld[1], 0.0, runtime.doorWorld[2] },
        rotation = { 0.0, yaw, 0.0 },
        scale = runtime.doorScale,
    })
end

function script.update(entity, dt)
    local runtime = corona_dungeon_runtime
    if runtime == nil then
        return
    end

    update_interaction(runtime)
    update_mesh(runtime, entity, dt)
end

return script
