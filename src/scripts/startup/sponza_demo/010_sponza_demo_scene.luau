-- Sponza fly-around demo: load the Sponza FBX as a script-spawned entity,
-- persist the scene to assets/maps/sponza, and host a small in-viewport
-- panel that toggles between the 4-channel hard-shadow path and the
-- ReSTIR Phase 1 reservoir path.

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

local restirOn = true
-- Mirrors the C++ default `Corona::ReSTIRShadowMaxM = 3.0f`. Local
-- shadow value so the slider can read/write without round-tripping
-- through corona.get_ui_state on every frame; pushed engine-side via
-- corona.set_ui_value on every drag.
local restirMaxM = 3.0

local function on_imgui()
    if not imgui.begin("Sponza Shadow Demo") then
        imgui.end_window()
        return
    end

    imgui.text("Current: " .. (restirOn and "ReSTIR DI (single-light reservoir)" or "4-channel (hard cap 3 lights)"))

    if not restirOn then
        if imgui.button("RESTIR MODE") then
            corona.set_ui_value("enable_restir_direct_shadow", true)
            restirOn = true
        end
    else
        if imgui.button("4CHANNEL MODE") then
            corona.set_ui_value("enable_restir_direct_shadow", false)
            restirOn = false
        end
    end

    if restirOn then
        local newVal, changed = imgui.slider_float("ReSTIR M cap", restirMaxM, 1.0, 32.0, "%.1f")
        if changed then
            restirMaxM = newVal
            corona.set_ui_value("restir_shadow_max_m", restirMaxM)
        end
    end

    imgui.end_window()
end

return { imgui = on_imgui }
