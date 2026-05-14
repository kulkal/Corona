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

local function mix(a, b, t)
    return a + (b - a) * t
end

local function sun_direction(angleDegrees)
    local orbit = math.rad(angleDegrees)
    local elevationDegrees = 18.0 + (math.sin(orbit) * 0.5 + 0.5) * 44.0
    local elevation = math.rad(elevationDegrees)
    local azimuth = orbit * 0.72
    local horizontal = math.cos(elevation)
    return {
        math.sin(azimuth) * horizontal,
        math.sin(elevation),
        math.cos(azimuth) * horizontal,
    }, elevationDegrees
end

function script.update(entity, dt)
    local runtime = corona_dungeon_runtime
    if runtime == nil or entity == nil then
        return
    end

    runtime.sunAngle = (runtime.sunAngle or 0.0) + dt * (runtime.sunAngularSpeed or 8.0)
    if runtime.sunAngle > 360.0 then
        runtime.sunAngle = runtime.sunAngle - 360.0
    end

    local direction, elevationDegrees = sun_direction(runtime.sunAngle)
    local daylight = clamp((elevationDegrees - 18.0) / 44.0, 0.0, 1.0)
    local warm = 1.0 - daylight
    local baseIntensity = runtime.sunIntensity or 0.72
    local intensity = baseIntensity * mix(0.62, 1.18, daylight)
    local color = {
        1.0,
        mix(0.80, 0.96, daylight),
        mix(0.58, 0.88, daylight) + warm * 0.02,
    }

    corona.LightComponent.set(entity, {
        type = "directional",
        enabled = true,
        direction = direction,
        color = color,
        intensity = intensity,
        persistent = false,
    })
end

return script
