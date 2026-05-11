corona_script_controls = corona_script_controls or {}
corona_script_controls.dungeon = corona_script_controls.dungeon or {}

local controls = corona_script_controls.dungeon

local function persistent_default(name, value)
    if corona.get_persistent_control ~= nil then
        return corona.get_persistent_control("dungeon." .. name, value)
    end
    return value
end

if controls.enabled == nil then
    controls.enabled = persistent_default("enabled", true)
end

if not controls.enabled then
    return {}
end

corona_script_controls.pistol = corona_script_controls.pistol or {}
corona_script_controls.props = corona_script_controls.props or {}
corona_script_controls.blockCharacter = corona_script_controls.blockCharacter or {}
corona_script_controls.pistol.enabled = false
corona_script_controls.props.buddhaEnabled = false
corona_script_controls.props.shaderBallEnabled = false
corona_script_controls.blockCharacter.enabled = false

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

local function vec_add(a, b)
    return { a[1] + b[1], a[2] + b[2], a[3] + b[3] }
end

local function vec_mul(a, s)
    return { a[1] * s, a[2] * s, a[3] * s }
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

local function world_to_cell(x, z)
    return {
        x = math.floor(x / tile + originX + 0.5),
        y = math.floor(z / tile + originY + 0.5),
    }
end

local function floor_at(x, y)
    return floor[y] ~= nil and floor[y][x] == true
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
local floorScene = corona.create_box_scene({ color = { 0.34, 0.33, 0.30 } })
local wallScene = corona.create_box_scene({ color = { 0.96, 0.92, 0.86 }, brick_texture = true, uv_repeat = 6.0 })
local pillarScene = corona.create_box_scene({ color = { 0.78, 0.74, 0.68 }, brick_texture = true, uv_repeat = 4.0 })
local doorScene = corona.create_box_scene({ color = { 0.45, 0.23, 0.11 } })
local characterScene = corona.create_block_character_scene({ seed = seed + 77 })

local spawned = {}

local function spawn_box(sceneHandle, position, scale, roughness, physics)
    local handle = corona.spawn(sceneHandle, {
        position = position,
        scale = scale,
        roughness = roughness or 0.84,
        metallic = 0.0,
        override_material = true,
        ray_tracing = true,
        physics = physics,
    })
    table.insert(spawned, handle)
    return handle
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
                floorScene,
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
                wallScene,
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
            spawn_box(pillarScene, { p[1], 0.0, p[2] }, { 34.0, 138.0, 34.0 }, 0.78, true)
        end
    end
end

local doorWorld = cell_world(door.x, door.y)
local doorScale = door.axis == "z" and { tile * 0.88, doorHeight, 18.0 } or { 18.0, doorHeight, tile * 0.88 }
local doorHandle = spawn_box(doorScene, { doorWorld[1], 0.0, doorWorld[2] }, doorScale, 0.62, false)

local startWorld = cell_world(startCell.x, startCell.y)
local endWorld = cell_world(endCell.x, endCell.y)
local player = {
    x = startWorld[1],
    z = startWorld[2],
    yaw = math.deg(math.atan2(endWorld[1] - startWorld[1], endWorld[2] - startWorld[2])),
}
local character = corona.spawn(characterScene, {
    position = { player.x, 0.0, player.z },
    rotation = { 0.0, player.yaw, 0.0 },
    target_extent = 82.0,
    roughness = 0.74,
    metallic = 0.0,
    override_material = true,
    ray_tracing = true,
    physics = false,
})

local doorOpen = false
local doorOpenAmount = 0.0
local walkBob = 0.0
local cameraYaw = player.yaw
local cameraPitch = 26.0
local mouseSensitivity = math.deg(0.003)
local gamepadYawSpeed = 165.0
local gamepadPitchSpeed = 115.0

local function forward_from_yaw(yaw)
    local radians = math.rad(yaw)
    return {
        math.sin(radians),
        0.0,
        math.cos(radians),
    }
end

local function is_door_blocking_cell(cell)
    return not doorOpen and cell.x == door.x and cell.y == door.y
end

local function is_walkable_position(x, z)
    local radius = tile * 0.28
    local samples = {
        { 0.0, 0.0 },
        { radius, 0.0 },
        { -radius, 0.0 },
        { 0.0, radius },
        { 0.0, -radius },
    }

    for _, sample in ipairs(samples) do
        local cell = world_to_cell(x + sample[1], z + sample[2])
        if not floor_at(cell.x, cell.y) or is_door_blocking_cell(cell) then
            return false
        end
    end
    return true
end

local function try_move(dx, dz)
    local nextX = player.x + dx
    local nextZ = player.z + dz
    if is_walkable_position(nextX, player.z) then
        player.x = nextX
    end
    if is_walkable_position(player.x, nextZ) then
        player.z = nextZ
    end
