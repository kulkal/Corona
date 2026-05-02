local pistolScene = corona.load_scene("assets/pistol/pistol.obj")

local weaponDistance = 96.0
local weaponRightOffset = 38.0
local weaponDownOffset = 34.0
local weaponExtent = 92.0
local yawOffset = 0.0
local pitchOffset = -4.0
local rollOffset = 0.0

local pistol = corona.spawn(pistolScene, {
    position = { 0.0, 0.0, 0.0 },
    rotation = { 0.0, 0.0, 0.0 },
    target_extent = weaponExtent,
    roughness = 0.55,
    metallic = 0.0,
    ray_tracing = true,
    physics = false,
})

local function add(a, b)
    return { a[1] + b[1], a[2] + b[2], a[3] + b[3] }
end

local function mul(a, s)
    return { a[1] * s, a[2] * s, a[3] * s }
end

local function update_weapon()
    local camera = corona.get_camera()
    local weaponPosition = add(camera.position, mul(camera.forward, weaponDistance))
    weaponPosition = add(weaponPosition, mul(camera.right, weaponRightOffset))
    weaponPosition = add(weaponPosition, mul(camera.up, -weaponDownOffset))

    corona.set_transform(pistol, {
        position = weaponPosition,
        rotation = {
            camera.pitch + pitchOffset,
            camera.yaw + yawOffset,
            rollOffset,
        },
        target_extent = weaponExtent,
    })
end

update_weapon()

corona.log("startup fps pistol follow script loaded")

function update(dt)
    update_weapon()
end
