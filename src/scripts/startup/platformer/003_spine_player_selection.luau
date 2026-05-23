local script = {}

corona_script_controls = corona_script_controls or {}
corona_script_controls.platformer = corona_script_controls.platformer or {}

corona_platformer_player_candidate_count = math.max(1, math.min(5, math.floor(corona_script_controls.platformer.playerSelectionCandidateCount or 5)))
corona_platformer_player_candidates = {}
corona_platformer_selected_player_candidate_index = 1
corona_platformer_selected_player_candidate = nil
corona_platformer_selected_player_name = nil
corona_platformer_selected_player_path = nil
corona_platformer_selected_player_candidate_signature = nil
corona_platformer_player_candidate_signature = "empty"
corona_platformer_menu_nav_held = false
corona_platformer_player_card_colors = {
    { 0.16, 0.41, 0.58 },
    { 0.45, 0.28, 0.66 },
    { 0.62, 0.36, 0.18 },
    { 0.22, 0.50, 0.30 },
    { 0.60, 0.24, 0.34 },
}

function corona_platformer_format_character_name(name)
    local text = tostring(name or "hero")
    text = string.gsub(text, ".*/", "")
    text = string.gsub(text, "_", " ")
    text = string.gsub(text, "(%a)([%w']*)", function(first, rest)
        return string.upper(first) .. string.lower(rest)
    end)
    return text
end

function corona_platformer_is_player_candidate(asset)
    if asset == nil or asset.name == nil or asset.path == nil then
        return false
    end
    local name = string.lower(tostring(asset.name))
    if string.find(name, "/") ~= nil then
        return false
    end
    if string.find(name, "_big") ~= nil or string.find(name, "_spear") ~= nil or string.find(name, "_levelup") ~= nil then
        return false
    end
    if corona_platformer_spine_asset_is_playable ~= nil then
        return corona_platformer_spine_asset_is_playable(asset)
    end

    if string.find(name, "tower") ~= nil or string.find(name, "castle") ~= nil or string.find(name, "statue") ~= nil then
        return false
    end
    if string.find(name, "cloud") ~= nil or string.find(name, "leaf") ~= nil or string.find(name, "box") ~= nil then
        return false
    end
    if string.find(name, "egg") ~= nil or string.find(name, "butterfly") ~= nil or string.find(name, "pipe") ~= nil then
        return false
    end
    if string.find(name, "title") ~= nil or string.find(name, "slime") ~= nil or string.find(name, "mushroom") ~= nil then
        return false
    end
    return true
end

function corona_platformer_make_player_candidate_signature()
    local parts = {}
    for index, candidate in ipairs(corona_platformer_player_candidates or {}) do
        local text = tostring(index) .. "_" .. tostring(candidate.name or "") .. "_" .. tostring(candidate.path or "")
        text = string.lower(text)
        text = string.gsub(text, "[^%w_]+", "_")
        if string.len(text) > 48 then
            text = string.sub(text, 1, 48)
        end
        table.insert(parts, text)
    end
    if #parts <= 0 then
        return "empty"
    end
    return table.concat(parts, "_")
end

function corona_platformer_player_candidate_ui_id(kind, index)
    return "platformer.menu." ..
        tostring(kind or "hero") ..
        "." .. tostring(index or 0) ..
        "." .. tostring(corona_platformer_player_candidate_signature or "empty")
end

