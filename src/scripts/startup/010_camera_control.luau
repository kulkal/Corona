local script = {}

corona_script_controls = corona_script_controls or {}
if corona_script_controls.dungeon ~= nil and corona_script_controls.dungeon.enabled then
    corona.log("startup free camera control skipped for dungeon mode")
    return {}
end

local moveSpeed = 200.0
local fastMoveSpeed = 520.0
local turnSpeed = 90.0
local mouseSensitivity = math.deg(0.003)
local pitchLimit = 45.0
local worldUp = { 0.0, 1.0, 0.0 }

local initialCamera = corona.get_camera()
local initialPosition = { initialCamera.position[1], initialCamera.position[2], initialCamera.position[3] }
local initialYaw = initialCamera.yaw
local initialPitch = initialCamera.pitch
local yaw = initialYaw
local pitch = initialPitch
local cameraPathWasDriving = false
local cameraEntity = corona.Entity.main_camera()

local function add(a, b)
    return { a[1] + b[1], a[2] + b[2], a[3] + b[3] }
end

local function clamp(value, minValue, maxValue)
    if value < minValue then
        return minValue
    end
    if value > maxValue then
        return maxValue
    end
    return value
end

local function forward_from_angles(yawDegrees, pitchDegrees)
    local yawRadians = math.rad(yawDegrees)
    local pitchRadians = math.rad(pitchDegrees)
    local r = math.cos(pitchRadians)
    return {
        r * math.sin(yawRadians),
        math.sin(pitchRadians),
        r * math.cos(yawRadians),
    }
end

local function apply_camera(position)
    local forward = forward_from_angles(yaw, pitch)
    corona.CameraComponent.set(cameraEntity, {
        position = position,
        look_at = add(position, forward),
        up = worldUp,
        active = true,
    })
end

corona.set_camera_control(true)
apply_camera(initialPosition)
corona.log("startup camera control script loaded")

function script.update(dt)
    local state = corona.get_ui_state()
    local camera = corona.get_camera()
    local position = { camera.position[1], camera.position[2], camera.position[3] }

    if state.camera_path_playing or state.camera_path_dumping then
        yaw = camera.yaw
        pitch = camera.pitch
        cameraPathWasDriving = true
        return
    end

    if cameraPathWasDriving then
        yaw = camera.yaw
        pitch = camera.pitch
        cameraPathWasDriving = false
    end

    if corona.was_key_pressed("ESC") then
        position = { initialPosition[1], initialPosition[2], initialPosition[3] }
        yaw = initialYaw
        pitch = initialPitch
    end

    local moveX = 0.0
    local moveY = 0.0
    local moveZ = 0.0

    if corona.is_key_down("A") then
        moveX = moveX - 1.0
    end
    if corona.is_key_down("D") then
        moveX = moveX + 1.0
    end
    if corona.is_key_down("W") then
        moveZ = moveZ - 1.0
    end
    if corona.is_key_down("S") then
        moveZ = moveZ + 1.0
    end
    if corona.is_key_down("Q") then
        moveY = moveY - 1.0
    end
    if corona.is_key_down("E") then
        moveY = moveY + 1.0
    end

    local speed = corona.is_key_down("SHIFT") and fastMoveSpeed or moveSpeed
    local moveInterval = speed * dt
    local yawRadians = math.rad(yaw)
    local worldMoveX = moveX * -math.cos(yawRadians) - moveZ * math.sin(yawRadians)
    local worldMoveZ = moveX * math.sin(yawRadians) - moveZ * math.cos(yawRadians)

    position[1] = position[1] + worldMoveX * moveInterval
    position[2] = position[2] + moveY * moveInterval
    position[3] = position[3] + worldMoveZ * moveInterval

    if corona.is_key_down("LEFT") then
        yaw = yaw + turnSpeed * dt
    end
    if corona.is_key_down("RIGHT") then
        yaw = yaw - turnSpeed * dt
    end
    if corona.is_key_down("UP") then
        pitch = pitch + turnSpeed * dt
    end
    if corona.is_key_down("DOWN") then
        pitch = pitch - turnSpeed * dt
    end

    local mouse = corona.get_mouse()
    if mouse.right_down then
        yaw = yaw - mouse.dx * mouseSensitivity
        pitch = pitch - mouse.dy * mouseSensitivity
    end
    pitch = clamp(pitch, -pitchLimit, pitchLimit)

    apply_camera(position)
end

return script
