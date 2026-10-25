-- Loaded from ~/.config/hypr/hyprland.local.lua by the desktop installer.
local deck_class = "org.cyberdeck.Terminal"
local deck_workspace = "special:CyberDeck"
local source = debug.getinfo(1, "S").source
local desktop_dir = assert(source:match("^@(.+)/[^/]+$"), "Cannot locate Cyber Deck launcher")
local deck_launcher = desktop_dir .. "/launch-cyber-deck.sh"

local function find_deck_window()
    for _, window in ipairs(hl.get_windows()) do
        if window.class == deck_class then return window end
    end
    return nil
end

-- Re-evaluating this file in a running Hyprland session must replace the
-- earlier launch-only binding instead of leaving both actions on the key.
hl.unbind("CTRL + SUPER + ALT + Y")
hl.bind("CTRL + SUPER + ALT + Y", function()
    local window = find_deck_window()
    local active_special = hl.get_active_special_workspace()
    local deck_is_visible = active_special and active_special.name == deck_workspace

    if window then
        if not window.workspace or window.workspace.name ~= deck_workspace then
            hl.dispatch(hl.dsp.window.move({ window = window, workspace = deck_workspace, follow = false }))
        end
        hl.dispatch(hl.dsp.workspace.toggle_special("CyberDeck"))
        return
    end

    if not deck_is_visible then
        hl.dispatch(hl.dsp.workspace.toggle_special("CyberDeck"))
    end
    hl.dispatch(hl.dsp.exec_cmd(deck_launcher))
end, { description = "Show or hide Cyber Deck" })

hl.window_rule({
    name = "local-cyber-deck-terminal",
    match = { class = "^org\\.cyberdeck\\.Terminal$" },
    float = true,
    center = true,
    workspace = deck_workspace,
})
