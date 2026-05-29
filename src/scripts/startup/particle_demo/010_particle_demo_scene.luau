-- Particle system Phase 1 / M1 demo: a single static, camera-facing spark
-- billboard at world (0, 3, 0). The ParticleSystem itself is created in C++
-- by the --particle-demo bootstrap path (Corona::LoadAssets). This script
-- only stages a camera looking at the emitter origin plus a directional
-- light so the surrounding HDR has something other than pitch black.
--
-- Triggered by `--particle-demo` (or `--startup-mode=particle_demo`).
--
-- M5 will move the C++ bootstrap behind a Luau binding and let this script
-- own the emitter creation.

local cameraEntity = corona.Entity.create("ParticleDemoCamera")
corona.CameraComponent.add(cameraEntity, {
    position   = { 0.0, 3.0, -5.0 },
    look_at    = { 0.0, 3.0, 0.0 },
    up         = { 0.0, 1.0, 0.0 },
    fov        = 0.9,
    near_plane = 0.1,
    far_plane  = 200.0,
    active     = true,
})

local sunEntity = corona.Entity.create("ParticleDemoSun")
corona.LightComponent.add(sunEntity, {
    type       = "directional",
    enabled    = true,
    direction  = { -0.35, -1.0, 0.45 },
    color      = { 1.0, 0.97, 0.88 },
    intensity  = 1.8,
    persistent = false,
})

corona.log("[particle_demo] camera + sun staged; spark billboard owned by C++ bootstrap")
