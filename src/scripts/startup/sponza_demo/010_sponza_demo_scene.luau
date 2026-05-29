-- Sponza fly-around demo: load the Sponza FBX as a script-spawned entity,
-- persist the scene to assets/maps/sponza, and host a small in-viewport
-- panel that toggles between the 4-channel hard-shadow path and the
-- ReSTIR Phase 1 reservoir path. Hitting "RESTIR MODE" also spawns 100
-- randomly-colored point lights so the difference between modes is
-- visible — Option A only shadows the first 3, ReSTIR samples all.

local mapName = "sponza"
local sponzaEntity

if corona.map_exists(mapName) then
    local ok, err = corona.load_map(mapName)
    if ok then
        sponzaEntity = corona.Entity.by_name("SponzaModel")
        corona.log("[sponza_demo] loaded map " .. mapName)
    else
        corona.log("[sponza_demo] load_map failed: " .. tostring(err) .. " — falling back to spawn block")
    end
end

if not sponzaEntity then
    sponzaEntity = corona.Entity.create("SponzaModel")
    corona.MeshComponent.add(sponzaEntity, {
        primitive    = "asset",
        path         = "assets/Sponza/Sponza.fbx",
        position     = { 0.0, 0.0, 0.0 },
        rotation     = { 0.0, 0.0, 0.0 },
        scale        = { 1.0, 1.0, 1.0 },
        roughness    = 1.0,
        metallic     = 0.0,
        override_material = false,
        ray_tracing  = true,
    })
    corona.log("[sponza_demo] spawned Sponza model")
    local ok, err = corona.save_map(mapName)
    if ok then
        corona.log("[sponza_demo] saved initial scene to " .. mapName)
    else
        corona.log("[sponza_demo] save_map failed: " .. tostring(err))
    end
end

-- Track lights spawned by the ReSTIR demo so we can clear only those
-- (and leave any user-placed lights alone) when switching back to the
-- 4-channel mode. Persisted in a script-local table; values are the
-- light ids returned by spawn_random_point_lights.
local demoLightIds = {}

local function spawn_demo_lights()
    if #demoLightIds > 0 then
        return -- already spawned
    end
    -- Sponza interior extents roughly [-1000, 1000] in X/Z and [0, 1300]
    -- in Y. Scatter lights through the cathedral height so the floor and
    -- the upper galleries both get coverage.
    local ids = corona.run_ui_command("spawn_random_point_lights", {
        count     = 100,
        area_min  = { -1100.0,  60.0, -480.0 },
        area_max  = {  1100.0, 700.0,  480.0 },
        radius    = 250.0,
        intensity = 8.0,
        seed      = 20260529,
    })
    if type(ids) == "table" then
        demoLightIds = ids
        corona.log("[sponza_demo] spawned " .. tostring(#ids) .. " ReSTIR demo lights")
    end
end

local function clear_demo_lights()
    if #demoLightIds == 0 then
        return
    end
    corona.run_ui_command("remove_point_lights_by_ids", demoLightIds)
    corona.log("[sponza_demo] removed " .. tostring(#demoLightIds) .. " ReSTIR demo lights")
    demoLightIds = {}
end

-- Track the shadow mode script-side so the imgui callback never has to
-- call corona.get_ui_state() each frame. Defaults to true to mirror the
-- engine's bEnableReSTIRDirectShadow default — flipping the toggle here
-- still syncs both sides via corona.set_ui_value.
local restirOn = true

local function on_imgui()
    if not imgui.begin("Sponza Shadow Demo") then
        imgui.end_window()
        return
    end

    imgui.text("Current: " .. (restirOn and "ReSTIR DI (single-light reservoir)" or "4-channel (hard cap 3 lights)"))
    imgui.text(string.format("Demo lights: %d", #demoLightIds))

    if not restirOn then
        if imgui.button("RESTIR MODE") then
            spawn_demo_lights()
            corona.set_ui_value("enable_restir_direct_shadow", true)
            restirOn = true
        end
    else
        if imgui.button("4CHANNEL MODE") then
            clear_demo_lights()
            corona.set_ui_value("enable_restir_direct_shadow", false)
            restirOn = false
        end
    end

    imgui.text("Tip: ReSTIR Phase 1 has no temporal reuse yet —")
    imgui.text("expect noise until Phase 2 reproject lands.")
    imgui.end_window()
end

return { imgui = on_imgui }
