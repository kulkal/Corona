-- Phase 5: Menu / HUD drawing helpers, extracted from
-- 004_spine_platformer.luau to reduce that file's top-level local count
-- and continue the per-feature script split described in
-- docs/spine_cpu_sprite_batching_optimization_plan.md.
--
-- This chunk loads BEFORE 004 (the `003y_` prefix puts it ahead of
-- `004_`). The functions defined here are platformer-namespaced globals
-- (`corona_platformer_draw_menu`, etc.) whose bodies read shared state
-- via `corona_platformer_locals.<name>` at call time. 004's main body
-- populates `corona_platformer_locals` with everything we need before
-- it invokes any of these functions (script.ui / load_level path).
--
-- `controls` is read directly from `corona_script_controls.platformer`
-- because 004's `local controls = ...` is not visible across chunks.

local function loc()
    return corona_platformer_locals or {}
end

local function platformer_controls()
    if corona_script_controls == nil then
        return {}
    end
    return corona_script_controls.platformer or {}
end

function corona_platformer_menu_preview_screen_to_world(screenX, screenY, screenWidth, screenHeight)
    local controls = platformer_controls()
    local cameraDistance = controls.cameraDistance or 2200.0
    local cameraFov = controls.cameraFov or 0.24
    local viewHeight = 2.0 * cameraDistance * math.tan(cameraFov * 0.5)
    local viewWidth = viewHeight * (screenWidth / math.max(1.0, screenHeight))
    -- The render camera basis maps positive world X to screen-left in this
    -- preview setup, so invert X to keep card labels and preview sprites in
    -- the same left-to-right order.
    return (0.5 - screenX / math.max(1.0, screenWidth)) * viewWidth,
        (0.5 - screenY / math.max(1.0, screenHeight)) * viewHeight
end

function corona_platformer_ensure_menu_preview_scene()
    local L = loc()
    local controls = platformer_controls()
    local add_spawned = L.add_spawned
    local characterZ = L.characterZ or 0.0
    if add_spawned == nil then
        return
    end

    if corona_platformer_menu_preview_camera == nil then
        corona_platformer_menu_preview_camera = add_spawned(corona.Entity.create("PlatformerMenuPreviewCamera"))
        corona.CameraComponent.add(corona_platformer_menu_preview_camera, {
            position = { 0.0, 0.0, -(controls.cameraDistance or 2200.0) },
            look_at = { 0.0, 0.0, characterZ },
            up = { 0.0, 1.0, 0.0 },
            fov = controls.cameraFov or 0.24,
            near_plane = 5.0,
            far_plane = 12000.0,
            active = true,
        })
    else
        corona.CameraComponent.set(corona_platformer_menu_preview_camera, {
            position = { 0.0, 0.0, -(controls.cameraDistance or 2200.0) },
            look_at = { 0.0, 0.0, characterZ },
            up = { 0.0, 1.0, 0.0 },
            active = true,
        })
    end

    if corona_platformer_menu_preview_light == nil then
        corona_platformer_menu_preview_light = add_spawned(corona.Entity.create("PlatformerMenuPreviewSun"))
        corona.LightComponent.add(corona_platformer_menu_preview_light, {
            type = "directional",
            enabled = true,
            direction = corona_platformer_sun_direction,
            color = { 1.0, 0.98, 0.92 },
            intensity = controls.menuPreviewSunIntensity or 1.05,
            persistent = false,
        })
    end
end

function corona_platformer_destroy_menu_preview_entities()
    for index, preview in ipairs(corona_platformer_menu_preview_entities or {}) do
        if preview ~= nil and preview.entity ~= nil then
            corona.Entity.destroy(preview.entity)
        end
        corona_platformer_menu_preview_entities[index] = nil
    end
end

