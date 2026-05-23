-- Phase 5: Spine benchmark helpers, extracted from 004_spine_platformer.luau
-- to reduce that file's top-level local count and start the per-feature
-- script split described in
-- docs/spine_cpu_sprite_batching_optimization_plan.md.
--
-- This chunk runs after 004_spine_platformer.luau, which is responsible
-- for populating `corona_platformer_locals` with the handful of locals
-- and helpers the benchmark code consumes. Keeping that contract small
-- (data + three functions + a `get_time` accessor) is what lets this
-- file remain a separate .luau chunk without losing access to the main
-- script's state.

local function loc()
    return corona_platformer_locals or {}
end

-- `controls` is a local in 004_spine_platformer.luau and therefore invisible
-- across chunks. Read the source-of-truth platformer controls table directly
-- so this file does not depend on 004's locals being lifted into the
-- corona_platformer_locals shim.
local function platformer_controls()
    if corona_script_controls == nil then
        return {}
    end
    return corona_script_controls.platformer or {}
end

function corona_platformer_build_spine_benchmark_assets(count)
    local assets = {}
    local enemyAssets = loc().enemyAssets or {}
    for _, asset in ipairs(enemyAssets) do
        if asset.path ~= nil and (corona_platformer_is_player_candidate == nil or corona_platformer_is_player_candidate(asset)) then
            table.insert(assets, asset)
            if #assets >= count then
                break
            end
        end
    end

    if #assets <= 0 then
        table.insert(assets, {
            name = "ares",
            path = "assets/spine/chars/ares/ares.json",
        })
    end
    return assets
end

function corona_platformer_spawn_spine_benchmark_characters()
    local controls = platformer_controls()
    if controls.spineSkinningBenchmark ~= true then
        return
    end

    local L = loc()
    local player = L.player
    local loopSeconds = L.loopSeconds or {}
    local add_spawned = L.add_spawned
    local get_spine_scene = L.get_spine_scene
    local set_spine_pose = L.set_spine_pose
    local characterZ = L.characterZ or 0.0
    local playerVisualFootOffset = L.playerVisualFootOffset or 0.0
    if player == nil or get_spine_scene == nil or set_spine_pose == nil or add_spawned == nil then
        return
    end

    corona_platformer_spine_benchmark_actors = {}
    corona_platformer_spine_benchmark_spawned_count = 0
    local requestedCount = math.max(1, math.min(1000, math.floor(controls.spineSkinningBenchmarkCharacterCount or 50)))
    local assets = corona_platformer_build_spine_benchmark_assets(requestedCount)
    local columns = math.max(1, math.min(requestedCount, math.floor(controls.spineSkinningBenchmarkColumns or 10)))
    local rows = math.max(1, math.ceil(requestedCount / columns))
    local worldScale = math.max(0.001, controls.spineSkinningBenchmarkWorldScale or 0.42)
    local spacingX = math.max(90.0, controls.spineSkinningBenchmarkSpacingX or 138.0)
    local spacingY = math.max(90.0, controls.spineSkinningBenchmarkSpacingY or 132.0)
    local sampleFps = math.max(1.0, controls.spineSkinningBenchmarkSampleFps or 12.0)
    local animation = controls.spineSkinningBenchmarkAnimation or "walk"
    local fallbackAnimation = controls.spineSkinningBenchmarkFallbackAnimation or "idle"
    local baseX = player.x - (columns - 1) * spacingX * 0.5
    local baseY = player.y + playerVisualFootOffset

    for index = 1, requestedCount do
        local asset = assets[((index - 1) % #assets) + 1]
        local row = math.floor((index - 1) / columns)
        local column = (index - 1) % columns
        local phase = (index - 1) * 0.037
        local sampleTime = phase % (loopSeconds[animation] or 1.0)
        local handle, frame, resolved = get_spine_scene(asset.path, animation, sampleTime, sampleFps, nil, true)
        local actorAnimation = animation
        if handle == nil and fallbackAnimation ~= animation then
            actorAnimation = fallbackAnimation
            sampleTime = phase % (loopSeconds[actorAnimation] or 1.0)
            handle, frame, resolved = get_spine_scene(asset.path, actorAnimation, sampleTime, sampleFps, nil, true)
        end
        if handle ~= nil then
            local entity = add_spawned(corona.Entity.create("PlatformerSpineBenchmark_" .. tostring(index)))
            local x = baseX + column * spacingX
            local y = baseY + (rows - row - 1) * spacingY
            set_spine_pose(entity, handle, x, y, characterZ, 0.0, worldScale, 0.72, 0.0, false, true, false)
            table.insert(corona_platformer_spine_benchmark_actors, {
                entity = entity,
                asset = asset,
                animation = actorAnimation,
                phase = phase,
                x = x,
                y = y,
                worldScale = worldScale,
                lastHandle = handle,
                lastFrame = frame or -1,
                lastAnimation = actorAnimation,
            })
            if resolved then
                corona_platformer_spine_benchmark_spawned_count = corona_platformer_spine_benchmark_spawned_count + 1
            end
        end
    end

    corona.log(
        "platformer spine benchmark spawned count=" .. tostring(#corona_platformer_spine_benchmark_actors) ..
        " requested=" .. tostring(requestedCount) ..
        " sample_fps=" .. tostring(sampleFps) ..
        " world_scale=" .. tostring(worldScale) ..
        " gpu_skinning=" .. tostring(controls.spineSkinningBenchmarkGpuSkinning or "command-line"))
end

function corona_platformer_update_spine_benchmark_characters(force)
    local controls = platformer_controls()
    if controls.spineSkinningBenchmark ~= true then
        return
    end

    local L = loc()
    local loopSeconds = L.loopSeconds or {}
    local get_spine_scene = L.get_spine_scene
    local set_spine_pose = L.set_spine_pose
    local characterZ = L.characterZ or 0.0
    if get_spine_scene == nil or set_spine_pose == nil or L.get_time == nil then
        return
    end
    local time = L.get_time()

    local sampleFps = math.max(1.0, controls.spineSkinningBenchmarkSampleFps or 12.0)
    for index, actor in ipairs(corona_platformer_spine_benchmark_actors) do
        local duration = loopSeconds[actor.animation] or 1.0
        local sampleTime = (time + actor.phase) % duration
        local sampleFrame = math.floor(sampleTime * sampleFps + 0.0001)
        local mirrorX = (index % 2) == 0
        if not force and sampleFrame == actor.lastFrame then
            set_spine_pose(actor.entity, nil, actor.x, actor.y, characterZ, 0.0, actor.worldScale or 0.42, 0.72, 0.0, mirrorX, true, false)
        else
            local handle, frame, resolved = get_spine_scene(actor.asset.path, actor.animation, sampleTime, sampleFps, actor.lastHandle, force)
            if handle ~= nil then
                local sceneHandle = nil
                if force or (resolved and frame ~= actor.lastFrame) then
                    sceneHandle = handle
                    if resolved then
                        actor.lastFrame = frame
                        actor.lastHandle = handle
                    end
                end
                set_spine_pose(actor.entity, sceneHandle, actor.x, actor.y, characterZ, 0.0, actor.worldScale or 0.42, 0.72, 0.0, mirrorX, true, false)
            end
        end
    end
end

-- Return an empty table so the script loader's `lua_istable(L, -1)` check
-- passes and finds no `update`/`shutdown`/`imgui`/`ui` fields. Without an
-- explicit table return, the loader falls back to globals — which would
-- otherwise inherit leftover values from a sibling script's chunk.
return {}
