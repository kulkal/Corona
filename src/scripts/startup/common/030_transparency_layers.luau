local script = {}

local startup_mode = corona_script_startup_mode or ""
local default_enabled = startup_mode == "sandbox"

corona_script_controls = corona_script_controls or {}
corona_script_controls.transparency_layers = corona_script_controls.transparency_layers or {}

local controls = corona_script_controls.transparency_layers

local function persistent_default(name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control("transparency_layers." .. name, value)
    end
    return value
end

local function set_default(name, value)
    if controls[name] == nil then
        controls[name] = persistent_default(name, value)
    end
end

set_default("enabled", default_enabled)
set_default("layer_count", 6)
set_default("distance", 48.0)
set_default("spacing", 8.0)
set_default("width", 88.0)
set_default("height", 54.0)
set_default("alpha", 0.24)
set_default("shape_count", 9)
set_default("shape_distance", 34.0)
set_default("shape_spread", 20.0)
set_default("shape_size", 10.0)
set_default("shape_alpha", 0.34)
set_default("diamond_enabled", true)
set_default("diamond_distance", 28.0)
set_default("diamond_size", 13.0)
set_default("diamond_alpha", 0.42)
set_default("rotate_objects", false)
set_default("follow_camera", true)
set_default("fixed_current_position", false)
set_default("distortion_scale", 1.0)
set_default("pre_lighting_refracted_gbuffer", true)
set_default("rt_refraction_layers", 4)
set_default("rt_refraction_ior", 1.45)
set_default("rt_refraction_ray_bias", 0.05)
set_default("rt_refraction_debug", false)
set_default("refraction_only", false)
set_default("reflection_enabled", true)
set_default("reflection_scale", 1.0)
set_default("reflection_strength", 0.92)
set_default("surface_strength", 0.55)
set_default("tint_strength", 0.22)
set_default("fresnel_power", 2.35)

local palette = {
    { 0.95, 0.18, 0.14 },
    { 0.12, 0.62, 1.00 },
    { 0.18, 0.86, 0.42 },
    { 1.00, 0.72, 0.18 },
    { 0.72, 0.30, 1.00 },
    { 0.10, 0.92, 0.86 },
    { 1.00, 0.38, 0.62 },
    { 0.72, 0.92, 0.18 },
}

local shape_primitives = {
    "CUBE",
    "SPHERE",
    "BOX",
    "SPHERE",
    "CUBE",
    "BOX",
    "SPHERE",
    "CUBE",
    "BOX",
}

local layers = {}
local shapes = {}
local diamond = nil
local layer_slots = 0
local shape_slots = 0
local spawned_logged = false
local pending_rebuild = false
local pending_capture_current = false
local elapsed_seconds = 0.0
local locked_layer_poses = {}
local locked_shape_poses = {}
local locked_diamond_pose = nil
local last_layer_poses = {}
local last_shape_poses = {}
local last_diamond_pose = nil

if controls.follow_camera ~= true then
    controls.fixed_current_position = true
end
if controls.fixed_current_position == true then
    controls.follow_camera = false
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

local function pseudo_random(index, salt)
    local value = math.sin(index * 12.9898 + salt * 78.233) * 43758.5453
    return value - math.floor(value)
end

local function persist_control(name, value)
    controls[name] = value
    if corona.set_persistent_control ~= nil then
        corona.set_persistent_control("transparency_layers." .. name, value)
    end
end

local function layer_color(index)
    local color = palette[((index - 1) % #palette) + 1]
    local alpha = clamp((controls.alpha or 0.24) + (index - 1) * 0.015, 0.05, 0.65)
    return color, alpha
end

local function shape_color(index)
    local color = palette[((index + 2) % #palette) + 1]
    local alpha = clamp((controls.shape_alpha or 0.34) + pseudo_random(index, 11.0) * 0.08, 0.08, 0.7)
    return color, alpha
end

local function add(a, b)
    return { a[1] + b[1], a[2] + b[2], a[3] + b[3] }
end

local function mul(a, s)
    return { a[1] * s, a[2] * s, a[3] * s }
end

local function clone_vec3(v)
    return { v[1], v[2], v[3] }
end

local function clone_pose(pose)
    return {
        position = clone_vec3(pose.position),
        rotation = clone_vec3(pose.rotation),
        scale = clone_vec3(pose.scale),
    }
end

local function clear_locked_poses()
    locked_layer_poses = {}
    locked_shape_poses = {}
    locked_diamond_pose = nil
    pending_capture_current = false
end

local function capture_current_poses()
    locked_layer_poses = {}
    locked_shape_poses = {}
    local captured = false
    for i = 1, layer_slots do
        if last_layer_poses[i] ~= nil then
            locked_layer_poses[i] = clone_pose(last_layer_poses[i])
            captured = true
        end
    end
    for i = 1, shape_slots do
        if last_shape_poses[i] ~= nil then
            locked_shape_poses[i] = clone_pose(last_shape_poses[i])
            captured = true
        end
    end
    if last_diamond_pose ~= nil then
        locked_diamond_pose = clone_pose(last_diamond_pose)
        captured = true
    end
    pending_capture_current = not captured
end

local function entity_alive(entity)
    if entity == nil then
        return false
    end
    if corona.Entity ~= nil and corona.Entity.exists ~= nil then
        return corona.Entity.exists(entity)
    end
    return true
end

local function spawn_layer(index)
    local color, alpha = layer_color(index)
    local entity = corona.Entity.create("TransparencyLayer_" .. tostring(index))
    local ok = corona.MeshComponent.add(entity, {
        primitive = "QUAD",
        color = { color[1], color[2], color[3], alpha },
        alpha = alpha,
        position = { 0.0, 0.0, 0.0 },
        rotation = { 0.0, 0.0, 0.0 },
        scale = { controls.width, controls.height, 0.02 },
        roughness = 0.25,
        metallic = 0.0,
        override_material = true,
        ray_tracing = true,
        physics = false,
    })
    if not ok then
        corona.Entity.destroy(entity)
        return nil
    end
    return entity
end

local function spawn_shape(index)
    local color, alpha = shape_color(index)
    local primitive = shape_primitives[((index - 1) % #shape_primitives) + 1]
    local entity = corona.Entity.create("TransparencyShape_" .. tostring(index) .. "_" .. primitive)
    local descriptor = {
        primitive = primitive,
        color = { color[1], color[2], color[3], alpha },
        alpha = alpha,
        position = { 0.0, 0.0, 0.0 },
        rotation = { 0.0, 0.0, 0.0 },
        scale = { controls.shape_size, controls.shape_size, controls.shape_size },
        roughness = 0.2,
        metallic = 0.0,
        override_material = true,
        ray_tracing = true,
        physics = false,
    }
    if primitive == "SPHERE" then
        descriptor.radius = 0.5
        descriptor.rings = 20
        descriptor.segments = 32
    end
    local ok = corona.MeshComponent.add(entity, descriptor)
    if not ok then
        corona.Entity.destroy(entity)
        return nil
    end
    return entity
end

local function spawn_diamond()
    local alpha = clamp(controls.diamond_alpha or controls.shape_alpha or 0.42, 0.08, 0.75)
    local entity = corona.Entity.create("TransparencyCenterDiamond")
    local ok = corona.MeshComponent.add(entity, {
        primitive = "DIAMOND",
        color = { 0.74, 0.94, 1.0, alpha },
        alpha = alpha,
        position = { 0.0, 0.0, 0.0 },
        rotation = { 0.0, 0.0, 0.0 },
        scale = { controls.diamond_size, controls.diamond_size, controls.diamond_size },
        roughness = 0.08,
        metallic = 0.0,
        override_material = true,
        ray_tracing = true,
        physics = false,
    })
    if not ok then
        corona.Entity.destroy(entity)
        return nil
    end
    return entity
end

local function ensure_layers()
    local count = math.max(5, math.floor(controls.layer_count or 6))
    for i = 1, count do
        if not entity_alive(layers[i]) then
            layers[i] = spawn_layer(i)
        end
    end
    for i = layer_slots, count + 1, -1 do
        if layers[i] ~= nil then
            corona.Entity.destroy(layers[i])
        end
        layers[i] = nil
    end
    layer_slots = count
end

local function ensure_shapes()
    local count = math.max(0, math.floor(controls.shape_count or 9))
    for i = 1, count do
        if not entity_alive(shapes[i]) then
            shapes[i] = spawn_shape(i)
        end
    end
    for i = shape_slots, count + 1, -1 do
        if shapes[i] ~= nil then
            corona.Entity.destroy(shapes[i])
        end
        shapes[i] = nil
    end
    shape_slots = count
end

local function ensure_diamond()
    if controls.diamond_enabled ~= true then
        if diamond ~= nil then
            corona.Entity.destroy(diamond)
            diamond = nil
        end
        last_diamond_pose = nil
        locked_diamond_pose = nil
        return
    end
    if not entity_alive(diamond) then
        diamond = spawn_diamond()
    end
end

local function destroy_layers()
    for i = 1, layer_slots do
        local entity = layers[i]
        if entity ~= nil then
            corona.Entity.destroy(entity)
        end
        layers[i] = nil
    end
    for i = 1, shape_slots do
        local entity = shapes[i]
        if entity ~= nil then
            corona.Entity.destroy(entity)
        end
        shapes[i] = nil
    end
    if diamond ~= nil then
        corona.Entity.destroy(diamond)
        diamond = nil
    end
    layer_slots = 0
    shape_slots = 0
    spawned_logged = false
    last_layer_poses = {}
    last_shape_poses = {}
    last_diamond_pose = nil
    locked_diamond_pose = nil
end

local function make_layer_pose(index, camera)
    local depth = controls.distance + (index - 1) * controls.spacing
    return {
        position = add(camera.position, mul(camera.forward, depth)),
        rotation = {
            -camera.pitch,
            camera.yaw,
            0.0,
        },
        scale = { controls.width, controls.height, 0.02 },
    }
end

local function make_shape_pose(index, camera, animation_time)
    local primitive = shape_primitives[((index - 1) % #shape_primitives) + 1]
    local depth = (controls.shape_distance or 34.0) + pseudo_random(index, 3.0) * 38.0
    local side = ((index % 2) == 0 and 1.0 or -1.0)
    local lateral = side * (5.0 + pseudo_random(index, 4.0) * (controls.shape_spread or 20.0))
    local vertical = (pseudo_random(index, 5.0) - 0.5) * 20.0
    lateral = clamp(lateral, -depth * 0.36, depth * 0.36)
    vertical = clamp(vertical, -depth * 0.22, depth * 0.22)

    local base_size = (controls.shape_size or 10.0) * (0.72 + pseudo_random(index, 6.0) * 0.58)
    local scale = { base_size, base_size, base_size }
    if primitive == "BOX" then
        scale = {
            base_size * (1.15 + pseudo_random(index, 7.0) * 0.65),
            base_size * (0.38 + pseudo_random(index, 8.0) * 0.35),
            base_size * (0.65 + pseudo_random(index, 9.0) * 0.55),
        }
    end

    local position = add(camera.position, mul(camera.forward, depth))
    position = add(position, mul(camera.right, lateral))
    position = add(position, mul(camera.up, vertical))

    return {
        position = position,
        rotation = {
            pseudo_random(index, 12.0) * 80.0 + animation_time * 12.0,
            pseudo_random(index, 13.0) * 180.0 + animation_time * 8.0,
            pseudo_random(index, 14.0) * 90.0 + animation_time * 10.0,
        },
        scale = scale,
    }
end

local function make_diamond_pose(camera)
    local depth = controls.diamond_distance or 28.0
    local size = controls.diamond_size or controls.shape_size or 13.0
    return {
        position = add(camera.position, mul(camera.forward, depth)),
        rotation = {
            -camera.pitch,
            camera.yaw + 45.0,
            0.0,
        },
        scale = { size, size, size },
    }
end

local function update_layers(dt)
    if pending_rebuild then
        destroy_layers()
        pending_rebuild = false
    end

    if not controls.enabled then
        destroy_layers()
        return
    end
    elapsed_seconds = elapsed_seconds + (dt or 0.0)

    ensure_layers()
    ensure_shapes()
    ensure_diamond()
    if not spawned_logged then
        corona.log("[transparency_layers] spawned translucent planes, shapes, and center diamond")
        spawned_logged = true
    end

    local camera = corona.get_camera()
    local use_fixed_position = controls.fixed_current_position == true or controls.follow_camera ~= true
    if pending_capture_current then
        capture_current_poses()
    end

    local count = layer_slots
    for i = 1, count do
        local entity = layers[i]
        if entity ~= nil then
            local color, alpha = layer_color(i)
            local pose = make_layer_pose(i, camera)
            if use_fixed_position then
                local locked_pose = locked_layer_poses[i]
                if locked_pose == nil then
                    locked_layer_poses[i] = clone_pose(pose)
                else
                    pose.position = clone_vec3(locked_pose.position)
                    pose.rotation = clone_vec3(locked_pose.rotation)
                end
            end
            last_layer_poses[i] = clone_pose(pose)

            corona.MeshComponent.set(entity, {
                color = { color[1], color[2], color[3], alpha },
                alpha = alpha,
                position = pose.position,
                rotation = pose.rotation,
                scale = pose.scale,
                roughness = 0.25,
                metallic = 0.0,
                override_material = true,
                ray_tracing = true,
                physics = false,
            })
        end
    end

    local animation_time = controls.rotate_objects and elapsed_seconds or 0.0
    for i = 1, shape_slots do
        local entity = shapes[i]
        if entity ~= nil then
            local color, alpha = shape_color(i)
            local pose = make_shape_pose(i, camera, animation_time)
            if use_fixed_position then
                local locked_pose = locked_shape_poses[i]
                if locked_pose == nil then
                    locked_shape_poses[i] = clone_pose(pose)
                else
                    pose.position = clone_vec3(locked_pose.position)
                end
            end
            last_shape_poses[i] = clone_pose(pose)

            corona.MeshComponent.set(entity, {
                color = { color[1], color[2], color[3], alpha },
                alpha = alpha,
                position = pose.position,
                rotation = pose.rotation,
                scale = pose.scale,
                roughness = 0.2,
                metallic = 0.0,
                override_material = true,
                ray_tracing = true,
                physics = false,
            })
        end
    end

    if diamond ~= nil then
        local alpha = clamp(controls.diamond_alpha or controls.shape_alpha or 0.42, 0.08, 0.75)
        local pose = make_diamond_pose(camera)
        if use_fixed_position then
            if locked_diamond_pose == nil then
                locked_diamond_pose = clone_pose(pose)
            else
                pose.position = clone_vec3(locked_diamond_pose.position)
                pose.rotation = clone_vec3(locked_diamond_pose.rotation)
            end
        end
        last_diamond_pose = clone_pose(pose)
        corona.MeshComponent.set(diamond, {
            color = { 0.74, 0.94, 1.0, alpha },
            alpha = alpha,
            position = pose.position,
            rotation = pose.rotation,
            scale = pose.scale,
            roughness = 0.08,
            metallic = 0.0,
            override_material = true,
            ray_tracing = true,
            physics = false,
        })
    end
end

local function slider_float(label, name, min_value, max_value, format, rebuild)
    local next_value, changed = imgui.slider_float(label .. "##transparency_layers_" .. name, controls[name] or min_value, min_value, max_value, format or "%.2f")
    if changed then
        persist_control(name, next_value)
        if rebuild then
            pending_rebuild = true
        end
    end
end

local function slider_int(label, name, min_value, max_value, rebuild)
    local next_value, changed = imgui.slider_int(label .. "##transparency_layers_" .. name, math.floor(controls[name] or min_value), min_value, max_value)
    if changed then
        persist_control(name, next_value)
        if rebuild then
            pending_rebuild = true
        end
    end
end

local function checkbox(label, name, rebuild)
    local next_value, changed = imgui.checkbox(label .. "##transparency_layers_" .. name, controls[name] == true)
    if changed then
        persist_control(name, next_value)
        if rebuild then
            pending_rebuild = true
        end
    end
    return next_value, changed
end

function script.imgui()
    if not imgui.begin("Transparency Layers") then
        imgui.end_window()
        return
    end

    checkbox("Enabled", "enabled", true)
    local _, rotate_changed = checkbox("Rotate Objects", "rotate_objects", false)
    if rotate_changed and controls.rotate_objects == false then
        elapsed_seconds = 0.0
    end
    local follow_camera, follow_changed = checkbox("Follow Camera", "follow_camera", false)
    if follow_changed then
        if follow_camera then
            persist_control("fixed_current_position", false)
            clear_locked_poses()
        else
            persist_control("fixed_current_position", true)
            capture_current_poses()
        end
    end
    local fixed_current_position, fixed_changed = checkbox("Lock Current Position", "fixed_current_position", false)
    if fixed_changed then
        if fixed_current_position then
            persist_control("follow_camera", false)
            capture_current_poses()
        else
            persist_control("follow_camera", true)
            clear_locked_poses()
        end
    end
    imgui.separator()
    slider_float("Object Alpha", "shape_alpha", 0.02, 0.70, "%.2f", true)
    slider_float("Layer Alpha", "alpha", 0.02, 0.65, "%.2f", true)
    slider_int("Layers", "layer_count", 5, 12, false)
    slider_int("Objects", "shape_count", 0, 18, false)
    slider_float("Object Size", "shape_size", 2.0, 24.0, "%.1f", false)
    checkbox("Center Diamond", "diamond_enabled", true)
    slider_float("Diamond Size", "diamond_size", 4.0, 28.0, "%.1f", false)
    slider_float("Diamond Alpha", "diamond_alpha", 0.08, 0.75, "%.2f", true)
    imgui.separator()
    slider_float("Distortion Scale", "distortion_scale", 0.0, 12.0, "%.2f", false)
    checkbox("Pre-light Refracted GBuffer", "pre_lighting_refracted_gbuffer", false)
    slider_int("RT Refract Layers", "rt_refraction_layers", 1, 8, false)
    slider_float("RT Refract IOR", "rt_refraction_ior", 1.01, 2.50, "%.2f", false)
    slider_float("RT Ray Bias", "rt_refraction_ray_bias", 0.001, 0.50, "%.3f", false)
    checkbox("RT Refract Debug", "rt_refraction_debug", false)
    local refraction_only, _ = checkbox("Refraction Only", "refraction_only", false)
    if refraction_only then
        imgui.begin_disabled()
    end
    local reflection_enabled, _ = checkbox("Reflection", "reflection_enabled", false)
    if not reflection_enabled then
        imgui.begin_disabled()
    end
    slider_float("Reflection Scale", "reflection_scale", 0.0, 3.0, "%.2f", false)
    slider_float("Reflection Strength", "reflection_strength", 0.0, 1.0, "%.2f", false)
    if not reflection_enabled then
        imgui.end_disabled()
    end
    slider_float("Surface Light", "surface_strength", 0.0, 1.0, "%.2f", false)
    slider_float("Tint", "tint_strength", 0.0, 1.0, "%.2f", false)
    slider_float("Fresnel Power", "fresnel_power", 0.25, 8.0, "%.2f", false)
    if refraction_only then
        imgui.end_disabled()
    end

    imgui.end_window()
end

corona.log("[transparency_layers] loaded enabled=" .. tostring(controls.enabled))

function script.update(dt)
    update_layers(dt)
end

function script.shutdown()
    destroy_layers()
end

return script