function corona_platformer_rebuild_player_candidates()
    corona_platformer_player_candidates = {}
    corona_platformer_selected_player_candidate_index = 1
    corona_platformer_selected_player_candidate = nil
    corona_platformer_selected_player_name = nil
    corona_platformer_selected_player_path = nil
    corona_platformer_selected_player_candidate_signature = nil

    local pool = {}
    for _, asset in ipairs(corona_platformer_enemy_assets or {}) do
        if corona_platformer_is_player_candidate(asset) then
            table.insert(pool, asset)
        end
    end

    if #pool <= 0 then
        table.insert(corona_platformer_player_candidates, {
            name = "ares",
            path = "assets/spine/chars/ares/ares.json",
        })
    else
        local desiredCount = math.min(corona_platformer_player_candidate_count, #pool)
        while #corona_platformer_player_candidates < desiredCount and #pool > 0 do
            local pickIndex = math.random(1, #pool)
            table.insert(corona_platformer_player_candidates, pool[pickIndex])
            table.remove(pool, pickIndex)
        end
    end

    corona_platformer_player_candidate_signature = corona_platformer_make_player_candidate_signature()
    if corona ~= nil and corona.log ~= nil then
        corona.log("platformer player candidates rebuilt signature=" .. tostring(corona_platformer_player_candidate_signature))
    end
    corona_platformer_select_player_candidate(1)
end

function corona_platformer_select_player_candidate(index)
    local candidates = corona_platformer_player_candidates or {}
    local candidate = candidates[index]
    if candidate == nil then
        return false
    end
    corona_platformer_selected_player_candidate_index = index
    corona_platformer_selected_player_candidate = candidate
    corona_platformer_selected_player_name = candidate.name
    corona_platformer_selected_player_path = candidate.path
    corona_platformer_selected_player_candidate_signature = corona_platformer_player_candidate_signature
    if corona ~= nil and corona.log ~= nil then
        corona.log("platformer player candidate selected index=" .. tostring(index) .. " name=" .. tostring(candidate.name) .. " path=" .. tostring(candidate.path))
    end
    return true
end

function corona_platformer_commit_selected_player_candidate()
    corona_platformer_player_selection_ensure()

    local candidates = corona_platformer_player_candidates or {}
    local index = corona_platformer_selected_player_candidate_index or 1
    if corona_platformer_selected_player_candidate_signature ~= nil and
        corona_platformer_selected_player_candidate_signature ~= corona_platformer_player_candidate_signature then
        index = 1
        corona_platformer_selected_player_candidate = nil
        corona_platformer_selected_player_name = nil
        corona_platformer_selected_player_path = nil
        corona_platformer_selected_player_candidate_signature = nil
    end
    local candidate = candidates[index]
    if candidate == nil then
        candidate = corona_platformer_selected_player_candidate
    end
    if candidate == nil and #candidates > 0 then
        index = 1
        candidate = candidates[1]
    end
    if candidate == nil then
        return nil, index
    end

    corona_platformer_selected_player_candidate_index = index
    corona_platformer_selected_player_candidate = candidate
    corona_platformer_selected_player_name = candidate.name
    corona_platformer_selected_player_path = candidate.path
    corona_platformer_selected_player_candidate_signature = corona_platformer_player_candidate_signature
    if corona ~= nil and corona.log ~= nil then
        corona.log("platformer player selection committed index=" .. tostring(index) .. " name=" .. tostring(candidate.name) .. " path=" .. tostring(candidate.path))
    end
    return candidate, index
end

function corona_platformer_select_player_candidate_delta(delta)
    local count = #(corona_platformer_player_candidates or {})
    if count <= 0 then
        return false
    end
    local nextIndex = corona_platformer_selected_player_candidate_index + delta
    if nextIndex < 1 then
        nextIndex = count
    elseif nextIndex > count then
        nextIndex = 1
    end
    return corona_platformer_select_player_candidate(nextIndex)
end

function corona_platformer_player_selection_ensure()
    if #(corona_platformer_player_candidates or {}) <= 0 then
        corona_platformer_rebuild_player_candidates()
    elseif corona_platformer_selected_player_candidate == nil then
        corona_platformer_select_player_candidate(corona_platformer_selected_player_candidate_index or 1)
    end
end

function corona_platformer_draw_player_selection_cards(originX, originY, panelWidth)
    if ui.overlay_button == nil or ui.overlay_rect_filled == nil or ui.overlay_text == nil then
        return originY
    end

    corona_platformer_player_selection_ensure()
    local candidates = corona_platformer_player_candidates or {}
    local count = #candidates
    if count <= 0 then
        return originY
    end

    local columns = count
    if panelWidth < 520.0 then
        columns = 2
    elseif panelWidth < 760.0 then
        columns = 3
    end

    local gap = 10.0
    local cardHeight = 176.0
    local cardWidth = math.max(118.0, (panelWidth - 48.0 - gap * (columns - 1)) / columns)
    local bottom = originY

    for index, candidate in ipairs(candidates) do
        local row = math.floor((index - 1) / columns)
        local column = (index - 1) % columns
        local rowCount = math.min(columns, count - row * columns)
        local rowWidth = rowCount * cardWidth + (rowCount - 1) * gap
        local x = originX + (panelWidth - rowWidth) * 0.5 + column * (cardWidth + gap)
        local y = originY + row * (cardHeight + gap)
        local selected = index == corona_platformer_selected_player_candidate_index
        local baseColor = selected and { 0.020, 0.070, 0.060, 0.07 } or { 0.006, 0.010, 0.014, 0.035 }
        local hoverColor = selected and { 0.055, 0.145, 0.125, 0.17 } or { 0.028, 0.040, 0.050, 0.11 }
        local frameColor = selected and { 0.95, 0.78, 0.32, 0.96 } or { 0.34, 0.40, 0.42, 0.70 }
        local color = corona_platformer_player_card_colors[((index - 1) % #corona_platformer_player_card_colors) + 1]
        local displayName = corona_platformer_format_character_name(candidate.name)
        if string.len(displayName) > 13 then
            displayName = string.sub(displayName, 1, 12) .. "."
        end

        if ui.overlay_button(
            corona_platformer_player_candidate_ui_id("hero", index),
            " ",
            x,
            y,
            cardWidth,
            cardHeight,
            baseColor,
            hoverColor,
            frameColor,
            8.0) then
            corona_platformer_select_player_candidate(index)
        end
        ui.overlay_rect_filled(x + 12.0, y + 12.0, cardWidth - 24.0, cardHeight - 70.0, { 0.0, 0.0, 0.0, 0.025 }, 6.0)
        ui.overlay_rect_filled(x + 12.0, y + cardHeight - 48.0, cardWidth - 24.0, 36.0, { 0.010, 0.014, 0.018, 0.88 }, 6.0)
        ui.overlay_rect_filled(x + 12.0, y + cardHeight - 48.0, 28.0, 36.0, { color[1], color[2], color[3], 0.84 }, 6.0)
        ui.overlay_text(tostring(index), x + 22.0, y + cardHeight - 38.0)
        ui.overlay_text(displayName, x + 48.0, y + cardHeight - 42.0)
        ui.overlay_text(selected and "SELECTED" or "CHOOSE", x + 48.0, y + cardHeight - 24.0)
        bottom = math.max(bottom, y + cardHeight)
    end

    return bottom
end

corona_platformer_rebuild_player_candidates()

return script