function corona_platformer_update_menu_previews(dt)
    local L = loc()
    if L.get_game_state == nil or L.get_game_state() ~= "menu" then
        return
    end
    if corona_platformer_player_selection_ensure ~= nil then
        corona_platformer_player_selection_ensure()
    end

    local candidates = corona_platformer_player_candidates or {}
    local count = #candidates
    if count <= 0 then
        return
    end

    local signature = tostring(corona_platformer_player_candidate_signature or "empty")
    if signature ~= corona_platformer_menu_preview_signature then
        corona_platformer_destroy_menu_preview_entities()
        corona_platformer_menu_preview_signature = signature
    end

    corona_platformer_ensure_menu_preview_scene()
    local controls = platformer_controls()
    if corona.set_ui_value ~= nil then
        corona.set_ui_value("sky_color_top", controls.menuSkyColorTop or { 0.42, 0.58, 0.78 })
        corona.set_ui_value("sky_color_bottom", controls.menuSkyColorBottom or { 0.62, 0.74, 0.88 })
        corona.set_ui_value("sky_intensity", controls.menuSkyIntensity or 0.54)
        corona.set_ui_value("light_intensity", controls.menuPreviewSunIntensity or 1.05)
    end

    local add_spawned = L.add_spawned
    local get_spine_scene = L.get_spine_scene
    local set_character_spine_pose = L.set_character_spine_pose
    local set_effect_entity_visible = L.set_effect_entity_visible
    local characterZ = L.characterZ or 0.0
    if add_spawned == nil or get_spine_scene == nil or set_character_spine_pose == nil or set_effect_entity_visible == nil then
        return
    end
    local time = L.get_time ~= nil and L.get_time() or 0.0

    local screenWidth, screenHeight = corona_platformer_get_ui_screen_size()
    local panelWidth = math.max(360.0, math.min(920.0, screenWidth - 48.0))
    local panelX = math.floor((screenWidth - panelWidth) * 0.5 + 0.5)
    local panelY = math.floor((screenHeight - math.max(520.0, math.min(620.0, screenHeight - 48.0))) * 0.5 + 0.5)
    local columns = count
    if panelWidth < 520.0 then
        columns = 2
    elseif panelWidth < 760.0 then
        columns = 3
    end

    local gap = 10.0
    local cardHeight = 176.0
    local cardWidth = math.max(118.0, (panelWidth - 48.0 - gap * (columns - 1)) / columns)
    local originY = panelY + 158.0
    local sampleTime = time % 2.0
    local cameraDistance = controls.cameraDistance or 2200.0
    local cameraFov = controls.cameraFov or 0.24
    local viewHeight = 2.0 * cameraDistance * math.tan(cameraFov * 0.5)

    for index, candidate in ipairs(candidates) do
        local row = math.floor((index - 1) / columns)
        local column = (index - 1) % columns
        local rowCount = math.min(columns, count - row * columns)
        local rowWidth = rowCount * cardWidth + (rowCount - 1) * gap
        local cardX = panelX + (panelWidth - rowWidth) * 0.5 + column * (cardWidth + gap)
        local cardY = originY + row * (cardHeight + gap)
        local selected = index == corona_platformer_selected_player_candidate_index
        local targetExtent = selected and (controls.menuPreviewSelectedExtent or 58.0) or (controls.menuPreviewTargetExtent or 52.0)
        local previewPixelHeight = targetExtent * screenHeight / math.max(1.0, viewHeight)
        local previewCenterY = (cardY + 12.0 + cardY + cardHeight - 58.0) * 0.5
        local footScreenX = cardX + cardWidth * 0.5
        local footScreenY = previewCenterY + previewPixelHeight * 0.5
        local worldX, worldY = corona_platformer_menu_preview_screen_to_world(footScreenX, footScreenY, screenWidth, screenHeight)
        local preview = corona_platformer_menu_preview_entities[index]
        if preview == nil then
            preview = {
                entity = add_spawned(corona.Entity.create("PlatformerHeroPreview" .. tostring(index))),
                lastHandle = nil,
                lastFrame = -1,
                lastAnimation = "",
                assetPath = "",
            }
            corona_platformer_menu_preview_entities[index] = preview
        end
        local assetChanged = preview.assetPath ~= candidate.path
        if assetChanged then
            preview.lastHandle = nil
            preview.lastFrame = -1
            preview.lastAnimation = ""
            preview.assetPath = candidate.path
            if corona ~= nil and corona.log ~= nil then
                corona.log("platformer menu preview slot=" .. tostring(index) .. " asset=" .. tostring(candidate.path) .. " name=" .. tostring(candidate.name))
            end
        end
        local handle, frame = get_spine_scene(candidate.path, "idle", sampleTime, 6.0, assetChanged and nil or preview.lastHandle, true)
        if handle ~= nil and preview.entity ~= nil then
            set_effect_entity_visible(preview.entity, true)
            local previewZ = selected and (characterZ - 0.95) or (characterZ + 0.35 + index * 0.025)
            set_character_spine_pose(preview.entity, handle, worldX, worldY, previewZ, 0.0, targetExtent, 0.72, 0.0, false, false)
            preview.lastHandle = handle
            preview.lastFrame = frame or preview.lastFrame
            preview.lastAnimation = "idle"
            preview.assetPath = candidate.path
        elseif preview.entity ~= nil then
            set_effect_entity_visible(preview.entity, false)
        end
    end

    for index = count + 1, #(corona_platformer_menu_preview_entities or {}) do
        local preview = corona_platformer_menu_preview_entities[index]
        if preview ~= nil and preview.entity ~= nil then
            corona.Entity.destroy(preview.entity)
        end
        corona_platformer_menu_preview_entities[index] = nil
    end
