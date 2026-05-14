local script = {}

corona_script_controls = corona_script_controls or {}
if corona_script_controls.dungeon ~= nil and corona_script_controls.dungeon.enabled then
    corona.log("startup pistol follow skipped for dungeon mode")
    return {}
end

corona_script_controls = corona_script_controls or {}
corona_script_controls.pistol = corona_script_controls.pistol or {}

local pistolControls = corona_script_controls.pistol

local function persistent_default(name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control("pistol." .. name, value)
    end
    return value
end

local function set_default(name, value)
    if pistolControls[name] == nil then
        pistolControls[name] = persistent_default(name, value)
    end
end

set_default("weaponDistance", 96.0)
set_default("weaponRightOffset", 14.0)
set_default("weaponDownOffset", 44.0)
set_default("weaponExtent", 92.0)
set_default("yawOffset", 0.0)
set_default("pitchOffset", -4.0)
set_default("rollOffset", 0.0)
set_default("enabled", true)
set_default("spawned", false)

local pistol = nil

local function spawn_pistol()
    if pistol ~= nil then
        return
    end

    pistol = corona.Entity.create("Pistol")
    local ok = corona.MeshComponent.add(pistol, {
        path = "assets/pistol/pistol.obj",
        position = { 0.0, 0.0, 0.0 },
        rotation = { 0.0, 0.0, 0.0 },
        target_extent = pistolControls.weaponExtent,
        roughness = 0.55,
        metallic = 0.0,
        ray_tracing = true,
        physics = false,
    })
    if not ok then
        corona.Entity.destroy(pistol)
        pistol = nil
    end
    pistolControls.spawned = pistol ~= nil
end

local function remove_pistol()
    if pistol == nil then
        pistolControls.spawned = false
        return
    end

    corona.Entity.destroy(pistol)
    pistol = nil
    pistolControls.spawned = false
end

local function add(a, b)
    return { a[1] + b[1], a[2] + b[2], a[3] + b[3] }
end

local function mul(a, s)
    return { a[1] * s, a[2] * s, a[3] * s }
end

local function update_weapon()
    if pistol == nil then
        return
    end

    local camera = corona.get_camera()
    local weaponPosition = add(camera.position, mul(camera.forward, pistolControls.weaponDistance))
    weaponPosition = add(weaponPosition, mul(camera.right, pistolControls.weaponRightOffset))
    weaponPosition = add(weaponPosition, mul(camera.up, -pistolControls.weaponDownOffset))

    corona.MeshComponent.set(pistol, {
        position = weaponPosition,
        rotation = {
            -camera.pitch + pistolControls.pitchOffset,
            camera.yaw + pistolControls.yawOffset,
            pistolControls.rollOffset,
        },
        target_extent = pistolControls.weaponExtent,
    })
end

if pistolControls.enabled then
    spawn_pistol()
    update_weapon()
end
corona.log("startup pistol follow script loaded")

function script.update(dt)
    if pistolControls.enabled and pistol == nil then
        spawn_pistol()
    elseif not pistolControls.enabled and pistol ~= nil then
        remove_pistol()
    end

    update_weapon()
end

return script
