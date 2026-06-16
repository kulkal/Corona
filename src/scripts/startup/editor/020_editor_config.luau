local DEBUG_BUFFERS = {
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

local RENDERING_MODES = {
	"HYBRID (Raster + RT)",
	"PATH TRACING",
}

local AA_MODES = {
	"Off",
	"TAA",
	"DLSS SR",
	"DLSS RR",
}

local DIFFUSE_GI_MODES = {
	"Simple Raytrace",
	"Spatial Hash",
	"Screen Probe",
}

local DIRECT_SHADOW_MODES = {
	"4-channel (top 3 local lights)",
	"ReSTIR DI",
}

local DLSS_QUALITY_MODES = {
	"Quality",
	"Balanced",
	"Performance",
	"Ultra Performance",
}

local DLSS_SR_PRESETS = {
	"Default",
	"Preset J",
	"Preset K",
	"Preset L",
	"Preset M",
}

local DLSS_SR_PRESET_VALUES = {
	0,
	10,
	11,
	12,
	13,
}

local DLSS_RR_PRESETS = {
	"Default",
	"Preset D",
	"Preset E",
}

local DLSS_RR_PRESET_VALUES = {
	0,
	4,
	5,
}

local SPATIAL_HASH_GI_VARIANTS = {
	"SH4 (spherical)",
	"Octahedral DDGI",
}

local function set_value(name, value, changed)
	if changed then
		corona.set_ui_value(name, value)
	end
end

local function checkbox(label, name, value)
	local next_value, changed = imgui.checkbox(label, value == true)
	set_value(name, next_value, changed)
	return next_value, changed
end

local function slider_float(label, name, value, min_value, max_value, format)
	local next_value, changed = imgui.slider_float(label, value or 0.0, min_value, max_value, format or "%.2f")
	set_value(name, next_value, changed)
	return next_value, changed
end

local function slider_int(label, name, value, min_value, max_value)
	local next_value, changed = imgui.slider_int(label, value or min_value, min_value, max_value)
	set_value(name, next_value, changed)
	return next_value, changed
end

local function combo_zero_based(label, name, value, items)
	local index = (value or 0) + 1
	local next_index, changed = imgui.combo(label, index, items)
	if changed then
		corona.set_ui_value(name, next_index - 1)
	end
	return next_index - 1, changed
end

local function combo_values(label, name, value, items, values)
	local index = 1
	for i, item_value in ipairs(values) do
		if item_value == value then
			index = i
			break
		end
	end

	local next_index, changed = imgui.combo(label, index, items)
	local next_value = values[next_index] or values[1]
	if changed then
		corona.set_ui_value(name, next_value)
	end
	return next_value, changed
end

local function color_edit3(label, name, value)
	local next_value, changed = imgui.color_edit3(label, value or { 0.0, 0.0, 0.0 })
	set_value(name, next_value, changed)
	return next_value, changed
end

local function begin_disabled(disabled)
	if disabled then
		imgui.begin_disabled(true)
	end
end

local function end_disabled(disabled)
	if disabled then
		imgui.end_disabled()
	end
end

local function draw_top(state)
	if not imgui.collapsing_header("Top", true) then
		return
	end

	checkbox("Frame Timing Overlay (CPU / GPU / Recording)", "show_frame_timing_overlay", state.show_frame_timing_overlay)
	slider_int("Timing Average Frames", "gpu_timing_average_frames", state.gpu_timing_average_frames, 1, 240)
	checkbox("Full Render Controls Window", "show_imgui", state.show_imgui)
	checkbox("Culling Overlay", "show_culling_overlay", state.show_culling_overlay)

	imgui.separator()
	imgui.text("Visualization")
	local debug_available = state.debug_visualization_available == true
	begin_disabled(not debug_available)
	checkbox("Visualize Buffers", "visualize_buffers", state.visualize_buffers)
	combo_zero_based("Full Screen Buffer", "fullscreen_debug_buffer", state.fullscreen_debug_buffer, DEBUG_BUFFERS)
	end_disabled(not debug_available)
	if not debug_available then
		corona.set_ui_value("visualize_buffers", false)
		imgui.text_disabled("Buffer visualization is available on the DX12 debug path.")
	end

	if imgui.button("Recompile all shaders") then
		corona.run_ui_command("recompile_shaders")
	end
end

local function draw_render_aa(state)
	if not imgui.collapsing_header("Top / Render & AA", true) then
		return
	end

	combo_zero_based("Rendering Mode", "rendering_mode", state.rendering_mode, RENDERING_MODES)

	local aa_index = (state.aa_mode or 0) + 1
	local next_aa, aa_changed = imgui.combo("Anti-Aliasing##editor_config_aa_script", aa_index, AA_MODES)
	if aa_changed then
		corona.set_ui_value("aa_mode", next_aa - 1)
	end

	combo_zero_based("DLSS Quality##editor_config_dlss_quality", "dlss_quality", state.dlss_quality, DLSS_QUALITY_MODES)
	slider_float("DLSS SR Jitter Phase Scale##editor_config_dlss_jitter_scale", "dlss_jitter_phase_scale", state.dlss_jitter_phase_scale, 0.25, 16.0, "%.2f")
	slider_int("DLSS SR Jitter Phase Override##editor_config_dlss_jitter_override", "dlss_jitter_phase_override", state.dlss_jitter_phase_override, 0, 512)
	slider_float("DLSS RR Jitter Phase Scale##editor_config_dlss_rr_jitter_scale", "dlss_rr_jitter_phase_scale", state.dlss_rr_jitter_phase_scale, 0.25, 16.0, "%.2f")
	slider_int("DLSS RR Jitter Phase Override##editor_config_dlss_rr_jitter_override", "dlss_rr_jitter_phase_override", state.dlss_rr_jitter_phase_override, 0, 512)
	slider_float("DLSS SR Sharpness##editor_config_dlss_sr_sharpness", "dlss_sr_sharpness", state.dlss_sr_sharpness, 0.0, 1.0, "%.2f")
	slider_float("DLSS RR Sharpness##editor_config_dlss_rr_sharpness", "dlss_rr_sharpness", state.dlss_rr_sharpness, 0.0, 1.0, "%.2f")
	combo_values("DLSS SR Preset Override##editor_config_dlss_sr_preset", "dlss_sr_preset", state.dlss_sr_preset, DLSS_SR_PRESETS, DLSS_SR_PRESET_VALUES)
	combo_values("DLSS RR Preset Override##editor_config_dlss_rr_preset", "dlss_rr_preset", state.dlss_rr_preset, DLSS_RR_PRESETS, DLSS_RR_PRESET_VALUES)
	imgui.text(string.format("DLSS SR Available: %s", state.dlss_available and "Yes" or "No"))
	imgui.text(string.format("DLSS RR Available: %s", state.dlss_rr_available and "Yes" or "No"))
	imgui.text(string.format("Render Resolution: %d x %d", state.render_width or 0, state.render_height or 0))

	if not state.dlss_available and not state.dlss_rr_available then
		if state.streamline_disabled then
			imgui.text_disabled("DLSS modes are unavailable because Streamline is disabled for this run.")
		elseif not state.backend_d3d12 then
			imgui.text_disabled("DLSS modes require the DX12 backend.")
		else
			imgui.text_disabled("DLSS modes are unavailable on this session.")
		end
	end
end

local function draw_direct_lighting(state)
	if not imgui.tree_node("Direct Lighting", true) then
		return
	end

	checkbox("Enable Direct Diffuse", "enable_direct_diffuse", state.enable_direct_diffuse)
	checkbox("Enable Direct Specular", "enable_direct_specular", state.enable_direct_specular)

	local light_locked = state.camera_path_owns_light_controls == true
	begin_disabled(light_locked)
	local light_dir = state.light_dir or { 0.0, 1.0, 0.0 }
	local editor_dir = { light_dir[3] or 0.0, -(light_dir[2] or 0.0), -(light_dir[1] or 0.0) }
	local next_editor_dir, dir_changed = imgui.gizmo3d("##editor_config_directional_light_gizmo_luau", editor_dir, 180)
	if dir_changed then
		corona.set_ui_value("light_dir", { -(next_editor_dir[3] or 0.0), -(next_editor_dir[2] or 0.0), next_editor_dir[1] or 0.0 })
	end
	imgui.same_line()
	imgui.text("Direction")
	slider_float("Light Brightness", "light_intensity", state.light_intensity, 0.0, 20.0, "%.2f")
	slider_float("Sun Angular Radius", "sun_angular_radius", state.sun_angular_radius, 0.0, 0.03, "%.4f rad")
	end_disabled(light_locked)
	if light_locked then
		imgui.text_disabled("Camera path playback is controlling the directional light.")
	end

	if state.rendering_mode == 0 then
		slider_int("Hybrid Shadow Samples", "hybrid_shadow_samples", state.hybrid_shadow_samples, 1, 16)
		local shadow_mode = state.enable_restir_direct_shadow and 1 or 0
		local next_mode, changed = imgui.combo("Direct Shadow Mode##editor_config_luau", shadow_mode + 1, DIRECT_SHADOW_MODES)
		if changed then
			corona.set_ui_value("enable_restir_direct_shadow", (next_mode - 1) == 1)
		end
	else
		slider_int("Path Tracing Sun Samples", "path_direct_light_samples", state.path_direct_light_samples, 1, 8)
		slider_int("Path Tracing Point Light Samples (0=All)", "path_point_light_samples", state.path_point_light_samples, 0, 16)
	end

	imgui.tree_pop()
end

local function draw_specular_gi(state)
	if not imgui.tree_node("Specular GI / Reflections", true) then
		return
	end

	checkbox("Enable Indirect Specular (GI)", "enable_specular_gi", state.enable_specular_gi)
	begin_disabled(not state.rt_reflection_ser_available)
	checkbox("RT Reflection SER", "rt_reflection_ser", state.rt_reflection_ser)
	end_disabled(not state.rt_reflection_ser_available)

	imgui.tree_pop()
end

local function draw_sky_lighting(state)
	if state.rendering_mode ~= 0 or not imgui.tree_node("Sky Lighting", true) then
		return
	end

	local sky_enabled = checkbox("Enable Sky Lighting", "enable_sky_lighting", state.enable_sky_lighting)
	if sky_enabled then
		slider_float("Sky Lighting Strength", "sky_lighting_strength", state.sky_lighting_strength, 0.0, 1.0, "%.2f")
		local rt_sky = checkbox("Ray Traced Sky Pass", "enable_ray_traced_sky_lighting", state.enable_ray_traced_sky_lighting)
		if rt_sky then
			slider_int("Sky Lighting Samples", "sky_lighting_samples", state.sky_lighting_samples, 1, 32)
			slider_float("Sky Lighting Ray Length", "sky_lighting_ray_length", state.sky_lighting_ray_length, 16.0, 10000.0, "%.1f")
			slider_float("Sky Lighting Normal Bias", "sky_lighting_normal_bias", state.sky_lighting_normal_bias, 0.01, 4.0, "%.2f")
			slider_float("Sky Lighting Up Bias", "sky_lighting_up_bias", state.sky_lighting_up_bias, 0.0, 1.0, "%.2f")
			slider_float("Sky Lighting Direction Power", "sky_lighting_direction_power", state.sky_lighting_direction_power, 0.25, 8.0, "%.2f")
			slider_float("Sky Lighting Min World Y", "sky_lighting_min_world_y", state.sky_lighting_min_world_y, -0.25, 0.75, "%.2f")
		end
	end

	imgui.tree_pop()
end

local function draw_diffuse_gi(state)
	if not imgui.tree_node("Diffuse GI", true) then
		return
	end

	checkbox("Enable Diffuse GI", "enable_diffuse_gi", state.enable_diffuse_gi)
	slider_int("Simple GI Samples/Pixel", "simple_gi_samples_per_pixel", state.simple_gi_samples_per_pixel, 1, 8)
	local disocclusion = checkbox("GI Disocclusion Filter (DLSS-RR)", "enable_gi_disocclusion_filter", state.enable_gi_disocclusion_filter)
	if disocclusion and not state.feed_raw_gi_to_rr then
		slider_int("  Disoccl Filter Radius", "diffuse_gi_spatial_filter_radius", state.diffuse_gi_spatial_filter_radius, 1, 8)
	end
	local feed_raw = checkbox("Feed RAW GI to DLSS-RR (unstable in dark scenes)", "feed_raw_gi_to_rr", state.feed_raw_gi_to_rr)
	if feed_raw then
		local simple_filter = checkbox("  Simple GI Spatial Filter (DLSS-RR)", "enable_simple_gi_spatial_filter", state.enable_simple_gi_spatial_filter)
		if simple_filter then
			slider_int("    GI Spatial Filter Radius", "diffuse_gi_spatial_filter_radius", state.diffuse_gi_spatial_filter_radius, 1, 8)
		end
	end

	begin_disabled(not state.rt_diffuse_gi_ser_available)
	checkbox("RT Diffuse GI SER", "rt_diffuse_gi_ser", state.rt_diffuse_gi_ser)
	end_disabled(not state.rt_diffuse_gi_ser_available)

	begin_disabled(state.rendering_mode ~= 0)
	combo_zero_based("Method##editor_config_diffuse_gi_luau", "diffuse_gi_mode", state.diffuse_gi_mode, DIFFUSE_GI_MODES)
	end_disabled(state.rendering_mode ~= 0)

	if state.rendering_mode == 0 and state.diffuse_gi_mode == 2 then
		slider_int("Screen Probe Spacing", "screen_probe_spacing", state.screen_probe_spacing, 4, 64)
		slider_int("Screen Probe Rays / Probe", "screen_probe_rays_per_probe", state.screen_probe_rays_per_probe, 1, 4)
		slider_float("Screen Probe Raw Blend", "screen_probe_raw_blend", state.screen_probe_raw_blend, 0.0, 0.35, "%.3f")
	elseif state.rendering_mode == 0 and state.diffuse_gi_mode == 1 then
		combo_zero_based("Spatial Hash GI Variant", "spatial_hash_gi_mode", state.spatial_hash_gi_mode, SPATIAL_HASH_GI_VARIANTS)
		slider_float("Spatial Hash Cell Size", "spatial_hash_cell_size", state.spatial_hash_cell_size, 4.0, 256.0, "%.2f")
		slider_int("Spatial Hash Rays / Cell", "spatial_hash_rays_per_cell", state.spatial_hash_rays_per_cell, 1, 8)
		slider_int("Spatial Hash Max Bounces", "spatial_hash_max_bounces", state.spatial_hash_max_bounces, 1, 8)
		slider_float("Oct Near Convergence Bias", "spatial_hash_oct_near_convergence_bias", state.spatial_hash_oct_near_convergence_bias, 0.0, 1.0, "%.2f")
		slider_float("Cell Evict Distance Weight", "spatial_hash_evict_distance_weight", state.spatial_hash_evict_distance_weight, 0.0, 1.0, "%.2f")
		local distance_levels = checkbox("Distance Cell Levels (SHaRC)", "spatial_hash_distance_cell_levels", state.spatial_hash_distance_cell_levels)
		if distance_levels then
			slider_float("Level Base Distance", "spatial_hash_level_base_distance", state.spatial_hash_level_base_distance, 100.0, 3000.0, "%.0f")
		end
		local deep_seed = checkbox("SHC Primary Deep Ray Seed", "shc_primary_deep_seed", state.shc_primary_deep_seed)
		begin_disabled(not deep_seed)
		slider_int("SHC Deep Seed Stride", "shc_primary_deep_stride", state.shc_primary_deep_stride, 1, 8)
		end_disabled(not deep_seed)
	end

	imgui.tree_pop()
end

local function draw_hybrid_extras(state)
	if state.rendering_mode ~= 0 then
		return
	end

	local async_available = state.async_shadow_ao_available == true
	begin_disabled(not async_available)
	local async_enabled = checkbox("Async RT Shadow/RTAO with GI##editor_config_luau", "enable_async_shadow_ao_overlap", state.enable_async_shadow_ao_overlap)
	end_disabled(not async_available)
	if not async_available then
		imgui.text_disabled("D3D12 async RT queue experiment.")
	end

	local async_controls = async_available and async_enabled
	begin_disabled(not async_controls)
	checkbox("Async RTAO with GI##editor_config_luau", "async_shadow_ao_overlap_rtao", state.async_shadow_ao_overlap_rtao)
	checkbox("Async RT Shadow with GI##editor_config_luau", "async_shadow_ao_overlap_shadow", state.async_shadow_ao_overlap_shadow)
	end_disabled(not async_controls)

	local rtao_enabled = checkbox("Enable RTAO##editor_config_luau", "enable_rtao", state.enable_rtao)
	if rtao_enabled and imgui.tree_node("RTAO Details", true) then
		slider_int("RTAO Samples", "rtao_samples", state.rtao_samples, 1, 16)
		slider_float("RTAO Radius", "rtao_radius", state.rtao_radius, 2.0, 256.0, "%.1f")
		slider_float("RTAO Power", "rtao_power", state.rtao_power, 0.25, 4.0, "%.2f")
		slider_float("RTAO Normal Bias", "rtao_normal_bias", state.rtao_normal_bias, 0.01, 2.0, "%.2f")
		slider_float("RTAO Contact Strength", "rtao_direct_contact", state.rtao_direct_contact, 0.0, 1.0, "%.2f")
		slider_float("RTAO AO Strength", "rtao_indirect_strength", state.rtao_indirect_strength, 0.0, 1.0, "%.2f")
		slider_float("RTAO AO Floor", "rtao_indirect_floor", state.rtao_indirect_floor, 0.0, 1.0, "%.2f")
		imgui.tree_pop()
	end
end

local function draw_environment(state)
	if not imgui.tree_node("Sky / Environment") then
		return
	end

	color_edit3("Sky Color Top", "sky_color_top", state.sky_color_top)
	color_edit3("Sky Color Bottom", "sky_color_bottom", state.sky_color_bottom)
	slider_float("Sky Intensity", "sky_intensity", state.sky_intensity, 0.0, 10.0, "%.2f")
	checkbox("Prefiltered Env Specular", "enable_prefiltered_env_specular", state.enable_prefiltered_env_specular)
	slider_float("Prefiltered Env Roughness Threshold", "prefiltered_env_roughness_threshold", state.prefiltered_env_roughness_threshold, 0.02, 1.0, "%.2f")
	slider_float("Prefiltered Env Roughness Fade", "prefiltered_env_roughness_fade", state.prefiltered_env_roughness_fade, 0.0, 0.5, "%.2f")

	imgui.tree_pop()
end

local function draw_lighting_gi(state)
	if not imgui.collapsing_header("Top / Lighting & GI", true) then
		return
	end

	if state.rendering_mode ~= 0 and state.rendering_mode ~= 1 then
		imgui.text_disabled("Lighting / GI controls are available in Hybrid and Path Tracing modes.")
		return
	end

	draw_direct_lighting(state)
	draw_specular_gi(state)
	draw_sky_lighting(state)
	draw_diffuse_gi(state)
	draw_hybrid_extras(state)
	draw_environment(state)
end

local function editor_config()
	local state = corona.get_ui_state("editor_config", false)
	if state.backend_name and state.backend_name ~= "" then
		imgui.text("Backend: " .. state.backend_name)
	end

	draw_top(state)
	draw_render_aa(state)
	draw_lighting_gi(state)
end

return {
	editor_config = editor_config,
}