end

function corona_platformer_draw_menu()
    local L = loc()
    local menuReason = L.get_menu_reason ~= nil and L.get_menu_reason() or ""
    local title = "PLATFORMER"
    if menuReason == "game_over" then
        title = "GAME OVER"
    elseif menuReason == "clear" then
        title = "ALL CLEAR"
    end

    local screenWidth, screenHeight = corona_platformer_get_ui_screen_size()
    local panelWidth = math.max(360.0, math.min(920.0, screenWidth - 48.0))
    local panelHeight = math.max(520.0, math.min(620.0, screenHeight - 48.0))
    local panelX = math.floor((screenWidth - panelWidth) * 0.5 + 0.5)
    local panelY = math.floor((screenHeight - panelHeight) * 0.5 + 0.5)
    local centerX = panelX + panelWidth * 0.5
    local selectedName = L.playerDisplayName or "Hero"
    if corona_platformer_selected_player_candidate ~= nil and corona_platformer_format_character_name ~= nil then
        selectedName = corona_platformer_format_character_name(corona_platformer_selected_player_candidate.name)
    end
    local selectedNameUpper = string.upper(selectedName)
    if string.len(selectedNameUpper) > 16 then
        selectedNameUpper = string.sub(selectedNameUpper, 1, 15) .. "."
    end

    local function draw_centered_text(text, y, glyphWidth)
        local approxWidth = string.len(text) * (glyphWidth or 7.0)
        ui.overlay_text(text, centerX - approxWidth * 0.5, y)
    end

    ui.overlay_rect_filled(0.0, 0.0, screenWidth, screenHeight, { 0.0, 0.0, 0.0, 0.14 }, 0.0)
    ui.overlay_rect_filled(panelX, panelY, panelWidth, panelHeight, { 0.014, 0.020, 0.026, 0.26 }, 8.0)
    ui.overlay_rect(panelX, panelY, panelWidth, panelHeight, { 0.86, 0.70, 0.34, 0.82 }, 1.5, 8.0)
    ui.overlay_rect_filled(panelX + 24.0, panelY + 80.0, panelWidth - 48.0, 1.0, { 0.86, 0.70, 0.34, 0.42 }, 0.0)

    draw_centered_text(title, panelY + 34.0, 8.2)
    draw_centered_text("CHOOSE YOUR HERO", panelY + 96.0, 7.0)
    draw_centered_text("Press 1-5 or use left/right, then start", panelY + 122.0, 6.0)

    local buttonWidth = math.min(panelWidth - 96.0, 330.0)
    local buttonHeight = 52.0
    local buttonX = centerX - buttonWidth * 0.5
    local cardsBottom = panelY + 158.0
    if corona_platformer_draw_player_selection_cards ~= nil then
        cardsBottom = corona_platformer_draw_player_selection_cards(panelX, panelY + 158.0, panelWidth)
    end
    local startButtonY = math.min(cardsBottom + 24.0, panelY + panelHeight - 122.0)
    if ui.overlay_button ~= nil then
        if ui.overlay_button(
            corona_platformer_player_candidate_ui_id ~= nil and corona_platformer_player_candidate_ui_id("start", 0) or "platformer.menu.start",
            ">  START AS " .. selectedNameUpper,
            buttonX,
            startButtonY,
            buttonWidth,
            buttonHeight,
            { 0.07, 0.27, 0.24, 0.96 },
            { 0.10, 0.41, 0.35, 0.98 },
            { 0.72, 0.96, 0.84, 0.90 },
            8.0) then
            if L.start_new_game ~= nil then L.start_new_game() end
            return
        end
        if ui.overlay_button(
            corona_platformer_player_candidate_ui_id ~= nil and corona_platformer_player_candidate_ui_id("exit", 0) or "platformer.menu.exit",
            "X  EXIT",
            buttonX,
            startButtonY + 64.0,
            buttonWidth,
            buttonHeight,
            { 0.28, 0.11, 0.11, 0.95 },
            { 0.44, 0.15, 0.13, 0.98 },
            { 0.96, 0.78, 0.56, 0.90 },
            8.0) then
            if L.request_exit ~= nil then L.request_exit() end
            return
        end
        return
    end

    ui.begin_window("Platformer Menu")
    ui.text(title)
    ui.text("Choose your hero")
    for index, candidate in ipairs(corona_platformer_player_candidates or {}) do
        local label = tostring(index) .. "  " .. (corona_platformer_format_character_name ~= nil and corona_platformer_format_character_name(candidate.name) or tostring(candidate.name or "Hero"))
        if index == corona_platformer_selected_player_candidate_index then
            label = "[selected] " .. label
        end
        local buttonId = corona_platformer_player_candidate_ui_id ~= nil and corona_platformer_player_candidate_ui_id("hero", index) or ("platformer.menu.hero." .. tostring(index))
        if ui.button(label, buttonId) and corona_platformer_select_player_candidate ~= nil then
            corona_platformer_select_player_candidate(index)
        end
    end
    local startButtonId = corona_platformer_player_candidate_ui_id ~= nil and corona_platformer_player_candidate_ui_id("start", 0) or "platformer.menu.start"
    if ui.button("START AS " .. selectedNameUpper, startButtonId) then
        ui.end_window()
        if L.start_new_game ~= nil then L.start_new_game() end
        return
    end
    local exitButtonId = corona_platformer_player_candidate_ui_id ~= nil and corona_platformer_player_candidate_ui_id("exit", 0) or "platformer.menu.exit"
    if ui.button("EXIT", exitButtonId) then
        ui.end_window()
        if L.request_exit ~= nil then L.request_exit() end
        return
    end
    ui.end_window()
