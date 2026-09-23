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
        require(config.bindings.size() == 10, "example shortcuts missing");
        auto *spawn = config.binding(SH_ALT, XKB_KEY_Return);
        require(spawn && spawn->command == shaode::Command{"kitty"}, "spawn argv mismatch");
        require(config.binding(SH_ALT | SH_SHIFT | 2, XKB_KEY_R)->action == SH_RELOAD,
                "shifted shortcut or CapsLock normalization failed");
        require(!config.binding(SH_ALT | SH_CTRL, XKB_KEY_Return), "extra modifiers matched");
        auto computed = shaode::parse_config("local gap = 3; return {layout={gap=gap*2}}");
        require(computed.settings.gap == 6, "Lua evaluation failed");
        rejects("return {layout={gap=-1}}");
        rejects("return {keyboard={repeat_rate='25'}}");
        rejects("return {appearance={background='#oops00'}}");
        rejects("return {layuot={gap=2}}");
        rejects("return {version=2}");
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
        require(config.settings.gap == 8 && config.bindings.size() == 10,
                "failed reload changed active configuration");
        std::cout << "Configuration validation, bindings, and transactional loading passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
