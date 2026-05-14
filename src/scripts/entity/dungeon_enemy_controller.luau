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

local function world_to_cell(runtime, x, z)
    return {
        x = math.floor(x / runtime.tile + runtime.originX + 0.5),
        y = math.floor(z / runtime.tile + runtime.originY + 0.5),
    }
end

local function floor_at(runtime, x, y)
    local floor = runtime.floor
    return floor[y] ~= nil and floor[y][x] == true
end

local function is_door_blocking_cell(runtime, cell)
    local door = runtime.door
    return runtime.doorOpen ~= true and cell.x == door.x and cell.y == door.y
end

local function is_walkable_position(runtime, x, z, radius)
    local samples = {
        { 0.0, 0.0 },
        { radius, 0.0 },
        { -radius, 0.0 },
        { 0.0, radius },
        { 0.0, -radius },
    }

    for _, sample in ipairs(samples) do
        local cell = world_to_cell(runtime, x + sample[1], z + sample[2])
        if not floor_at(runtime, cell.x, cell.y) or is_door_blocking_cell(runtime, cell) then
            return false
        end
    end
    return true
end

local function try_move(runtime, enemy, dx, dz)
    local radius = math.min(enemy.radius or 34.0, (runtime.tile or 120.0) * 0.32)
    local nextX = enemy.x + dx
    local nextZ = enemy.z + dz
    if is_walkable_position(runtime, nextX, enemy.z, radius) then
        enemy.x = nextX
    end
    if is_walkable_position(runtime, enemy.x, nextZ, radius) then
        enemy.z = nextZ
    end
end

local function turn_toward(enemy, dx, dz)
    if math.abs(dx) + math.abs(dz) > 0.001 then
        enemy.yaw = math.deg(math.atan2(dx, dz))
    end
end

local function update_ai(runtime, enemy, dt)
    local player = runtime.player
    if player == nil then
        return false
    end

    enemy.stunTimer = math.max(0.0, (enemy.stunTimer or 0.0) - dt)
    enemy.attackTimer = math.max(0.0, (enemy.attackTimer or 0.0) - dt)
    if enemy.stunTimer > 0.0 then
        return false
    end

    local dx = player.x - enemy.x
    local dz = player.z - enemy.z
    local distance = math.sqrt(dx * dx + dz * dz)
    local moving = false
    if distance < (enemy.aggroRange or 520.0) and distance > (enemy.stopDistance or 84.0) then
        local speed = enemy.speed or 70.0
        local invDistance = 1.0 / math.max(distance, 0.001)
        local moveX = dx * invDistance * speed * dt
        local moveZ = dz * invDistance * speed * dt
        try_move(runtime, enemy, moveX, moveZ)
        turn_toward(enemy, dx, dz)
        moving = true
    elseif distance <= (enemy.stopDistance or 84.0) + 18.0 then
        turn_toward(enemy, dx, dz)
        if enemy.attackTimer <= 0.0 and (player.health or 0.0) > 0.0 then
            player.health = math.max(0.0, (player.health or 0.0) - (enemy.attackDamage or 6.0))
            runtime.playerHurtTimer = 0.36
            enemy.attackTimer = enemy.attackCooldown or 1.1
            enemy.hitFlash = math.max(enemy.hitFlash or 0.0, 0.10)
        end
    elseif enemy.kind ~= "boss" then
        enemy.wanderTimer = (enemy.wanderTimer or 0.0) - dt
        if enemy.wanderTimer <= 0.0 then
            enemy.wanderTimer = 0.9 + ((enemy.entity or 1) % 7) * 0.18
            enemy.wanderAngle = ((enemy.wanderAngle or enemy.yaw or 0.0) + 67.0 + ((enemy.entity or 1) % 5) * 23.0) % 360.0
        end
        local radians = math.rad(enemy.wanderAngle or 0.0)
        local moveX = math.sin(radians) * (enemy.speed or 70.0) * 0.28 * dt
        local moveZ = math.cos(radians) * (enemy.speed or 70.0) * 0.28 * dt
        try_move(runtime, enemy, moveX, moveZ)
        turn_toward(enemy, moveX, moveZ)
        moving = true
    else
        turn_toward(enemy, dx, dz)
    end
    return moving
end

local function update_mesh(enemy, entity, dt, moving)
    enemy.walkBob = (enemy.walkBob or 0.0) + dt * (moving and 6.5 or 2.0)
    enemy.hitFlash = math.max(0.0, (enemy.hitFlash or 0.0) - dt)

    local bobScale = enemy.kind == "boss" and 2.2 or 3.6
    local hitLift = enemy.hitFlash > 0.0 and (enemy.kind == "boss" and 8.0 or 5.0) or 0.0
    local bob = math.sin(enemy.walkBob) * bobScale + hitLift
    local yaw = enemy.yaw or 0.0
    if enemy.hitFlash > 0.0 then
        yaw = yaw + math.sin(enemy.hitFlash * 54.0) * (enemy.kind == "boss" and 2.5 or 7.0)
    end

    corona.MeshComponent.set(entity, {
        position = { enemy.x, (enemy.meshY or 0.0) + bob, enemy.z },
        rotation = { 0.0, yaw, 0.0 },
        target_extent = enemy.targetExtent or 68.0,
    })
end

function script.update(entity, dt)
    local runtime = corona_dungeon_runtime
    if runtime == nil or runtime.enemyByEntity == nil then
        return
    end

    local enemy = runtime.enemyByEntity[entity]
    if enemy == nil then
        return
    end

    if enemy.health <= 0.0 then
        enemy.alive = false
    end
    if not enemy.alive then
        runtime.enemyByEntity[entity] = nil
        corona.Entity.destroy(entity)
        return
    end

    local moving = update_ai(runtime, enemy, dt)
    update_mesh(enemy, entity, dt, moving)
end

function script.ui(entity)
    local runtime = corona_dungeon_runtime
    if runtime == nil or runtime.enemyByEntity == nil then
        return
    end

    local enemy = runtime.enemyByEntity[entity]
    if enemy == nil or not enemy.alive or enemy.health == nil or enemy.maxHealth == nil or enemy.maxHealth <= 0.0 then
        return
    end

    local fraction = clamp(enemy.health / enemy.maxHealth, 0.0, 1.0)
    local headOffset = enemy.kind == "boss" and 220.0 or 96.0
    local width = enemy.kind == "boss" and 118.0 or 72.0
    local height = enemy.kind == "boss" and 10.0 or 7.0
    local label = string.format("%d / %d", math.ceil(enemy.health), math.ceil(enemy.maxHealth))
    if enemy.kind == "boss" then
        label = "Boss " .. label
    end

    local fillColor = fraction > 0.55 and { 0.25, 0.82, 0.32, 0.95 }
        or fraction > 0.25 and { 0.96, 0.62, 0.18, 0.95 }
        or { 0.90, 0.13, 0.16, 0.95 }

    ui.world_progress_bar(
        "dungeon.enemy.hp." .. tostring(entity),
        { enemy.x, (enemy.meshY or 0.0) + headOffset, enemy.z },
        fraction,
        label,
        width,
        height,
        fillColor,
        { 0.08, 0.07, 0.07, 0.80 },
        { 1.0, 1.0, 1.0, 0.70 })

    if enemy.kind == "boss" then
        ui.world_text(
            "dungeon.enemy.name." .. tostring(entity),
            { enemy.x, (enemy.meshY or 0.0) + headOffset - 22.0, enemy.z },
            "BUDDHA",
            0.0,
            0.0,
            { 1.0, 0.88, 0.34, 0.95 })
    end
end

return script
