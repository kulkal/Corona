local script = {}

corona_script_controls = corona_script_controls or {}

local DEBUG_BUFFER_ITEMS = {
    "SHADOW",
    "WORLD_NORMAL",
    "GEO_NORMAL",
    "DEPTH",
    "RAW_DIFFUSE_GI",
    "RAW_DIFFUSE_GI_AUX",
    "SCREEN_PROBE_DIFFUSE_GI",
    "SCREEN_PROBE_PROBES",
    "SCREEN_PROBE_HISTORY_LENGTH",
    "SCREEN_PROBE_ATLAS_HISTORY_LENGTH",
    "TEMPORAL_FILTERED_DIFFUSE_GI",
    "RESOLVED_DIFFUSE_GI",
    "FINAL_DIFFUSE_GI",
    "ALBEDO",
    "VELOCITY",
    "ROUGNESS_METALLIC",
    "SPECULAR_RAW",
    "TEMPORAL_FILTERED_SPECULAR",
    "BLOOM",
    "SPEC_HISTORY_LENGTH",
    "RTAO",
    "NO_FULLSCREEN",
}

local function set_default(table_value, name, value)
    if table_value[name] == nil then
        table_value[name] = value
    end
end

local function persistent_default(prefix, name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control(prefix .. "." .. name, value)
    end
    return value
end

local function persist_control(prefix, name, value)
    if corona.set_persistent_control ~= nil then
        corona.set_persistent_control(prefix .. "." .. name, value)
    end
    return value
end

local function set_persistent_default(table_value, prefix, name, value)
    if table_value[name] == nil then
        table_value[name] = persistent_default(prefix, name, value)
    end
end

local function save_persistent_controls(prefix, controls, keys)
    for _, key in ipairs(keys) do
        persist_control(prefix, key, controls[key])
    end
end

local function ensure_pistol_controls()
    corona_script_controls.pistol = corona_script_controls.pistol or {}
    local pistol = corona_script_controls.pistol
    set_persistent_default(pistol, "pistol", "weaponDistance", 96.0)
    set_persistent_default(pistol, "pistol", "weaponRightOffset", 14.0)
    set_persistent_default(pistol, "pistol", "weaponDownOffset", 44.0)
    set_persistent_default(pistol, "pistol", "weaponExtent", 92.0)
    set_persistent_default(pistol, "pistol", "yawOffset", 0.0)
    set_persistent_default(pistol, "pistol", "pitchOffset", -4.0)
    set_persistent_default(pistol, "pistol", "rollOffset", 0.0)
    set_persistent_default(pistol, "pistol", "enabled", true)
    set_default(pistol, "spawned", false)
    return pistol
end

local function ensure_props_controls()
    corona_script_controls.props = corona_script_controls.props or {}
    local props = corona_script_controls.props
    set_persistent_default(props, "props", "buddhaPosition", { 273.0, -12.0, -6.0 })
    set_persistent_default(props, "props", "shaderBallPosition", { 0.0, -4.0, -72.0 })
    set_persistent_default(props, "props", "buddhaBaseYaw", -12.0)
    set_persistent_default(props, "props", "shaderBallBaseYaw", -120.0)
    set_persistent_default(props, "props", "buddhaExtent", 260.0)
    set_persistent_default(props, "props", "shaderBallExtent", 220.0)
    set_persistent_default(props, "props", "buddhaBobHeight", 38.0)
    set_persistent_default(props, "props", "buddhaBobSpeed", 1.35)
    set_persistent_default(props, "props", "spinSpeed", 35.0)
    set_persistent_default(props, "props", "buddhaEnabled", true)
    set_default(props, "buddhaSpawned", false)
    set_persistent_default(props, "props", "shaderBallEnabled", true)
    set_default(props, "shaderBallSpawned", false)
    return props
end

local function ensure_block_character_controls()
    corona_script_controls.blockCharacter = corona_script_controls.blockCharacter or {}
    local character = corona_script_controls.blockCharacter
    set_persistent_default(character, "block_character", "enabled", true)
    set_persistent_default(character, "block_character", "position", { -28.0, -12.0, -48.0 })
    set_persistent_default(character, "block_character", "targetExtent", 105.0)
    set_persistent_default(character, "block_character", "baseYaw", 18.0)
    set_persistent_default(character, "block_character", "pitch", 0.0)
    set_persistent_default(character, "block_character", "roll", 0.0)
    set_persistent_default(character, "block_character", "spinSpeed", 12.0)
    set_persistent_default(character, "block_character", "bobHeight", 3.5)
    set_persistent_default(character, "block_character", "bobSpeed", 1.15)
    set_persistent_default(character, "block_character", "shapeSeed", 20260503)
    set_default(character, "spawned", false)
    set_default(character, "forceRespawn", false)
    return character
end

local function ensure_window_controls()
    corona_script_controls.windows = corona_script_controls.windows or {}
    local windows = corona_script_controls.windows
    set_default(windows, "objectControlsOpen", false)
    set_default(windows, "pointLightsOpen", false)
    set_default(windows, "pointLightAxesVisible", true)
    return windows
end

local function ensure_gpu_controls()
    corona_script_controls.gpu = corona_script_controls.gpu or {}
    local gpu = corona_script_controls.gpu
    set_default(gpu, "showOverlay", false)
    return gpu
end

local function section(title)
    ui.separator()
    ui.text(title)
end

local function reset_pistol(pistol)
    pistol.weaponDistance = 96.0
    pistol.weaponRightOffset = 14.0
    pistol.weaponDownOffset = 44.0
    pistol.weaponExtent = 92.0
    pistol.yawOffset = 0.0
    pistol.pitchOffset = -4.0
    pistol.rollOffset = 0.0
end

local function reset_props(props)
    props.buddhaPosition = { 273.0, -12.0, -6.0 }
    props.shaderBallPosition = { 0.0, -4.0, -72.0 }
    props.buddhaBaseYaw = -12.0
    props.shaderBallBaseYaw = -120.0
    props.buddhaExtent = 260.0
    props.shaderBallExtent = 220.0
    props.buddhaBobHeight = 38.0
    props.buddhaBobSpeed = 1.35
    props.spinSpeed = 35.0
    props.buddhaEnabled = true
    props.shaderBallEnabled = true
end

local function reset_block_character(character)
    character.enabled = true
    character.position = { -28.0, -12.0, -48.0 }
    character.targetExtent = 105.0
    character.baseYaw = 18.0
    character.pitch = 0.0
    character.roll = 0.0
    character.spinSpeed = 12.0
    character.bobHeight = 3.5
    character.bobSpeed = 1.15
    character.shapeSeed = 20260503
    character.forceRespawn = true
end

local function set_value_if_changed(key, old_value, new_value)
    if old_value ~= new_value then
        corona.set_ui_value(key, new_value)
    end
    return new_value
end

local function slider_float_state(state, key, label, id, min_value, max_value)
    local value = state[key]
    if value == nil then
        return nil
    end

    local new_value = ui.slider_float(label, id, value, min_value, max_value)
    if math.abs(new_value - value) > 0.0001 then
        corona.set_ui_value(key, new_value)
    end
    return new_value
end

local function slider_int_state(state, key, label, id, min_value, max_value)
    local value = state[key]
    if value == nil then
        return nil
    end

    local new_value = ui.slider_int(label, id, value, min_value, max_value)
    return set_value_if_changed(key, value, new_value)
end

local function checkbox_state(state, key, label, id)
    local value = state[key]
    if value == nil then
        return nil
    end

    local new_value = ui.checkbox(label, id, value)
    return set_value_if_changed(key, value, new_value)
end

local function combo_state_zero_based(state, key, label, id, items)
    local value = state[key]
    if value == nil then
        return nil
    end

    local new_index = ui.combo(label, id, value + 1, items)
    local new_value = new_index - 1
    return set_value_if_changed(key, value, new_value)
end

local function slider_vec3_state(state, key, label, id, min_value, max_value)
    local value = state[key]
    if value == nil then
        return nil
    end

    local x = value[1] or 0.0
    local y = value[2] or 0.0
    local z = value[3] or 0.0

    ui.text(string.format("%s: %.3f, %.3f, %.3f", label, x, y, z))
    local new_x = ui.slider_float(label .. " X", id .. ".x", x, min_value, max_value)
    local new_y = ui.slider_float(label .. " Y", id .. ".y", y, min_value, max_value)
    local new_z = ui.slider_float(label .. " Z", id .. ".z", z, min_value, max_value)

    if math.abs(new_x - x) > 0.0001 or math.abs(new_y - y) > 0.0001 or math.abs(new_z - z) > 0.0001 then
        corona.set_ui_value(key, { new_x, new_y, new_z })
    end

    return { new_x, new_y, new_z }
end

local function draw_light_direction_gizmo(state)
    local value = state.light_dir
    if value == nil then
        return
    end

    local light_x = value[1] or 0.0
    local light_y = value[2] or 0.0
    local light_z = value[3] or 0.0
    local gizmo_value = { light_z, -light_y, -light_x }

    ui.text(string.format("Light Direction: %.3f, %.3f, %.3f", light_x, light_y, light_z))
    local new_gizmo = ui.gizmo3d("##light_direction_gizmo", "script.lighting.light_dir.gizmo", gizmo_value, 200.0, 200)
    local new_light = {
        -(new_gizmo[3] or 0.0),
        -(new_gizmo[2] or 0.0),
        new_gizmo[1] or 0.0,
    }

    if math.abs(new_light[1] - light_x) > 0.0001 or
        math.abs(new_light[2] - light_y) > 0.0001 or
        math.abs(new_light[3] - light_z) > 0.0001 then
        corona.set_ui_value("light_dir", new_light)
    end
end

local function persistent_slider_float(controls, prefix, key, label, id, min_value, max_value)
    local old_value = controls[key]
    local new_value = ui.slider_float(label, id, old_value, min_value, max_value)
    if math.abs(new_value - old_value) > 0.0001 then
        controls[key] = persist_control(prefix, key, new_value)
    end
    return controls[key]
end

local function persistent_slider_vec3(controls, prefix, key, label, id, min_value, max_value)
    local value = controls[key] or { 0.0, 0.0, 0.0 }
    local x = value[1] or 0.0
    local y = value[2] or 0.0
    local z = value[3] or 0.0

    ui.text(string.format("%s: %.3f, %.3f, %.3f", label, x, y, z))
    local new_x = ui.slider_float(label .. " X", id .. ".x", x, min_value, max_value)
    local new_y = ui.slider_float(label .. " Y", id .. ".y", y, min_value, max_value)
    local new_z = ui.slider_float(label .. " Z", id .. ".z", z, min_value, max_value)

    if math.abs(new_x - x) > 0.0001 or math.abs(new_y - y) > 0.0001 or math.abs(new_z - z) > 0.0001 then
        controls[key] = persist_control(prefix, key, { new_x, new_y, new_z })
    end

    return controls[key]
end

local function draw_spawn_toggle(label, controls, enabled_key, spawned_key, id_prefix, persist_prefix)
    if ui.button(controls[enabled_key] and ("Remove " .. label) or ("Add " .. label), id_prefix .. ".toggle") then
        controls[enabled_key] = not controls[enabled_key]
        if persist_prefix ~= nil then
            persist_control(persist_prefix, enabled_key, controls[enabled_key])
        end
    end
    ui.same_line()
    ui.text(controls[spawned_key] and "in scene" or "hidden")
end

local function update_point_light(light, patch)
    patch.id = light.id
    patch.index = light.index
    corona.set_ui_value("point_light", patch)
end

local function draw_top_controls(state)
    ui.text("Render Backend: " .. (state.backend or "None"))
    combo_state_zero_based(state, "rendering_mode", "Rendering Mode", "script.rendering.mode", {
        "HYBRID (Raster + RT)",
        "PATH TRACING",
    })

    ui.text("Arrow keys : rotate camera imGui\nWASD keys : move camera imGui\nI : show/hide imGui\nB : show/hide buffer visualization\nT : cycle anti-aliasing mode")

    if state.debug_visualization_available then
        checkbox_state(state, "visualize_buffers", "Visualize Buffers", "script.debug.visualize_buffers")
    else
        ui.text("Visualize Buffers is currently available on the DX12 backend.")
    end
    combo_state_zero_based(state, "fullscreen_debug_buffer", "Visualize Full Screen", "script.debug.fullscreen_buffer", DEBUG_BUFFER_ITEMS)
    checkbox_state(state, "draw_histogram", "Draw Histogram", "script.debug.draw_histogram")
end

local function draw_capture_controls(state)
    section("Capture / Profiling")
    local gpu = ensure_gpu_controls()

    if ui.button("Recompile all shaders", "script.capture.recompile") then
        corona.run_ui_command("recompile_shaders")
    end

    if ui.button("Capture Final Backbuffer", "script.capture.final_backbuffer") then
        corona.run_ui_command("capture_final_backbuffer")
    end
    if state.final_screenshot_status ~= nil and state.final_screenshot_status ~= "" then
        ui.text("Screenshot: " .. state.final_screenshot_status)
    end

    slider_int_state(state, "gpu_timing_average_frames", "GPU Timing Average Frames", "script.gpu.average_frames", 1, 240)
    gpu.showOverlay = ui.checkbox("Show Frame Timings On Screen", "script.gpu.show_overlay", gpu.showOverlay)
end

local function draw_gpu_timing_overlay(state)
    local gpu = ensure_gpu_controls()
    if not gpu.showOverlay or state.gpu_passes == nil then
        return
    end

    local total_last = 0.0
    local total_average = 0.0
    local has_frame_total = false
    for _, pass in ipairs(state.gpu_passes) do
        if pass.name == "Frame Total" then
            total_last = pass.last_ms or 0.0
            total_average = pass.average_ms or 0.0
            has_frame_total = true
            break
        end
    end
    if not has_frame_total then
        for _, pass in ipairs(state.gpu_passes) do
            total_last = total_last + (pass.last_ms or 0.0)
            total_average = total_average + (pass.average_ms or 0.0)
        end
    end

    local lines = {
        string.format("GPU frame: %.3f ms (avg %.3f ms)", total_last, total_average),
        string.format(
            "CPU update: %.3f ms (avg %.3f ms)",
            state.cpu_update_last_ms or 0.0,
            state.cpu_update_average_ms or 0.0
        ),
    }

    if state.cpu_update_phases ~= nil then
        for _, phase in ipairs(state.cpu_update_phases) do
            table.insert(lines, string.format(
                "  %s: %.3f ms (avg %.3f)",
                phase.name or "CPU phase",
                phase.last_ms or 0.0,
                phase.average_ms or 0.0
            ))
        end
    end

    local shown = 0
    for _, pass in ipairs(state.gpu_passes) do
        if shown < 10 then
            table.insert(lines, string.format("%s: %.3f ms (avg %.3f)", pass.name, pass.last_ms, pass.average_ms))
            shown = shown + 1
        end
    end

    ui.overlay_text(table.concat(lines, "\n"), 10.0, 30.0)
end

local function draw_camera_path_controls(state)
    section("Camera Path")
    local busy = state.camera_path_recording or state.camera_path_playing or state.camera_path_dumping

    if ui.button("Start Camera Path", "script.camera_path.start") and not busy then
        corona.run_ui_command("start_camera_path")
    end
    ui.same_line()
    if ui.button("End Camera Path", "script.camera_path.end") and state.camera_path_recording then
        corona.run_ui_command("end_camera_path")
    end

    if ui.button("Refresh Paths", "script.camera_path.refresh") and not busy then
        corona.run_ui_command("refresh_camera_paths")
    end

    if state.camera_path_entries ~= nil and #state.camera_path_entries > 0 then
        local selected = state.camera_path_selected_index
        if selected == nil or selected < 1 then
            selected = 1
        end
        local new_selected = ui.combo("Saved Paths", "script.camera_path.saved_paths", selected, state.camera_path_entries)
        if new_selected ~= state.camera_path_selected_index then
            corona.run_ui_command("select_camera_path", new_selected)
        end
    else
        ui.text("No saved paths")
    end

    if ui.button(state.camera_path_playing and "Stop Playback" or "Play Camera Path", "script.camera_path.play") then
        corona.run_ui_command("play_camera_path")
    end
    ui.same_line()
    if ui.button("Load Selected Path", "script.camera_path.load_selected") then
        corona.run_ui_command("load_selected_path")
    end

    if ui.button("Load Latest Path", "script.camera_path.load_latest") then
        corona.run_ui_command("load_latest_path")
    end
    ui.same_line()
    if ui.button("Dump Selected Path", "script.camera_path.dump_selected") then
        corona.run_ui_command("dump_selected_path")
    end

    if ui.button("Play + Dump 30fps PNG Sequence", "script.camera_path.dump") then
        corona.run_ui_command("start_camera_path_dump")
    end
    ui.same_line()
    if ui.button("Stop Playback + Dump", "script.camera_path.stop_dump") then
        corona.run_ui_command("stop_playback_dump")
    end

    if ui.button("Convert Last Dump To MP4", "script.camera_path.convert_mp4") then
        corona.run_ui_command("convert_last_dump_to_mp4")
    end

    ui.text(string.format("Path keyframes: %d, duration: %.2fs", state.camera_path_keyframes or 0, state.camera_path_duration or 0.0))
    if state.camera_path_dumping then
        ui.text(string.format("Dump progress: %d / %d", state.camera_path_dump_frame_index or 0, state.camera_path_dump_frame_count or 0))
    end
    if state.camera_path_status ~= nil and state.camera_path_status ~= "" then
        ui.text("Camera path: " .. state.camera_path_status)
    end
end

local function draw_camera_aa_controls(state)
    section("Camera / Anti-Aliasing")
    slider_float_state(state, "camera_turn_speed", "Camera turn speed", "script.camera.turn_speed", 0.0, 3.14159)
    combo_state_zero_based(state, "aa_mode", "Anti-Aliasing", "script.aa.mode", {
        "Off",
        "TAA",
        "DLSS SR",
        "DLSS RR",
    })

    if state.dlss_available or state.dlss_rr_available then
        combo_state_zero_based(state, "dlss_quality", "DLSS Quality", "script.dlss.quality", {
            "Quality",
            "Balanced",
            "Performance",
            "Ultra Performance",
        })
        slider_float_state(state, "dlss_jitter_phase_scale", "DLSS Jitter Phase Scale", "script.dlss.jitter_scale", 0.25, 16.0)
        slider_int_state(state, "dlss_jitter_phase_override", "DLSS Jitter Phase Override", "script.dlss.jitter_override", 0, 512)
        ui.text(string.format("DLSS SR Available: %s", state.dlss_available and "Yes" or "No"))
        ui.text(string.format("DLSS RR Available: %s", state.dlss_rr_available and "Yes" or "No"))
        ui.text(string.format("Render Resolution: %d x %d", state.render_width or 0, state.render_height or 0))
    else
        ui.text("DLSS SR Available: No")
        ui.text("DLSS RR Available: No")
    end
end

local function draw_path_tracing_controls(state)
    if state.rendering_mode ~= 1 then
        return
    end

    section("Path Tracing")
    slider_int_state(state, "path_max_bounces", "Max Bounces", "script.path.max_bounces", 1, 8)
    slider_int_state(state, "path_samples_per_pixel", "Samples Per Pixel", "script.path.samples_per_pixel", 1, 16)
    slider_int_state(state, "path_direct_light_samples", "Sun Samples", "script.path.direct_light_samples", 1, 8)
    ui.text(string.format("Accumulated Frames: %d", state.frame_counter or 0))
    if ui.button("Reset Accumulation", "script.path.reset_accumulation") then
        corona.run_ui_command("reset_accumulation")
    end
    combo_state_zero_based(state, "path_debug_mode", "Debug Mode", "script.path.debug_mode", {
        "None",
        "Albedo",
        "Normal",
        "Roughness",
        "Metallic",
        "World Position",
        "Barycentric",
    })
end

local function draw_lighting_controls(state)
    section("Lighting / GI")
    checkbox_state(state, "enable_direct_diffuse", "Enable Direct Diffuse", "script.lighting.direct_diffuse")
    checkbox_state(state, "enable_direct_specular", "Enable Direct Specular", "script.lighting.direct_specular")
    checkbox_state(state, "enable_specular_gi", "Enable Indirect Specular (GI)", "script.lighting.specular_gi")
    if state.rt_reflection_ser_available then
        checkbox_state(state, "rt_reflection_ser", "RT Reflection SER", "script.lighting.reflection.ser")
    end
    if state.camera_path_owns_light_controls then
        ui.text("Camera path playback is controlling the directional light.")
    else
        draw_light_direction_gizmo(state)
        slider_float_state(state, "light_intensity", "Light Intensity", "script.lighting.light_intensity", 0.0, 30.0)
    end
    slider_float_state(state, "sun_angular_radius", "Sun Angular Radius", "script.lighting.sun_radius", 0.0, 0.03)

    if state.rendering_mode == 0 then
        slider_int_state(state, "hybrid_shadow_samples", "Hybrid Shadow Samples", "script.lighting.shadow_samples", 1, 16)
        checkbox_state(state, "enable_rtao", "Enable RTAO", "script.lighting.rtao.enable")
        if state.enable_rtao then
            slider_int_state(state, "rtao_samples", "RTAO Samples", "script.lighting.rtao.samples", 1, 16)
            slider_float_state(state, "rtao_radius", "RTAO Radius", "script.lighting.rtao.radius", 2.0, 256.0)
            slider_float_state(state, "rtao_power", "RTAO Power", "script.lighting.rtao.power", 0.25, 4.0)
            slider_float_state(state, "rtao_normal_bias", "RTAO Normal Bias", "script.lighting.rtao.normal_bias", 0.01, 2.0)
        end
    end

    checkbox_state(state, "enable_diffuse_gi", "Enable Diffuse GI", "script.lighting.diffuse_gi")
    if state.rt_diffuse_gi_ser_available then
        checkbox_state(state, "rt_diffuse_gi_ser", "RT Diffuse GI SER", "script.lighting.diffuse_gi.ser")
    end
    if state.rendering_mode == 0 then
        combo_state_zero_based(state, "diffuse_gi_mode", "Diffuse GI Method", "script.lighting.diffuse_gi.mode", {
            "Simple Raytrace",
            "Spatial Hash",
            "Screen Probe",
        })
    else
        ui.text("Diffuse GI method selection is used by Hybrid rendering.")
    end

    if state.diffuse_gi_mode == 2 and state.rendering_mode == 0 then
        slider_int_state(state, "screen_probe_spacing", "Screen Probe Spacing", "script.screen_probe.spacing", 4, 64)
        slider_int_state(state, "screen_probe_gather_radius", "Screen Probe Gather Radius", "script.screen_probe.gather_radius", 1, 3)
        slider_int_state(state, "screen_probe_rays_per_probe", "Screen Probe Rays / Probe", "script.screen_probe.rays_per_probe", 1, 4)
        combo_state_zero_based(state, "screen_probe_sh_mode", "Screen Probe SH Coefficients", "script.screen_probe.sh_mode", {
            "4 coeffs (L0 + L1)",
            "9 coeffs (L0 + L1 + L2)",
        })
        slider_float_state(state, "screen_probe_raw_blend", "Screen Probe Raw Blend", "script.screen_probe.raw_blend", 0.0, 0.35)
        slider_float_state(state, "screen_probe_resolve_depth", "Screen Probe Resolve Depth", "script.screen_probe.resolve_depth", 1.0, 96.0)
        slider_float_state(state, "screen_probe_resolve_normal", "Screen Probe Resolve Normal", "script.screen_probe.resolve_normal", 1.0, 96.0)
        slider_float_state(state, "screen_probe_edge_depth", "Screen Probe Edge Depth", "script.screen_probe.edge_depth", 8.0, 192.0)
        slider_float_state(state, "screen_probe_edge_normal", "Screen Probe Edge Normal", "script.screen_probe.edge_normal", 1.0, 96.0)
        slider_int_state(state, "screen_probe_edge_samples", "Screen Probe Edge Samples", "script.screen_probe.edge_samples", 1, 4)
    elseif state.diffuse_gi_mode == 1 and state.rendering_mode == 0 then
        slider_float_state(state, "spatial_hash_cell_size", "Spatial Hash Cell Size", "script.spatial_hash.cell_size", 4.0, 256.0)
        slider_int_state(state, "spatial_hash_rays_per_cell", "Spatial Hash Rays / Cell", "script.spatial_hash.rays_per_cell", 1, 8)
        slider_int_state(state, "spatial_hash_max_bounces", "Spatial Hash Max Bounces", "script.spatial_hash.max_bounces", 1, 8)
        slider_float_state(state, "spatial_hash_interpolation", "Spatial Hash Interpolation", "script.spatial_hash.interpolation", 0.0, 1.0)
        slider_float_state(state, "spatial_hash_smoothing", "Spatial Hash Smoothing", "script.spatial_hash.smoothing", 0.0, 1.0)
        slider_float_state(state, "spatial_hash_temporal_alpha", "Spatial Hash Temporal Alpha", "script.spatial_hash.temporal_alpha", 0.02, 1.0)
    end

    combo_state_zero_based(state, "ray_noise_mode", "RT Noise", "script.lighting.ray_noise", {
        "Blue Noise",
        "R2 Low Discrepancy",
        "Stable Hash",
    })
end

local function draw_sky_controls(state)
    section("Sky / Environment")
    local sky_enabled = checkbox_state(state, "enable_sky_lighting", "Enable Sky Lighting", "script.sky.enable")
    if sky_enabled then
        slider_float_state(state, "sky_lighting_strength", "Sky Lighting Strength", "script.sky.strength", 0.0, 1.0)
        local rt_sky_enabled = checkbox_state(state, "enable_ray_traced_sky_lighting", "Ray Traced Sky Pass", "script.sky.rt_pass")
        if rt_sky_enabled then
            slider_int_state(state, "sky_lighting_samples", "Sky Lighting Samples", "script.sky.samples", 1, 32)
            slider_float_state(state, "sky_lighting_ray_length", "Sky Lighting Ray Length", "script.sky.ray_length", 16.0, 10000.0)
            slider_float_state(state, "sky_lighting_normal_bias", "Sky Lighting Normal Bias", "script.sky.normal_bias", 0.01, 4.0)
            slider_float_state(state, "sky_lighting_up_bias", "Sky Lighting Up Bias", "script.sky.up_bias", 0.0, 1.0)
            slider_float_state(state, "sky_lighting_direction_power", "Sky Lighting Direction Power", "script.sky.direction_power", 0.25, 8.0)
            slider_float_state(state, "sky_lighting_min_world_y", "Sky Lighting Min World Y", "script.sky.min_world_y", -0.25, 0.75)
        else
            ui.text("Diffuse GI handles sky contribution")
        end
    end

    slider_float_state(state, "surface_bounce_strength", "Surface Bounce Strength", "script.sky.surface_bounce_strength", 0.0, 1.0)
    slider_float_state(state, "surface_bounce_saturation", "Surface Bounce Saturation", "script.sky.surface_bounce_saturation", 0.0, 1.0)
    slider_float_state(state, "sky_intensity", "Sky Intensity", "script.sky.intensity", 0.0, 10.0)
    checkbox_state(state, "enable_prefiltered_env_specular", "Prefiltered Env Specular", "script.sky.prefiltered_env")
    slider_float_state(state, "prefiltered_env_roughness_threshold", "Prefiltered Env Roughness Threshold", "script.sky.prefilter_threshold", 0.02, 1.0)
    slider_float_state(state, "prefiltered_env_roughness_fade", "Prefiltered Env Roughness Fade", "script.sky.prefilter_fade", 0.0, 0.5)
end

local function draw_tonemap_controls(state)
    section("Tone Mapping")
    combo_state_zero_based(state, "tone_map_mode", "Tone Map Operator", "script.tonemap.mode", {
        "LINEAR_TO_SRGB",
        "REINHARD",
        "FILMIC_ALU",
        "FILMIC_HABLE",
    })

    if state.tone_map_mode == 3 then
        slider_float_state(state, "tonemap_whitepoint_hejl", "WhitePoint_Hejl", "script.tonemap.whitepoint_hejl", 0.1, 5.0)
        slider_float_state(state, "tonemap_shoulder_strength", "ShoulderStrength", "script.tonemap.shoulder", 0.1, 10.0)
        slider_float_state(state, "tonemap_linear_strength", "LinearStrength", "script.tonemap.linear_strength", 0.1, 10.0)
        slider_float_state(state, "tonemap_linear_angle", "LinearAngle", "script.tonemap.linear_angle", 0.1, 20.0)
        slider_float_state(state, "tonemap_toe_strength", "ToeStrength", "script.tonemap.toe", 0.1, 20.0)
        slider_float_state(state, "tonemap_whitepoint_hable", "WhitePoint_Hable", "script.tonemap.whitepoint_hable", 0.1, 20.0)
    end
end

local function draw_object_controls_window(windows, pistol, props, block_character)
    if not windows.objectControlsOpen then
        return
    end

    ui.begin_window("Object Controls")

    if ui.button("Close", "script.objects.close") then
        windows.objectControlsOpen = false
    end

    section("Scene Objects")
    draw_spawn_toggle("Buddha", props, "buddhaEnabled", "buddhaSpawned", "script.objects.buddha", "props")
    draw_spawn_toggle("ShaderBall", props, "shaderBallEnabled", "shaderBallSpawned", "script.objects.shaderball", "props")
    draw_spawn_toggle("Pistol", pistol, "enabled", "spawned", "script.objects.pistol", "pistol")
    draw_spawn_toggle("Procedural Character", block_character, "enabled", "spawned", "script.objects.block_character", "block_character")

    section("Buddha")
    persistent_slider_vec3(props, "props", "buddhaPosition", "Buddha Position", "script.props.buddha.position", -1000.0, 1000.0)
    persistent_slider_float(props, "props", "buddhaExtent", "Buddha Size", "script.props.buddha.size", 20.0, 500.0)
    persistent_slider_float(props, "props", "buddhaBaseYaw", "Buddha Yaw", "script.props.buddha.yaw", -180.0, 180.0)
    persistent_slider_float(props, "props", "buddhaBobHeight", "Buddha Bob Height", "script.props.buddha.bob_height", 0.0, 120.0)
    persistent_slider_float(props, "props", "buddhaBobSpeed", "Buddha Bob Speed", "script.props.buddha.bob_speed", 0.0, 6.0)

    section("ShaderBall")
    persistent_slider_vec3(props, "props", "shaderBallPosition", "ShaderBall Position", "script.props.shaderball.position", -1000.0, 1000.0)
    persistent_slider_float(props, "props", "shaderBallExtent", "ShaderBall Size", "script.props.shaderball.size", 20.0, 500.0)
    persistent_slider_float(props, "props", "shaderBallBaseYaw", "ShaderBall Yaw", "script.props.shaderball.yaw", -180.0, 180.0)
    persistent_slider_float(props, "props", "spinSpeed", "Shared Spin Speed", "script.props.spin_speed", -180.0, 180.0)
    if ui.button("Reset Buddha / ShaderBall", "script.props.reset") then
        reset_props(props)
        save_persistent_controls("props", props, {
            "buddhaPosition",
            "shaderBallPosition",
            "buddhaBaseYaw",
            "shaderBallBaseYaw",
            "buddhaExtent",
            "shaderBallExtent",
            "buddhaBobHeight",
            "buddhaBobSpeed",
            "spinSpeed",
            "buddhaEnabled",
            "shaderBallEnabled",
        })
    end

    section("Pistol / View model")
    persistent_slider_float(pistol, "pistol", "weaponDistance", "Pistol Distance", "script.pistol.distance", 40.0, 180.0)
    persistent_slider_float(pistol, "pistol", "weaponRightOffset", "Pistol Right offset", "script.pistol.right", -80.0, 80.0)
    persistent_slider_float(pistol, "pistol", "weaponDownOffset", "Pistol Down offset", "script.pistol.down", 0.0, 100.0)
    persistent_slider_float(pistol, "pistol", "weaponExtent", "Pistol Size", "script.pistol.size", 30.0, 150.0)
    persistent_slider_float(pistol, "pistol", "pitchOffset", "Pistol Pitch offset", "script.pistol.pitch", -30.0, 30.0)
    if ui.button("Reset pistol", "script.pistol.reset") then
        reset_pistol(pistol)
        save_persistent_controls("pistol", pistol, {
            "weaponDistance",
            "weaponRightOffset",
            "weaponDownOffset",
            "weaponExtent",
            "yawOffset",
            "pitchOffset",
            "rollOffset",
            "enabled",
        })
    end

    section("Procedural Character")
    persistent_slider_vec3(block_character, "block_character", "position", "Character Position", "script.block_character.position", -500.0, 500.0)
    persistent_slider_float(block_character, "block_character", "targetExtent", "Character Size", "script.block_character.size", 20.0, 240.0)
    persistent_slider_float(block_character, "block_character", "baseYaw", "Character Yaw", "script.block_character.yaw", -180.0, 180.0)
    persistent_slider_float(block_character, "block_character", "pitch", "Character Pitch", "script.block_character.pitch", -90.0, 90.0)
    persistent_slider_float(block_character, "block_character", "roll", "Character Roll", "script.block_character.roll", -90.0, 90.0)
    persistent_slider_float(block_character, "block_character", "spinSpeed", "Character Spin Speed", "script.block_character.spin_speed", -180.0, 180.0)
    persistent_slider_float(block_character, "block_character", "bobHeight", "Character Bob Height", "script.block_character.bob_height", 0.0, 80.0)
    persistent_slider_float(block_character, "block_character", "bobSpeed", "Character Bob Speed", "script.block_character.bob_speed", 0.0, 6.0)
    ui.text("Character Shape Seed: " .. tostring(block_character.shapeSeed))
    if ui.button("Randomize character shape", "script.block_character.randomize") then
        local seed = math.floor(block_character.shapeSeed or 20260503) + 1
        if seed > 2147480000 then
            seed = 20260503
        end
        block_character.shapeSeed = persist_control("block_character", "shapeSeed", seed)
        block_character.forceRespawn = true
    end
    if ui.button("Reset character", "script.block_character.reset") then
        reset_block_character(block_character)
        save_persistent_controls("block_character", block_character, {
            "enabled",
            "position",
            "targetExtent",
            "baseYaw",
            "pitch",
            "roll",
            "spinSpeed",
            "bobHeight",
            "bobSpeed",
            "shapeSeed",
        })
    end

    ui.end_window()
end

local function draw_point_lights_window(windows, state)
    if not windows.pointLightsOpen then
        return
    end

    ui.begin_window("Point Lights")

    if ui.button("Close", "script.point_lights.close") then
        windows.pointLightsOpen = false
    end

    local lights = state.point_lights or {}
    local max_count = state.point_light_max_count or 0
    ui.text(string.format("Point Lights: %d / %d", #lights, max_count))
    windows.pointLightAxesVisible = ui.checkbox(
        "Show World Axes",
        "script.point_lights.show_world_axes",
        windows.pointLightAxesVisible == true
    )

    if ui.button("Add Point Light", "script.point_lights.add") then
        corona.run_ui_command("add_point_light")
    end
    ui.same_line()
    if ui.button("Remove Last", "script.point_lights.remove_last") then
        corona.run_ui_command("remove_point_light")
    end
    ui.same_line()
    if ui.button("Clear", "script.point_lights.clear") then
        corona.run_ui_command("clear_point_lights")
    end

    for _, light in ipairs(lights) do
        local id = light.id or light.index or 0
        local prefix = "script.point_lights." .. tostring(id)
        section("Point Light " .. tostring(light.index or id))

        local enabled = ui.checkbox("Enabled", prefix .. ".enabled", light.enabled == true)
        if enabled ~= light.enabled then
            update_point_light(light, { enabled = enabled })
        end

        local position = light.position or { 0.0, 0.0, 0.0 }
        local x = position[1] or 0.0
        local y = position[2] or 0.0
        local z = position[3] or 0.0
        local new_x = ui.slider_float("Position X", prefix .. ".pos_x", x, -2500.0, 2500.0)
        local new_y = ui.slider_float("Position Y", prefix .. ".pos_y", y, -500.0, 1500.0)
        local new_z = ui.slider_float("Position Z", prefix .. ".pos_z", z, -2500.0, 2500.0)
        if math.abs(new_x - x) > 0.001 or math.abs(new_y - y) > 0.001 or math.abs(new_z - z) > 0.001 then
            update_point_light(light, { position = { new_x, new_y, new_z } })
        end

        local radius = light.radius or 1.0
        local new_radius = ui.slider_float("Radius", prefix .. ".radius", radius, 1.0, 2000.0)
        if math.abs(new_radius - radius) > 0.001 then
            update_point_light(light, { radius = new_radius })
        end

        if windows.pointLightAxesVisible == true and enabled == true then
            local axis_length = math.min(math.max(new_radius * 0.25, 50.0), 220.0)
            ui.world_axis(prefix .. ".world_axis", { new_x, new_y, new_z }, axis_length, 2.0)
        end

        local intensity = light.intensity or 0.0
        local new_intensity = ui.slider_float("Intensity", prefix .. ".intensity", intensity, 0.0, 100.0)
        if math.abs(new_intensity - intensity) > 0.001 then
            update_point_light(light, { intensity = new_intensity })
        end

        local color = light.color or { 1.0, 1.0, 1.0 }
        local r = color[1] or 1.0
        local g = color[2] or 1.0
        local b = color[3] or 1.0
        local new_r = ui.slider_float("Color R", prefix .. ".color_r", r, 0.0, 2.0)
        local new_g = ui.slider_float("Color G", prefix .. ".color_g", g, 0.0, 2.0)
        local new_b = ui.slider_float("Color B", prefix .. ".color_b", b, 0.0, 2.0)
        if math.abs(new_r - r) > 0.001 or math.abs(new_g - g) > 0.001 or math.abs(new_b - b) > 0.001 then
            update_point_light(light, { color = { new_r, new_g, new_b } })
        end

        if ui.button("Remove Point Light", prefix .. ".remove") then
            corona.run_ui_command("remove_point_light", id)
        end
    end

    ui.end_window()
end

function script.ui()
    local state = corona.get_ui_state()
    if state == nil then
        return
    end

    local windows = ensure_window_controls()
    local props = ensure_props_controls()
    local pistol = ensure_pistol_controls()
    local block_character = ensure_block_character_controls()

    draw_top_controls(state)
    draw_capture_controls(state)
    draw_gpu_timing_overlay(state)
    draw_camera_path_controls(state)

    section("Script Objects")
    if ui.button(windows.objectControlsOpen and "Hide Object Controls" or "Object Controls", "script.objects.open") then
        windows.objectControlsOpen = not windows.objectControlsOpen
    end
    ui.same_line()
    ui.text("Buddha / ShaderBall / Pistol / Character")
    if ui.button(windows.pointLightsOpen and "Hide Point Lights" or "Point Lights", "script.point_lights.open") then
        windows.pointLightsOpen = not windows.pointLightsOpen
    end
    ui.same_line()
    ui.text("Add / Remove / Move / Radius / Intensity")

    draw_object_controls_window(windows, pistol, props, block_character)
    draw_point_lights_window(windows, state)
    draw_camera_aa_controls(state)
    draw_path_tracing_controls(state)
    draw_lighting_controls(state)
    draw_sky_controls(state)
    draw_tonemap_controls(state)

    if state.shader_error ~= nil and state.shader_error ~= "" then
        section("Shader Error")
        ui.text(state.shader_error)
        if ui.button("Compile again", "script.shader_error.compile") then
            corona.run_ui_command("recompile_shaders")
        end
        ui.same_line()
        if ui.button("Close", "script.shader_error.close") then
            corona.run_ui_command("clear_shader_error")
        end
    end
end

return script