end

local function update_door(dt)
    local target = doorOpen and 1.0 or 0.0
    doorOpenAmount = doorOpenAmount + (target - doorOpenAmount) * clamp(dt * 9.0, 0.0, 1.0)
    local yaw = doorOpenAmount * 88.0
    if door.axis == "x" then
        yaw = -yaw
    end
    corona.set_transform(doorHandle, {
        position = { doorWorld[1], 0.0, doorWorld[2] },
        rotation = { 0.0, yaw, 0.0 },
        scale = doorScale,
    })
end

local function update_character(dt, moving)
    if moving then
        walkBob = walkBob + dt * 9.0
    else
        walkBob = walkBob + dt * 2.5
    end
    local bob = math.sin(walkBob) * (moving and 5.0 or 1.4)
    corona.set_transform(character, {
        position = { player.x, bob, player.z },
        rotation = { 0.0, player.yaw, 0.0 },
        target_extent = 82.0,
    })
end

local function update_camera()
    local forward = forward_from_yaw(cameraYaw)
    local focus = { player.x, 72.0, player.z }
    local pitchRadians = math.rad(cameraPitch)
    local cameraDistance = 300.0
    local horizontalDistance = math.cos(pitchRadians) * cameraDistance
    local verticalOffset = 80.0 + math.sin(pitchRadians) * cameraDistance
    local desired = vec_add(focus, vec_add(vec_mul(forward, -horizontalDistance), { 0.0, verticalOffset, 0.0 }))
    corona.set_camera({
        position = desired,
        look_at = vec_add(focus, vec_mul(forward, 86.0)),
        up = { 0.0, 1.0, 0.0 },
    })
end

corona.set_camera_control(true)
update_character(0.0, false)
update_door(0.0)
update_camera()
corona.log("procedural BSP dungeon loaded")

local dungeonScript = {}

function dungeonScript.update(dt)
    local gamepad = corona.get_gamepad ~= nil and corona.get_gamepad() or { connected = false }
    local fastMove = corona.is_key_down("SHIFT") or (gamepad.connected and (gamepad.right_shoulder or gamepad.right_trigger > 0.35 or gamepad.left_thumb))
    local turnSpeed = fastMove and 170.0 or 125.0
    local moveSpeed = fastMove and 285.0 or 180.0

    local mouse = corona.get_mouse()
    if mouse.dx ~= 0 or mouse.dy ~= 0 then
        cameraYaw = cameraYaw - mouse.dx * mouseSensitivity
        cameraPitch = clamp(cameraPitch + mouse.dy * mouseSensitivity, 12.0, 52.0)
    end
    if gamepad.connected then
        cameraYaw = cameraYaw - gamepad.right_x * gamepadYawSpeed * dt
        cameraPitch = clamp(cameraPitch - gamepad.right_y * gamepadPitchSpeed * dt, 12.0, 52.0)
    end
    if corona.is_key_down("LEFT") then
        cameraYaw = cameraYaw + turnSpeed * dt
    end
    if corona.is_key_down("RIGHT") then
        cameraYaw = cameraYaw - turnSpeed * dt
    end
    player.yaw = cameraYaw

    local move = 0.0
    if corona.is_key_down("W") or corona.is_key_down("UP") then
        move = move + 1.0
    end
    if corona.is_key_down("S") or corona.is_key_down("DOWN") then
        move = move - 1.0
    end
    if gamepad.connected then
        move = move + gamepad.left_y
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
        strafe = strafe - gamepad.left_x
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

    local forward = forward_from_yaw(cameraYaw)
    local right = { forward[3], 0.0, -forward[1] }
    local dx = (forward[1] * move + right[1] * strafe) * moveSpeed * dt
    local dz = (forward[3] * move + right[3] * strafe) * moveSpeed * dt
    local moving = math.abs(dx) + math.abs(dz) > 0.001
    try_move(dx, dz)

    if corona.was_key_pressed("SPACE") or (gamepad.connected and (gamepad.a_pressed or gamepad.x_pressed)) then
        local distanceToDoor = math.sqrt((player.x - doorWorld[1]) * (player.x - doorWorld[1]) + (player.z - doorWorld[2]) * (player.z - doorWorld[2]))
        if distanceToDoor < tile * 1.35 then
            doorOpen = not doorOpen
        end
    end

    update_door(dt)
    update_character(dt, moving)
    update_camera()
end

function dungeonScript.shutdown()
    if character ~= nil then
        corona.destroy(character)
        character = nil
    end
    for _, handle in ipairs(spawned) do
        corona.destroy(handle)
    end
    spawned = {}
    corona.set_default_world_visible(true)
end

return dungeonScript
