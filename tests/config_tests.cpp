// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaode/config.hpp"
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void rejects(const std::string &source) {
    try {
        (void)shaode::parse_config(source);
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("invalid configuration was accepted: " + source);
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "example config path required");
        auto config = shaode::load_config(argv[1]);
        require(config.bindings.size() == 24, "example shortcuts missing");
        require(config.shell.enabled && config.shell.panel_height == 52 &&
                    config.shell.launchers.size() == 2,
                "example shell settings missing");
        require(config.shell.launchers.front().command == shaode::Command{"kitty"},
                "pinned command arguments changed");
        auto *spawn = config.binding(SH_LOGO, XKB_KEY_q);
        require(spawn && spawn->command == shaode::Command{"kitty"}, "spawn argv mismatch");
        require(config.binding(SH_LOGO | SH_SHIFT | 2, XKB_KEY_R)->action == SH_RELOAD,
                "shifted shortcut or CapsLock normalization failed");
        require(!config.binding(SH_LOGO | SH_CTRL, XKB_KEY_q), "extra modifiers matched");
        auto *fullscreen = config.binding(SH_LOGO, XKB_KEY_f);
        require(fullscreen && fullscreen->action == SH_FULLSCREEN, "fullscreen binding missing");
        auto *move = config.binding(SH_LOGO | SH_SHIFT, XKB_KEY_3);
        require(move && move->action == SH_MOVE_TO_WORKSPACE && move->workspace == 3,
                "move-to-workspace binding missing");
        require(config.settings.workspaces == 4, "example workspace count changed");
        require(!config.settings.tiling, "example starts tiled");
        auto *toggle = config.binding(SH_LOGO | SH_SHIFT, XKB_KEY_T);
        require(toggle && toggle->action == SH_TOGGLE_TILING, "tiling toggle binding missing");
        require(shaode::parse_config("return {layout={tiling=true}}").settings.tiling,
                "layout.tiling not parsed");
        auto *launcher = config.binding(SH_LOGO, XKB_KEY_r);
        require(launcher && launcher->action == SH_LAUNCHER, "launcher binding missing");
        require(shaode::parse_action("toggle_floating") == SH_TOGGLE_FLOATING,
                "toggle_floating action missing");
        rejects("return {layout={tiling='yes'}}");
        auto outputs =
            shaode::parse_config("return {outputs={order={'HDMI-A-1','DP-3'},primary='DP-3'}}");
        require(outputs.settings.output_count == 2 &&
                    std::string(outputs.settings.output_order[1]) == "DP-3" &&
                    std::string(outputs.settings.primary_output) == "DP-3",
                "output order or primary not parsed");
        rejects("return {outputs={order={'DP-1','DP-1'}}}");
        rejects("return {outputs={order={''}}}");
        rejects("return {outputs={order={'1','2','3','4','5','6','7','8','9'}}}");
        rejects("return {outputs={primary=1}}");
        rejects("return {outputs={position={}}}");
        auto monitors = shaode::parse_config(
            "return {outputs={monitors={['DP-3']={mode='2560x1440@143.98',scale=1.25,"
            "position={x=-2048,y=0},transform=1},['HDMI-A-1']={enabled=false}}}}");
        require(monitors.settings.monitor_count == 2, "monitors not parsed");
        for (int i = 0; i < 2; ++i) {
            const auto &m = monitors.settings.monitors[i];
            if (std::string(m.name) == "DP-3")
                require(m.enabled && m.width == 2560 && m.height == 1440 && m.refresh == 143980 &&
                            m.scale == 1.25F && m.positioned && m.x == -2048 && m.y == 0 &&
                            m.transform == 1,
                        "monitor settings mismatch");
            else
                require(std::string(m.name) == "HDMI-A-1" && !m.enabled && m.width == 0 &&
                            !m.positioned,
                        "disabled monitor mismatch");
        }
        auto plain = shaode::parse_config("return {outputs={monitors={X={mode='800x600'}}}}");
        require(plain.settings.monitors[0].refresh == 0 && plain.settings.monitors[0].enabled,
                "mode without refresh mismatch");
        rejects("return {outputs={monitors={'DP-1'}}}");
        rejects("return {outputs={monitors={['']={}}}}");
        rejects("return {outputs={monitors={X={mode='2560x1440@'}}}}");
        rejects("return {outputs={monitors={X={mode='2560x1440x'}}}}");
        rejects("return {outputs={monitors={X={mode='0x1440'}}}}");
        rejects("return {outputs={monitors={X={scale=0}}}}");
        rejects("return {outputs={monitors={X={transform=8}}}}");
        rejects("return {outputs={monitors={X={position={x=1}}}}}");
        rejects("return {outputs={monitors={X={position={x=1,y=2,z=3}}}}}");
        rejects("return {outputs={monitors={X={refresh=60}}}}");
        require(shaode::parse_action("workspace_next") == SH_WORKSPACE_NEXT,
                "control action names differ from Lua");
        rejects("return {bindings={{mods={'Alt'},key='1',action='workspace'}}}");
        rejects("return {bindings={{mods={'Alt'},key='1',action='workspace',workspace=5}}}");
        rejects("return {layout={workspaces=2},bindings={{mods={'Alt'},key='1',"
                "action='move_to_workspace',workspace=3}}}");
        rejects("return {bindings={{mods={'Alt'},key='1',action='close',workspace=1}}}");
        rejects("return {layout={workspaces=0}}");
        rejects("return {layout={workspaces=11}}");
        auto computed = shaode::parse_config("local gap = 3; return {layout={gap=gap*2}}");
        require(computed.settings.gap_inner == 6 && computed.settings.gap_outer == 6,
                "Lua evaluation failed");
        rejects("return {layout={gap=-1}}");
        auto gaps = shaode::parse_config("return {layout={gap=4,gap_outer=10}}");
        require(gaps.settings.gap_inner == 4 && gaps.settings.gap_outer == 10,
                "gap_inner/gap_outer not parsed");
        rejects("return {layout={gap_inner=101}}");
        auto windows = shaode::parse_config(
            "return {windows={border_width=2,border_color='#ff000080',"
            "border_inactive_color='#00ff00',opacity=0.95,inactive_opacity=0.8,"
            "rules={{app_id='^firefox$',opacity=0.9},{app_id='code',opacity=0.7,"
            "inactive_opacity=0.6}}}}");
        const auto &ws = windows.settings;
        require(ws.border_width == 2 && ws.border_active[0] > 0.50F &&
                    ws.border_active[0] < 0.51F && ws.border_active[3] > 0.50F &&
                    ws.border_active[3] < 0.51F && ws.border_inactive[1] == 1.0F &&
                    ws.border_inactive[3] == 1.0F,
                "border settings not parsed or not premultiplied");
        require(windows.window_opacity("firefox", true) == 0.9F &&
                    windows.window_opacity("firefox", false) == 0.9F &&
                    windows.window_opacity("code-oss", false) == 0.6F &&
                    windows.window_opacity("firefox-esr", true) == 0.95F &&
                    windows.window_opacity("", false) == 0.8F,
                "window opacity rules mismatch");
        auto opaque = shaode::parse_config("return {}");
        require(opaque.window_opacity("x", false) == 1 && opaque.settings.border_width == 0,
                "window defaults changed");
        rejects("return {windows={border_width=21}}");
        auto input = shaode::parse_config(
            "return {mouse={speed=-0.5,acceleration='flat',natural_scroll=false},"
            "touchpad={natural_scroll=true,tap_to_click=true,disable_while_typing=false}}");
        const auto &is = input.settings;
        require(is.pointer_speed_set && is.pointer_speed == -0.5 && is.pointer_accel == 0 &&
                    is.mouse_natural_scroll == 0 && is.touchpad_natural_scroll == 1 &&
                    is.touchpad_tap == 1 && is.touchpad_dwt == 0 && is.mouse_modifier == SH_ALT,
                "pointer settings not parsed");
        const auto &defaults = opaque.settings;
        require(!defaults.pointer_speed_set && defaults.pointer_accel == -1 &&
                    defaults.mouse_natural_scroll == -1 && defaults.touchpad_tap == -1,
                "pointer defaults must leave devices alone");
        rejects("return {mouse={speed=2}}");
        rejects("return {mouse={acceleration='fast'}}");
        rejects("return {touchpad={tap_to_click=1}}");
        rejects("return {touchpad={scroll_factor=2}}");
        rejects("return {windows={border_color='red'}}");
        rejects("return {windows={opacity=0}}");
        rejects("return {windows={opacity=1.5}}");
        rejects("return {windows={rules={{app_id='('}}}}");
        rejects("return {windows={rules={{opacity=0.5}}}}");
        rejects("return {windows={rules={{app_id='x',class='y'}}}}");
        rejects("return {keyboard={repeat_rate='25'}}");
        rejects("return {appearance={background='#oops00'}}");
        rejects("return {layuot={gap=2}}");
        rejects("return {version=2}");
        rejects("return {shell={enabled='yes'}}");
        rejects("return {shell={panel_height=0}}");
        auto bar = shaode::parse_config(
            "return {shell={panel_position='top',panel_margin={top=6,left=10,right=10},"
            "panel_radius=12,font='JetBrainsMono Nerd Font',font_size=13,"
            "panel_color='#151e2ccc'}}");
        require(bar.shell.panel_top && bar.shell.panel_margin[0] == 6 &&
                    bar.shell.panel_margin[1] == 10 && bar.shell.panel_margin[2] == 0 &&
                    bar.shell.panel_margin[3] == 10 && bar.shell.panel_radius == 12 &&
                    bar.shell.font == "JetBrainsMono Nerd Font" && bar.shell.font_size == 13 &&
                    bar.shell.panel_color == "#151e2ccc",
                "bar settings not parsed");
        auto even = shaode::parse_config("return {shell={panel_margin=8}}");
        require(even.shell.panel_margin[0] == 8 && even.shell.panel_margin[3] == 8 &&
                    !even.shell.panel_top,
                "single panel margin not parsed");
        rejects("return {shell={panel_position='left'}}");
        rejects("return {shell={panel_margin=-1}}");
        rejects("return {shell={panel_margin={middle=1}}}");
        rejects("return {shell={panel_radius=51}}");
        rejects("return {shell={font_size=2}}");
        rejects("return {shell={accent='#12345'}}");
        rejects("return {appearance={background='#11223344'}}");
        rejects("return {shell={accent='red'}}");
        rejects("return {shell={launchers={{name='',command={'kitty'}}}}}");
        rejects("return {shell={launchers={{name='Terminal',command='kitty'}}}}");
        rejects("return true");
        rejects("return {startup={{}}}");
        rejects("return {startup={{'kitty', [3]='bad'}}}");
        rejects("return {startup={{'kitty\\0bad'}}}");
        rejects("return {bindings={{mods={'Hyper'}, key='a', action='quit'}}}");
        rejects("return {bindings={{mods={}, key='NotAKey', action='quit'}}}");
        rejects("return {bindings={{mods={}, key='a', action='unknown'}}}");
        rejects("local b={mods={'Alt'},key='a',action='quit'}; return {bindings={b,b}}");
        rejects("while true do end");
        rejects("os.execute('false')");
        // Failed reload leaves the previously active value intact.
        try {
            config = shaode::parse_config("return {layout={gap=999}}");
        } catch (const std::exception &) {
        }
        require(config.settings.gap_inner == 8 && config.bindings.size() == 24,
                "failed reload changed active configuration");
        std::cout << "Configuration validation, bindings, and transactional loading passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
