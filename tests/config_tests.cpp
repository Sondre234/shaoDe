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
        require(config.bindings.size() == 21, "example shortcuts missing");
        require(config.shell.enabled && config.shell.panel_height == 52 &&
                    config.shell.launchers.size() == 2,
                "example shell settings missing");
        require(config.shell.launchers.front().command == shaode::Command{"kitty"},
                "pinned command arguments changed");
        auto *spawn = config.binding(SH_ALT, XKB_KEY_Return);
        require(spawn && spawn->command == shaode::Command{"kitty"}, "spawn argv mismatch");
        require(config.binding(SH_ALT | SH_SHIFT | 2, XKB_KEY_R)->action == SH_RELOAD,
                "shifted shortcut or CapsLock normalization failed");
        require(!config.binding(SH_ALT | SH_CTRL, XKB_KEY_Return), "extra modifiers matched");
        auto *fullscreen = config.binding(SH_ALT, XKB_KEY_F11);
        require(fullscreen && fullscreen->action == SH_FULLSCREEN, "fullscreen binding missing");
        auto *move = config.binding(SH_CTRL | SH_ALT | SH_SHIFT, XKB_KEY_3);
        require(move && move->action == SH_MOVE_TO_WORKSPACE && move->workspace == 3,
                "move-to-workspace binding missing");
        require(config.settings.workspaces == 4, "example workspace count changed");
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
        require(computed.settings.gap == 6, "Lua evaluation failed");
        rejects("return {layout={gap=-1}}");
        rejects("return {keyboard={repeat_rate='25'}}");
        rejects("return {appearance={background='#oops00'}}");
        rejects("return {layuot={gap=2}}");
        rejects("return {version=2}");
        rejects("return {shell={enabled='yes'}}");
        rejects("return {shell={panel_height=0}}");
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
        require(config.settings.gap == 8 && config.bindings.size() == 21,
                "failed reload changed active configuration");
        std::cout << "Configuration validation, bindings, and transactional loading passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
