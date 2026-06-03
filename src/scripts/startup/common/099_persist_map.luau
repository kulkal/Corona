-- Auto-persist the current scene to assets/maps/<mode>.map on the first
-- run of any startup mode that doesn't already have a saved map. Runs
-- last (file prefix 099) so all spawn scripts have finished populating
-- the ECS by the time we serialize. Loading the map back is mode-
-- specific (gameplay state isn't fully serializable yet) — see the
-- individual mode scene scripts for inline load_map calls.

local mode = corona_script_startup_mode
if mode == nil or mode == "" then
    return
end

corona_script_mode_persist_state = corona_script_mode_persist_state or {}

if mode == "editor" then
    corona_script_mode_persist_state[mode] = {
        has_map = false,
        save_only = true,
    }
    return
end

-- Modes whose runtime state isn't restorable from the map yet (live
-- spine, runtime corona_*_runtime globals, etc.). Saving is still useful
-- as a snapshot for inspector / debugging but skipping load is required.
local saveOnly = {
    dungeon = true,
    platformer = true,
    spine_benchmark = true,
    sandbox = true,
}

if not corona.map_exists(mode) then
    local ok, err = corona.save_map(mode)
    if ok then
        corona.log("[map_persist] saved " .. mode .. " initial scene")
    else
        corona.log("[map_persist] save_map failed for " .. mode .. ": " .. tostring(err))
    end
end

-- Stash a flag so other scripts can tell whether map-based fast-load is
-- supported for this mode. Currently terrain is the only gameplay mode
-- with full round-trip support (editor uses the save-only branch above).
corona_script_mode_persist_state[mode] = {
    has_map = corona.map_exists(mode),
    save_only = saveOnly[mode] == true,
}
