corona_script_controls = corona_script_controls or {}
if corona_script_controls.platformer ~= nil and corona_script_controls.platformer.enabled then
    corona.log("procedural dungeon skipped for platformer mode")
    return {}
end

corona_script_controls.dungeon = corona_script_controls.dungeon or {}

local controls = corona_script_controls.dungeon

local function persistent_default(name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control("dungeon." .. name, value)
    end
    return value
end

if controls.enabled == nil then
    controls.enabled = persistent_default("enabled", false)
end

if not controls.enabled then
    return {}
end

corona_script_controls.pistol = corona_script_controls.pistol or {}
corona_script_controls.props = corona_script_controls.props or {}
corona_script_controls.pistol.enabled = false
corona_script_controls.props.buddhaEnabled = false
corona_script_controls.props.shaderBallEnabled = false

corona.set_default_world_visible(false)

local tile = 120.0
local mapWidth = 41
local mapHeight = 33
local wallHeight = 185.0
local doorHeight = 152.0
local floorThickness = 10.0
local floorChunkTiles = 5
local wallChunkTiles = 4
local seed = math.floor(persistent_default("seed", 20260504))
local rngState = seed

local function rand()
    rngState = (rngState * 1664525 + 1013904223) % 4294967296
    return rngState / 4294967296
end

local function rand_int(minValue, maxValue)
    if maxValue < minValue then
        return minValue
    end
    return minValue + math.floor(rand() * (maxValue - minValue + 1))
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

local floor = {}
local roomCell = {}
local cellKind = {}
local wall = {}
local visitedWall = {}
for y = 1, mapHeight do
    floor[y] = {}
    roomCell[y] = {}
    cellKind[y] = {}
    wall[y] = {}
    visitedWall[y] = {}
    for x = 1, mapWidth do
        floor[y][x] = false
        roomCell[y][x] = false
        cellKind[y][x] = ""
        wall[y][x] = false
        visitedWall[y][x] = false
    end
end

local leaves = {}

local function split_node(node, depth)
    local canSplitVertical = node.w >= 14
    local canSplitHorizontal = node.h >= 12
    if depth <= 0 or (not canSplitVertical and not canSplitHorizontal) then
        table.insert(leaves, node)
        return
    end

    local splitVertical = canSplitVertical and (not canSplitHorizontal or node.w > node.h or rand() < 0.5)
    if splitVertical then
        local split = rand_int(6, node.w - 7)
        node.a = { x = node.x, y = node.y, w = split, h = node.h }
        node.b = { x = node.x + split, y = node.y, w = node.w - split, h = node.h }
    else
        local split = rand_int(5, node.h - 6)
        node.a = { x = node.x, y = node.y, w = node.w, h = split }
        node.b = { x = node.x, y = node.y + split, w = node.w, h = node.h - split }
    end

    split_node(node.a, depth - 1)
    split_node(node.b, depth - 1)
end

local function carve_cell(x, y, kind)
    if x < 2 or x > mapWidth - 1 or y < 2 or y > mapHeight - 1 then
        return
    end
    floor[y][x] = true
    if kind == "room" or kind == "hall" then
        roomCell[y][x] = true
    end
    if cellKind[y][x] == "" then
        cellKind[y][x] = kind
    end
end

local function carve_rect(x, y, w, h, kind)
    for yy = y, y + h - 1 do
        for xx = x, x + w - 1 do
            carve_cell(xx, yy, kind)
        end
    end
end

local function carve_corridor_cell(x, y)
    if x < 2 or x > mapWidth - 1 or y < 2 or y > mapHeight - 1 then
        return
    end
    if not floor[y][x] then
        cellKind[y][x] = "corridor"
    end
    floor[y][x] = true
end

local function carve_hline(x0, x1, y)
    local a = math.min(x0, x1)
    local b = math.max(x0, x1)
    for x = a, b do
        carve_corridor_cell(x, y)
    end
end

local function carve_vline(y0, y1, x)
    local a = math.min(y0, y1)
    local b = math.max(y0, y1)
    for y = a, b do
        carve_corridor_cell(x, y)
    end
end

local function carve_corridor(a, b)
    if rand() < 0.5 then
        carve_hline(a.x, b.x, a.y)
        carve_vline(a.y, b.y, b.x)
    else
        carve_vline(a.y, b.y, a.x)
        carve_hline(a.x, b.x, b.y)
    end
end

local root = { x = 2, y = 2, w = mapWidth - 3, h = mapHeight - 3 }
split_node(root, 5)

local hallLeaf = leaves[1]
for _, leaf in ipairs(leaves) do
    if leaf.w * leaf.h > hallLeaf.w * hallLeaf.h then
        hallLeaf = leaf
    end
end

local rooms = {}
for _, leaf in ipairs(leaves) do
    local isHall = leaf == hallLeaf
    local maxRoomW = math.max(4, leaf.w - 2)
    local maxRoomH = math.max(4, leaf.h - 2)
    local roomW = 0
    local roomH = 0
    if isHall then
        roomW = clamp(rand_int(8, 11), 4, maxRoomW)
        roomH = clamp(rand_int(6, 9), 4, maxRoomH)
    else
        roomW = clamp(rand_int(4, 7), 3, maxRoomW)
        roomH = clamp(rand_int(3, 6), 3, maxRoomH)
    end

    local roomX = leaf.x + rand_int(1, math.max(1, leaf.w - roomW - 1))
    local roomY = leaf.y + rand_int(1, math.max(1, leaf.h - roomH - 1))
    local kind = isHall and "hall" or "room"
    carve_rect(roomX, roomY, roomW, roomH, kind)

    leaf.center = {
        x = roomX + math.floor(roomW * 0.5),
        y = roomY + math.floor(roomH * 0.5),
    }
    leaf.room = { x = roomX, y = roomY, w = roomW, h = roomH, hall = isHall }
    table.insert(rooms, leaf.room)
end

local function connect_node(node)
    if node.a == nil or node.b == nil then
        return node.center
    end
    local a = connect_node(node.a)
    local b = connect_node(node.b)
    carve_corridor(a, b)
    return rand() < 0.5 and a or b
end

connect_node(root)

for y = 2, mapHeight - 1 do
    for x = 2, mapWidth - 1 do
        if not floor[y][x] then
            local nearFloor = false
            for dy = -1, 1 do
                for dx = -1, 1 do
                    if floor[y + dy] ~= nil and floor[y + dy][x + dx] then
                        nearFloor = true
                    end
                end
            end
            wall[y][x] = nearFloor
        end
    end
end

local originX = (mapWidth + 1) * 0.5
local originY = (mapHeight + 1) * 0.5

local function cell_world(x, y)
    return {
        (x - originX) * tile,
        (y - originY) * tile,
    }
end

local function rect_world_center(x, y, w, h)
    local cx = x + (w - 1) * 0.5
    local cy = y + (h - 1) * 0.5
    return cell_world(cx, cy)
end

local function floor_at(x, y)
    return floor[y] ~= nil and floor[y][x] == true
end

local function cell_key(x, y)
    return tostring(x) .. ":" .. tostring(y)
end

local startRoom = rooms[1]
local endRoom = rooms[#rooms]
local startCell = {
    x = startRoom.x + math.floor(startRoom.w * 0.5),
    y = startRoom.y + math.floor(startRoom.h * 0.5),
}
local endCell = {
    x = endRoom.x + math.floor(endRoom.w * 0.5),
    y = endRoom.y + math.floor(endRoom.h * 0.5),
}

local function choose_door()
    local best = nil
    local bestScore = -1.0
    for y = 3, mapHeight - 2 do
        for x = 3, mapWidth - 2 do
            if floor[y][x] and not roomCell[y][x] then
                local verticalCorridor = floor_at(x, y - 1) and floor_at(x, y + 1) and not floor_at(x - 1, y) and not floor_at(x + 1, y)
                local horizontalCorridor = floor_at(x - 1, y) and floor_at(x + 1, y) and not floor_at(x, y - 1) and not floor_at(x, y + 1)
                if verticalCorridor or horizontalCorridor then
                    local dx = x - startCell.x
                    local dy = y - startCell.y
                    local distanceScore = dx * dx + dy * dy
                    if distanceScore > bestScore then
                        bestScore = distanceScore
                        best = {
                            x = x,
                            y = y,
                            axis = verticalCorridor and "z" or "x",
                        }
                    end
                end
            end
        end
    end
    if best == nil then
        best = { x = endCell.x, y = endCell.y, axis = "x" }
    end
    return best
end

local door = choose_door()
local floorMesh = { primitive = "box", color = { 0.34, 0.33, 0.30 } }
local wallMesh = { primitive = "box", color = { 0.96, 0.92, 0.86 }, brick_texture = true, uv_repeat = 6.0 }
local pillarMesh = { primitive = "box", color = { 0.78, 0.74, 0.68 }, brick_texture = true, uv_repeat = 4.0 }

local spawned = {}

local function spawn_box(meshDesc, position, scale, roughness, physics)
    local entity = corona.Entity.create("DungeonBox")
    local ok = corona.MeshComponent.add(entity, {
        primitive = meshDesc.primitive,
        color = meshDesc.color,
        brick_texture = meshDesc.brick_texture,
        uv_repeat = meshDesc.uv_repeat,
        position = position,
        scale = scale,
        roughness = roughness or 0.84,
        metallic = 0.0,
        override_material = true,
        ray_tracing = true,
        physics = physics,
    })
    if not ok then
        corona.Entity.destroy(entity)
        return nil
    end
    table.insert(spawned, entity)
    return entity
end

for chunkY = 2, mapHeight - 1, floorChunkTiles do
    for chunkX = 2, mapWidth - 1, floorChunkTiles do
        local chunkEndX = math.min(mapWidth - 1, chunkX + floorChunkTiles - 1)
        local chunkEndY = math.min(mapHeight - 1, chunkY + floorChunkTiles - 1)
        local hasFloor = false
        for y = chunkY, chunkEndY do
            for x = chunkX, chunkEndX do
                if floor[y][x] or wall[y][x] then
                    hasFloor = true
                end
            end
        end

        if hasFloor then
            local chunkWidth = chunkEndX - chunkX + 1
            local chunkHeight = chunkEndY - chunkY + 1
            local center = rect_world_center(chunkX, chunkY, chunkWidth, chunkHeight)
            spawn_box(
                floorMesh,
                { center[1], -floorThickness, center[2] },
                { chunkWidth * tile, floorThickness, chunkHeight * tile },
                0.9,
                true)
        end
    end
end

for y = 2, mapHeight - 1 do
    for x = 2, mapWidth - 1 do
        if wall[y][x] and not visitedWall[y][x] then
            local width = 1
            while width < wallChunkTiles and x + width <= mapWidth - 1 and wall[y][x + width] and not visitedWall[y][x + width] do
                width = width + 1
            end

            local height = 1
            local canGrow = true
            while canGrow and height < wallChunkTiles and y + height <= mapHeight - 1 do
                for xx = x, x + width - 1 do
                    if not wall[y + height][xx] or visitedWall[y + height][xx] then
                        canGrow = false
                    end
                end
                if canGrow then
                    height = height + 1
                end
            end

            for yy = y, y + height - 1 do
                for xx = x, x + width - 1 do
                    visitedWall[yy][xx] = true
                end
            end

            local center = rect_world_center(x, y, width, height)
            spawn_box(
                wallMesh,
                { center[1], 0.0, center[2] },
                { width * tile, wallHeight, height * tile },
                0.82,
                true)
        end
    end
end

for _, room in ipairs(rooms) do
    if room.hall then
        local insetX = math.max(1, math.floor(room.w * 0.24))
        local insetY = math.max(1, math.floor(room.h * 0.24))
        local pillarCells = {
            { room.x + insetX, room.y + insetY },
            { room.x + room.w - 1 - insetX, room.y + insetY },
            { room.x + insetX, room.y + room.h - 1 - insetY },
            { room.x + room.w - 1 - insetX, room.y + room.h - 1 - insetY },
        }
        for _, cell in ipairs(pillarCells) do
            local p = cell_world(cell[1], cell[2])
            spawn_box(pillarMesh, { p[1], 0.0, p[2] }, { 34.0, 138.0, 34.0 }, 0.78, true)
        end
    end
end

local doorWorld = cell_world(door.x, door.y)
local doorScale = door.axis == "z" and { tile * 0.88, doorHeight, 18.0 } or { 18.0, doorHeight, tile * 0.88 }
local doorEntity = corona.Entity.create("DungeonDoor")
local doorOk = corona.MeshComponent.add(doorEntity, {
    primitive = "box",
    color = { 0.45, 0.23, 0.11 },
    position = { doorWorld[1], 0.0, doorWorld[2] },
    scale = doorScale,
    roughness = 0.62,
    metallic = 0.0,
    override_material = true,
    ray_tracing = true,
    physics = false,
})
if not doorOk then
    corona.Entity.destroy(doorEntity)
    doorEntity = nil
end

local startWorld = cell_world(startCell.x, startCell.y)
local endWorld = cell_world(endCell.x, endCell.y)
local player = {
    x = startWorld[1],
    z = startWorld[2],
    yaw = math.deg(math.atan2(endWorld[1] - startWorld[1], endWorld[2] - startWorld[2])),
    health = 100.0,
    maxHealth = 100.0,
}
local character = corona.Entity.create("DungeonCharacter")
local characterOk = corona.MeshComponent.add(character, {
    primitive = "block_character",
    seed = seed + 77,
    position = { player.x, 0.0, player.z },
    rotation = { 0.0, player.yaw, 0.0 },
    target_extent = 82.0,
    roughness = 0.74,
    metallic = 0.0,
    override_material = true,
    ray_tracing = true,
    physics = false,
})
if not characterOk then
    corona.Entity.destroy(character)
    character = nil
end

local enemyStates = {}
local enemyByEntity = {}
local usedEnemyCells = {}
usedEnemyCells[cell_key(endCell.x, endCell.y)] = true

local function is_enemy_spawn_cell(x, y, minStartDistanceSq)
    if not floor_at(x, y) then
        return false
    end
    if x == endCell.x and y == endCell.y then
        return false
    end
    if door ~= nil and x == door.x and y == door.y then
        return false
    end
    local dx = x - startCell.x
    local dy = y - startCell.y
    if dx * dx + dy * dy < minStartDistanceSq then
        return false
    end
    return usedEnemyCells[cell_key(x, y)] ~= true
end

local function choose_enemy_cell(minStartDistanceSq)
    for _ = 1, 180 do
        local x = rand_int(2, mapWidth - 1)
        local y = rand_int(2, mapHeight - 1)
        if is_enemy_spawn_cell(x, y, minStartDistanceSq) then
            usedEnemyCells[cell_key(x, y)] = true
            return { x = x, y = y }
        end
    end

    for y = 2, mapHeight - 1 do
        for x = 2, mapWidth - 1 do
            if is_enemy_spawn_cell(x, y, minStartDistanceSq) then
                usedEnemyCells[cell_key(x, y)] = true
                return { x = x, y = y }
            end
        end
    end
    return { x = endCell.x, y = endCell.y }
end

local function register_enemy(state)
    table.insert(enemyStates, state)
    if state.entity ~= nil then
        enemyByEntity[state.entity] = state
    end
end

local function spawn_enemy(index)
    local cell = choose_enemy_cell(30)
    local world = cell_world(cell.x, cell.y)
    local enemy = corona.Entity.create("DungeonEnemy")
    local yaw = rand() * 360.0
    local ok = corona.MeshComponent.add(enemy, {
        primitive = "block_character",
        seed = seed + 900 + index * 17,
        position = { world[1], 0.0, world[2] },
        rotation = { 0.0, yaw, 0.0 },
        target_extent = 68.0,
        roughness = 0.78,
        metallic = 0.0,
        override_material = true,
        ray_tracing = true,
        physics = false,
    })
    if not ok then
        corona.Entity.destroy(enemy)
        return nil
    end

    local state = {
        entity = enemy,
        kind = "enemy",
        x = world[1],
        z = world[2],
        meshY = 0.0,
        yaw = yaw,
        health = 70.0,
        maxHealth = 70.0,
        radius = 38.0,
        targetExtent = 68.0,
        speed = 78.0 + rand() * 22.0,
        aggroRange = tile * 5.5,
        stopDistance = 88.0,
        wanderAngle = yaw,
        wanderTimer = rand() * 1.4,
        walkBob = rand() * 6.28,
        hitFlash = 0.0,
        stunTimer = 0.0,
        attackDamage = 7.0,
        attackCooldown = 1.05,
        attackTimer = rand() * 0.7,
        alive = true,
    }
    register_enemy(state)
    return state
end

for i = 1, 5 do
    spawn_enemy(i)
end

local bossWorld = cell_world(endCell.x, endCell.y)
local boss = corona.Entity.create("DungeonBossBuddha")
local bossYaw = math.deg(math.atan2(startWorld[1] - bossWorld[1], startWorld[2] - bossWorld[2]))
local bossOk = corona.MeshComponent.add(boss, {
    path = "assets/buddha/buddha.obj",
    position = { bossWorld[1], -8.0, bossWorld[2] },
    rotation = { 0.0, bossYaw, 0.0 },
    target_extent = 190.0,
    roughness = 0.65,
    metallic = 0.0,
    ray_tracing = true,
    physics = false,
})
if bossOk then
    usedEnemyCells[cell_key(endCell.x, endCell.y)] = true
    register_enemy({
        entity = boss,
        kind = "boss",
        x = bossWorld[1],
        z = bossWorld[2],
        meshY = -8.0,
        yaw = bossYaw,
        health = 260.0,
        maxHealth = 260.0,
        radius = 96.0,
        targetExtent = 190.0,
        speed = 42.0,
        aggroRange = tile * 8.0,
        stopDistance = 150.0,
        wanderAngle = bossYaw,
        wanderTimer = 0.0,
        walkBob = 0.0,
        hitFlash = 0.0,
        stunTimer = 0.0,
        attackDamage = 18.0,
        attackCooldown = 1.55,
        attackTimer = 0.6,
        alive = true,
    })
else
    corona.Entity.destroy(boss)
    boss = nil
end

local cameraEntity = corona.Entity.create("DungeonCamera")
corona.CameraComponent.add(cameraEntity, {
    position = { player.x, 180.0, player.z - 300.0 },
    look_at = { player.x, 72.0, player.z },
    up = { 0.0, 1.0, 0.0 },
    fov = 0.8,
    near_plane = 10.0,
    far_plane = 20000.0,
    active = true,
})

local sunEntity = corona.Entity.create("DungeonSun")
local sunOk = corona.LightComponent.add(sunEntity, {
    type = "directional",
    enabled = true,
    direction = { 0.44, 0.82, 0.36 },
    color = { 1.0, 0.95, 0.84 },
    intensity = 0.72,
    persistent = false,
})
if not sunOk then
    corona.Entity.destroy(sunEntity)
    sunEntity = nil
end

local hudEntity = corona.Entity.create("DungeonHud")

corona_dungeon_runtime = {
    tile = tile,
    originX = originX,
    originY = originY,
    floor = floor,
    door = door,
    doorWorld = doorWorld,
    doorEntity = doorEntity,
    doorScale = doorScale,
    doorInteractDistance = tile * 1.35,
    doorOpen = false,
    doorOpenAmount = 0.0,
    player = player,
    enemies = enemyStates,
    enemyByEntity = enemyByEntity,
    playerAttackCooldown = 0.0,
    playerAttackTimer = 0.0,
    playerAttackRange = 150.0,
    playerAttackHalfAngleCos = 0.42,
    playerAttackDamage = 35.0,
    playerHurtTimer = 0.0,
    walkBob = 0.0,
    cameraYaw = player.yaw,
    cameraPitch = 26.0,
    mouseSensitivity = math.deg(0.003),
    gamepadYawSpeed = 165.0,
    gamepadPitchSpeed = 115.0,
    cameraEntity = cameraEntity,
    sunEntity = sunEntity,
    hudEntity = hudEntity,
    sunAngle = 28.0,
    sunAngularSpeed = 8.0,
    sunIntensity = 0.72,
    characterTargetExtent = 82.0,
}

corona.set_camera_control(true)
if character ~= nil then
    local attached = corona.ScriptComponent.attach_file(character, "scripts/entity/dungeon_character_controller.luau")
    if not attached then
        corona.log("failed to attach dungeon character controller")
    end
end
if doorEntity ~= nil then
    local attached = corona.ScriptComponent.attach_file(doorEntity, "scripts/entity/dungeon_door_controller.luau")
    if not attached then
        corona.log("failed to attach dungeon door controller")
    end
end
for _, enemy in ipairs(enemyStates) do
    if enemy.entity ~= nil and enemy.alive then
        local attached = corona.ScriptComponent.attach_file(enemy.entity, "scripts/entity/dungeon_enemy_controller.luau")
        if not attached then
            corona.log("failed to attach dungeon enemy controller")
        end
    end
end
if cameraEntity ~= nil then
    local attached = corona.ScriptComponent.attach_file(cameraEntity, "scripts/entity/dungeon_camera_controller.luau")
    if not attached then
        corona.log("failed to attach dungeon camera controller")
    end
end
if sunEntity ~= nil then
    local attached = corona.ScriptComponent.attach_file(sunEntity, "scripts/entity/dungeon_sun_controller.luau")
    if not attached then
        corona.log("failed to attach dungeon sun controller")
    end
end
if hudEntity ~= nil then
    local attached = corona.ScriptComponent.attach_file(hudEntity, "scripts/entity/dungeon_hud.luau")
    if not attached then
        corona.log("failed to attach dungeon HUD")
    end
end

local dungeonScript = {}
corona.log("procedural BSP dungeon loaded")

function dungeonScript.shutdown()
    if character ~= nil then
        corona.Entity.destroy(character)
        character = nil
    end
    if doorEntity ~= nil then
        corona.Entity.destroy(doorEntity)
        doorEntity = nil
    end
    for _, enemy in ipairs(enemyStates) do
        if enemy.entity ~= nil then
            corona.Entity.destroy(enemy.entity)
            enemy.entity = nil
        end
    end
    enemyStates = {}
    enemyByEntity = {}
    if cameraEntity ~= nil then
        corona.Entity.destroy(cameraEntity)
        cameraEntity = nil
    end
    if sunEntity ~= nil then
        corona.Entity.destroy(sunEntity)
        sunEntity = nil
    end
    if hudEntity ~= nil then
        corona.Entity.destroy(hudEntity)
        hudEntity = nil
    end
    for _, handle in ipairs(spawned) do
        corona.Entity.destroy(handle)
    end
    spawned = {}
    corona_dungeon_runtime = nil
    corona.set_default_world_visible(false)
end

return dungeonScript