end

function corona_platformer_draw_top_hud()
    if ui.overlay_rect_filled == nil or ui.overlay_text == nil then
        return
    end
    local L = loc()
    local combat = L.combat or {}
    local player = L.player
    local clamp = L.clamp
    if player == nil or clamp == nil then
        return
    end

    local screenWidth = select(1, corona_platformer_get_ui_screen_size())
    local panelWidth = math.max(560.0, math.min(screenWidth - 32.0, 1040.0))
    local panelX = (screenWidth - panelWidth) * 0.5
    local panelY = 14.0
    local panelHeight = screenWidth < 860.0 and 128.0 or 86.0
    local progressDirection = corona_platformer_progress_world_direction()
    local goalX = L.goalX or 0.0
    local startX = L.startX or 0.0
    local travel = (goalX - startX) * progressDirection
    local progress = 0.0
    if math.abs(travel) > 0.001 then
        progress = clamp(((player.x - startX) * progressDirection) / travel, 0.0, 1.0)
    end

    ui.overlay_rect_filled(panelX + 3.0, panelY + 4.0, panelWidth, panelHeight, { 0.0, 0.0, 0.0, 0.30 }, 8.0)
    ui.overlay_rect_filled(panelX, panelY, panelWidth, panelHeight, { 0.030, 0.042, 0.050, 0.82 }, 8.0)
    ui.overlay_rect(panelX, panelY, panelWidth, panelHeight, { 0.88, 0.72, 0.36, 0.78 }, 1.4, 8.0)
    ui.overlay_rect_filled(panelX + 12.0, panelY + 9.0, panelWidth - 24.0, 3.0, { 1.0, 0.84, 0.38, 0.50 }, 2.0)

    local leftX = panelX + 22.0
    local topY = panelY + 21.0
    local playerDisplayName = L.playerDisplayName or "Hero"
    local hudName = string.upper(playerDisplayName)
    if string.len(hudName) > 10 then
        hudName = string.sub(hudName, 1, 9) .. "."
    end
    ui.overlay_text(hudName, leftX, topY - 5.0)
    local health = L.get_health ~= nil and L.get_health() or 0
    local maxHealth = L.maxHealth or 1
    ui.overlay_progress_bar(
        "platformer.hud.hp",
        leftX + 62.0,
        topY - 2.0,
        208.0,
        18.0,
        health / maxHealth,
        "HP " .. tostring(health) .. "/" .. tostring(maxHealth),
        { 0.15, 0.88, 0.28, 0.96 },
        { 0.12, 0.16, 0.15, 0.78 },
        { 0.72, 0.96, 0.74, 0.86 })

    ui.overlay_text("LIVES", leftX, topY + 29.0)
    local lives = L.get_lives ~= nil and L.get_lives() or 0
    local maxLives = L.maxLives or 1
    for index = 1, maxLives do
        local slotX = leftX + 66.0 + (index - 1) * 25.0
        local alive = index <= lives
        ui.overlay_rect_filled(slotX, topY + 30.0, 18.0, 14.0, alive and { 0.90, 0.18, 0.18, 0.94 } or { 0.20, 0.20, 0.22, 0.80 }, 3.0)
        ui.overlay_rect(slotX, topY + 30.0, 18.0, 14.0, alive and { 1.0, 0.70, 0.42, 0.86 } or { 0.45, 0.45, 0.48, 0.62 }, 1.0, 3.0)
    end
    ui.overlay_text(tostring(lives) .. "/" .. tostring(maxLives), leftX + 202.0, topY + 27.0)

    local centerX = panelX + panelWidth * 0.5 - 165.0
    local centerY = topY - 3.0
    if screenWidth < 860.0 then
        centerX = panelX + 22.0
        centerY = panelY + 73.0
    end
    local currentLevel = L.get_current_level ~= nil and L.get_current_level() or 1
    local levelCount = L.levelCount or 1
    ui.overlay_text("LEVEL " .. tostring(currentLevel) .. "/" .. tostring(levelCount), centerX, centerY - 2.0)
    ui.overlay_progress_bar(
        "platformer.hud.progress",
        centerX,
        centerY + 23.0,
        330.0,
        16.0,
        progress,
        "GOAL " .. tostring(math.floor(progress * 100.0 + 0.5)) .. "%",
        { 0.94, 0.68, 0.22, 0.95 },
        { 0.12, 0.13, 0.16, 0.78 },
        { 0.94, 0.88, 0.62, 0.84 })
    ui.overlay_rect_filled(centerX + 330.0 * progress - 3.0, centerY + 19.0, 6.0, 24.0, { 1.0, 0.96, 0.50, 0.94 }, 2.0)
    local defeatedEnemyCount = L.get_defeated_enemy_count ~= nil and L.get_defeated_enemy_count() or 0
    local currentLevelEnemyTotal = L.get_current_level_enemy_total ~= nil and L.get_current_level_enemy_total() or 0
    ui.overlay_text("ENEMIES " .. tostring(defeatedEnemyCount) .. "/" .. tostring(currentLevelEnemyTotal), centerX + 118.0, centerY + 44.0)

    local ammoCardWidth = 158.0
    local ammoX = panelX + panelWidth - ammoCardWidth - 22.0
    local ammoY = topY - 5.0
    local ammo = combat.rangedAmmo or 0
    local ready = ammo > 0
    local frameColor = ready and { 0.95, 0.56, 0.18, 0.88 } or { 0.46, 0.46, 0.48, 0.72 }
    local coreColor = ready and { 1.0, 0.34, 0.08, 0.92 } or { 0.28, 0.28, 0.30, 0.82 }
    local flameColor = ready and { 1.0, 0.78, 0.20, 0.94 } or { 0.40, 0.40, 0.42, 0.82 }

    ui.overlay_rect_filled(ammoX, ammoY, ammoCardWidth, 52.0, { 0.025, 0.027, 0.030, 0.70 }, 8.0)
    ui.overlay_rect(ammoX, ammoY, ammoCardWidth, 52.0, frameColor, 1.4, 8.0)
    ui.overlay_rect_filled(ammoX + 16.0, ammoY + 21.0, 22.0, 18.0, coreColor, 3.0)
    ui.overlay_rect_filled(ammoX + 23.0, ammoY + 10.0, 10.0, 22.0, flameColor, 3.0)
    ui.overlay_rect_filled(ammoX + 27.0, ammoY + 28.0, 8.0, 8.0, { 1.0, 0.94, 0.50, ready and 0.92 or 0.32 }, 2.0)
    ui.overlay_text("FIRE", ammoX + 54.0, ammoY + 7.0)
    ui.overlay_text("x " .. tostring(ammo), ammoX + 54.0, ammoY + 27.0)

    if corona_platformer_collision_debug_enabled then
        ui.overlay_text("COLLISION DEBUG", panelX + panelWidth - 170.0, panelY + panelHeight + 8.0)
    end
end

-- Return an empty table so the script loader's `lua_istable(L, -1)` check
-- passes with no `update`/`shutdown`/`imgui`/`ui` fields attached. The
-- main UI dispatch still lives in 004's `script.ui`, which calls these
-- globals as needed.
return {}
