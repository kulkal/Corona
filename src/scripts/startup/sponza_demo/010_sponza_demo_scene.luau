-- Sponza fly-around demo: load the Sponza FBX as a script-spawned entity
-- (instead of the legacy hard-coded SceneObject), persist everything to
-- assets/maps/sponza so the next launch is a single load_map call.
--
-- The camera and sun are managed by the C++ engine (ApplySponzaFlyCamera /
-- LoadCameraState) so SimpleCamera's mouse + WASD input keeps working.
-- We only contribute the Sponza mesh entity here. Once CameraComponent
-- can consume mouse delta we can move the camera into the map too.

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
