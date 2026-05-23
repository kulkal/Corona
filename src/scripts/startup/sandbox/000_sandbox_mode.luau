local script = {}

corona_script_startup_mode = "sandbox"

corona_script_controls = corona_script_controls or {}
corona_script_controls.platformer = corona_script_controls.platformer or {}
corona_script_controls.dungeon = corona_script_controls.dungeon or {}

corona_script_controls.platformer.enabled = false
corona_script_controls.dungeon.enabled = false

if corona.set_persistent_control ~= nil then
    corona.set_persistent_control("platformer.enabled", false)
    corona.set_persistent_control("dungeon.enabled", false)
end

corona.log("startup mode selected: sandbox")

return script
