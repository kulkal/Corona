local script = {}

local function clamp(value, minValue, maxValue)
    if value < minValue then
        return minValue
    end
    if value > maxValue then
        return maxValue
    end
    return value
end

local function count_alive_enemies(enemies)
    local count = 0
    for _, enemy in ipairs(enemies or {}) do
        if enemy.alive and (enemy.health or 0.0) > 0.0 then
            count = count + 1
        end
    end
    return count
end

function script.update(entity, dt)
    local runtime = corona_dungeon_runtime
    if runtime == nil then
        return
    end
    runtime.playerHurtTimer = math.max(0.0, (runtime.playerHurtTimer or 0.0) - dt)
end

function script.ui(entity)
    local runtime = corona_dungeon_runtime
    if runtime == nil or runtime.player == nil then
        return
    end

    local player = runtime.player
    local maxHealth = math.max(player.maxHealth or 100.0, 1.0)
    local health = clamp(player.health or maxHealth, 0.0, maxHealth)
    local fraction = health / maxHealth
    local prefix = (runtime.playerHurtTimer or 0.0) > 0.0 and "PLAYER HP !" or "PLAYER HP"
    local fillColor = fraction > 0.55 and { 0.24, 0.86, 0.36, 0.95 }
        or fraction > 0.25 and { 0.96, 0.66, 0.18, 0.95 }
        or { 0.92, 0.18, 0.18, 0.95 }

    ui.overlay_rect_filled(22.0, 32.0, 306.0, 76.0, { 0.015, 0.018, 0.022, 0.72 }, 6.0)
    ui.overlay_rect(22.0, 32.0, 306.0, 76.0, { 1.0, 1.0, 1.0, 0.16 }, 1.0, 6.0)
    ui.overlay_text(prefix, 34.0, 42.0)
    ui.progress_bar(
        "dungeon.player.health",
        34.0,
        66.0,
        260.0,
        16.0,
        fraction,
        string.format("%d / %d", math.ceil(health), math.ceil(maxHealth)),
        fillColor,
        { 0.08, 0.07, 0.07, 0.78 },
        { 1.0, 1.0, 1.0, 0.62 })
    ui.overlay_text(string.format("ENEMIES  %d", count_alive_enemies(runtime.enemies)), 34.0, 88.0)
end

return script
