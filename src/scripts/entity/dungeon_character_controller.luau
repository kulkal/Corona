local script = {}

local function forward_from_yaw(yaw)
    local radians = math.rad(yaw)
    return {
        math.sin(radians),
        0.0,
        math.cos(radians),
    }
end

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

local function is_walkable_position(runtime, x, z)
    local radius = runtime.tile * 0.28
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

local function try_move(runtime, dx, dz)
    local player = runtime.player
    local nextX = player.x + dx
    local nextZ = player.z + dz
    if is_walkable_position(runtime, nextX, player.z) then
        player.x = nextX
    end
    if is_walkable_position(runtime, player.x, nextZ) then
        player.z = nextZ
    end
end

local function update_attack(runtime, dt, gamepad)
    runtime.playerAttackCooldown = math.max(0.0, (runtime.playerAttackCooldown or 0.0) - dt)
    runtime.playerAttackTimer = math.max(0.0, (runtime.playerAttackTimer or 0.0) - dt)

    local attackPressed =
        corona.was_key_pressed("F") or
        corona.was_key_pressed("CTRL") or
        corona.was_key_pressed("ENTER") or
        (gamepad.connected and (gamepad.b_pressed or gamepad.y_pressed))
    if not attackPressed or runtime.playerAttackCooldown > 0.0 then
        return
    end

    runtime.playerAttackCooldown = 0.36
    runtime.playerAttackTimer = 0.20

    local player = runtime.player
    local forward = forward_from_yaw(runtime.cameraYaw)
    local attackRange = runtime.playerAttackRange or 150.0
    local bestEnemy = nil
    local bestDistance = attackRange + 1000.0

    for _, enemy in ipairs(runtime.enemies or {}) do
        if enemy.alive and enemy.health ~= nil and enemy.health > 0.0 then
            local dx = enemy.x - player.x
            local dz = enemy.z - player.z
            local distance = math.sqrt(dx * dx + dz * dz)
            local radius = enemy.radius or 35.0
            if distance <= attackRange + radius and distance > 0.001 then
                local facing = (dx * forward[1] + dz * forward[3]) / distance
                if facing >= (runtime.playerAttackHalfAngleCos or 0.42) and distance < bestDistance then
                    bestDistance = distance
                    bestEnemy = enemy
                end
            end
        end
    end

    if bestEnemy ~= nil then
        local damage = runtime.playerAttackDamage or 35.0
        if bestEnemy.kind == "boss" then
            damage = damage * 0.62
        end
        bestEnemy.health = math.max(0.0, bestEnemy.health - damage)
        bestEnemy.hitFlash = 0.22
        bestEnemy.stunTimer = math.max(bestEnemy.stunTimer or 0.0, 0.18)
        bestEnemy.lastHitYaw = runtime.cameraYaw
    end
end

local function update_character(runtime, entity, dt, moving)
    if entity == nil then
        return
    end

    if moving then
        runtime.walkBob = runtime.walkBob + dt * 9.0
    else
        runtime.walkBob = runtime.walkBob + dt * 2.5
    end
    local bob = math.sin(runtime.walkBob) * (moving and 5.0 or 1.4)
    local attackLift = 0.0
    if (runtime.playerAttackTimer or 0.0) > 0.0 then
        attackLift = math.sin((runtime.playerAttackTimer / 0.20) * 3.14159) * 7.0
    end
    local player = runtime.player
    corona.MeshComponent.set(entity, {
        position = { player.x, bob + attackLift, player.z },
        rotation = { 0.0, player.yaw, 0.0 },
        target_extent = runtime.characterTargetExtent,
    })
end

function script.update(entity, dt)
    local runtime = corona_dungeon_runtime
    if runtime == nil or runtime.player == nil then
        return
    end

    local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
    local fastMove =
        corona.is_key_down("SHIFT") or
        (gamepad.connected and ((gamepad.right_shoulder == true) or ((gamepad.right_trigger or 0.0) > 0.35) or (gamepad.left_thumb == true)))
    local moveSpeed = fastMove and 285.0 or 180.0

    local move = 0.0
    if corona.is_key_down("W") or corona.is_key_down("UP") then
        move = move + 1.0
    end
    if corona.is_key_down("S") or corona.is_key_down("DOWN") then
        move = move - 1.0
    end
    if gamepad.connected then
        move = move + (gamepad.left_y or 0.0)
        if gamepad.dpad_up then
            move = move + 1.0
        end
        if gamepad.dpad_down then
            move = move - 1.0
        end
    end

    local strafe = 0.0
    if corona.is_key_down("A") or corona.is_key_down("Q") then
        strafe = strafe + 1.0
    end
    if corona.is_key_down("D") or corona.is_key_down("E") then
        strafe = strafe - 1.0
    end
    if gamepad.connected then
        strafe = strafe - (gamepad.left_x or 0.0)
        if gamepad.dpad_left then
            strafe = strafe + 1.0
        end
        if gamepad.dpad_right then
            strafe = strafe - 1.0
        end
    end

    local moveLength = math.sqrt(move * move + strafe * strafe)
    if moveLength > 1.0 then
        move = move / moveLength
        strafe = strafe / moveLength
    end

    local forward = forward_from_yaw(runtime.cameraYaw)
    local right = { forward[3], 0.0, -forward[1] }
    local dx = (forward[1] * move + right[1] * strafe) * moveSpeed * dt
    local dz = (forward[3] * move + right[3] * strafe) * moveSpeed * dt
    local moving = math.abs(dx) + math.abs(dz) > 0.001
    try_move(runtime, dx, dz)

    update_attack(runtime, dt, gamepad)
    update_character(runtime, entity, dt, moving)
end

return script
