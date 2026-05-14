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

local function vec_add(a, b)
    return { a[1] + b[1], a[2] + b[2], a[3] + b[3] }
end

local function vec_mul(a, s)
    return { a[1] * s, a[2] * s, a[3] * s }
end

local function forward_from_yaw(yaw)
    local radians = math.rad(yaw)
    return {
        math.sin(radians),
        0.0,
        math.cos(radians),
    }
end

local function update_look(runtime, dt)
    local player = runtime.player
    if player == nil then
        return
    end

    local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
    local fastLook =
        corona.is_key_down("SHIFT") or
        (gamepad.connected and ((gamepad.right_shoulder == true) or ((gamepad.right_trigger or 0.0) > 0.35) or (gamepad.left_thumb == true)))
    local turnSpeed = fastLook and 170.0 or 125.0

    local mouse = corona.get_mouse()
    if mouse.dx ~= 0 or mouse.dy ~= 0 then
        runtime.cameraYaw = runtime.cameraYaw - mouse.dx * runtime.mouseSensitivity
        runtime.cameraPitch = clamp(runtime.cameraPitch + mouse.dy * runtime.mouseSensitivity, 12.0, 52.0)
    end
    if gamepad.connected then
        runtime.cameraYaw = runtime.cameraYaw - (gamepad.right_x or 0.0) * runtime.gamepadYawSpeed * dt
        runtime.cameraPitch = clamp(runtime.cameraPitch - (gamepad.right_y or 0.0) * runtime.gamepadPitchSpeed * dt, 12.0, 52.0)
    end
    if corona.is_key_down("LEFT") then
        runtime.cameraYaw = runtime.cameraYaw + turnSpeed * dt
    end
    if corona.is_key_down("RIGHT") then
        runtime.cameraYaw = runtime.cameraYaw - turnSpeed * dt
    end
    player.yaw = runtime.cameraYaw
end

local function update_camera(runtime, entity)
    local player = runtime.player
    if player == nil or entity == nil then
        return
    end

    local forward = forward_from_yaw(runtime.cameraYaw)
    local focus = { player.x, 72.0, player.z }
    local pitchRadians = math.rad(runtime.cameraPitch)
    local cameraDistance = 300.0
    local horizontalDistance = math.cos(pitchRadians) * cameraDistance
    local verticalOffset = 80.0 + math.sin(pitchRadians) * cameraDistance
    local desired = vec_add(focus, vec_add(vec_mul(forward, -horizontalDistance), { 0.0, verticalOffset, 0.0 }))
    corona.CameraComponent.set(entity, {
        position = desired,
        look_at = vec_add(focus, vec_mul(forward, 86.0)),
        up = { 0.0, 1.0, 0.0 },
        active = true,
    })
end

function script.update(entity, dt)
    local runtime = corona_dungeon_runtime
    if runtime == nil then
        return
    end

    update_look(runtime, dt)
    update_camera(runtime, entity)
end

return script
