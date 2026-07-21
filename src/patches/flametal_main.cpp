//
// Created by DiaLight on 9/8/2025.
//

#include "flametal_main.h"

#include <algorithm>

#include <iostream>
#include <patches/game_version_patch.h>
#include <patches/logging.h>
#include <patches/big_resolution_fix/screen_resolution.h>
#include <stdexcept>
#include <thread>
#include <tools/flametal_config.h>
#include "gog_patch.h"
#include "patches/inspect_tools.h"
#include "patches/micro_patches.h"
#include "patches/original_compatible.h"
#include "tools/bug_hunter.h"
#include "tools/command_line.h"
#include "tools/console.h"

#if __has_include(<dk2_research.h>)
#include "dk2_research.h"
#endif
#include "alternative_resources.h"
#include "patches/protocol_dump.h"
#ifdef FLAMETAL_WELCOME_WINDOW
#include "patches/welcome_window/welcome_window.h"
#endif
#include "patches/wine_support.h"
#include "dk2_functions.h"


namespace dk2 {
    bool installSpatialSamplerHotCallsite();
    bool installCameraPhaseProfiler();
    bool installFtolSse3();
    bool installRenderDispatchTargets();
}


flametal_config::define_flame_option<bool> o_console(
    "flametal:console", flametal_config::OG_Config,
    "Show console window to see logs\n",
    false
);
flametal_config::define_flame_option<bool> o_windowed(
    "flametal:windowed", flametal_config::OG_Config,
    "Open game in windowed mode\n",
    false
);
flametal_config::define_flame_option<bool> o_single_core(
    "flametal:single-core", flametal_config::OG_Config,
    "Limit game threading to one core\n"
    "This is what gog patches doing by default but in some cases they might be disabled\n"
    "Added this option as duplicate of gog:misc:SingleCore, but it will work in all cases\n"
    "",
    true
);
flametal_config::define_flame_option<bool> o_skip_launcher(
    "flametal:skip-launcher", flametal_config::OG_Config,
    "Do not show flametal launcher. Works only from command line\n"
    "DKII-DX.EXE -skip-launcher",
    false
);


void patch::flametalInit(int argc, const char **argv) {
    command_line_init(argc, argv);
    initConsole(false);
    bug_hunter::init();

    if (cmdl::hasFlag("h") || cmdl::hasFlag("help") || cmdl::hasFlag("-help")) {
        initConsole();
        flametal_config::help();
        std::cout << '\n' << "Press a key to continue...";
        std::cin.get();
        return;
    }
    if (cmdl::hasFlag("v") || cmdl::hasFlag("version") || cmdl::hasFlag("-version")) {
        initConsole();
        std::string version = "<unknown>";
        if(auto *ver = patch::game_version_patch::getFileVersion()) version = ver;
        std::replace(version.begin(), version.end(), '\n', ' ');
        std::cout << "DKII" << version << std::endl;
        std::cout << '\n' << "Press a key to continue...";
        std::cin.get();
        return;
    }

    {
        std::string config = "flametal/config.toml";
        auto it = cmdl::dict.find("c");
        if (it == cmdl::dict.end()) it = cmdl::dict.find("-config");
        if (it != cmdl::dict.end()) {
            if (!it->second.empty()) config = it->second;
        }
        flametal_config::load(config);
    }

    auto skipLauncher = flametal_config::get_cmdl_option(o_skip_launcher.path);
    if(skipLauncher.ty != flametal_config::VT_Boolean || !skipLauncher.bool_value) {
#ifdef FLAMETAL_WELCOME_WINDOW
        patch::welcome_window::welcome_data_t res;
        res.win32_class_name = L"Flametal_win32";
        res.win32_title = L"DungeonKeeper 2 Flametal";
        res.win32_size = {400, 600};
        patch::welcome_window::imgui_main(res);  // long blocking call
        if(!res.play) {
            flametalCleanup();
            ExitProcess(0);
        }
#endif  // FLAMETAL_WELCOME_WINDOW: without it we always behave as -skip-launcher
    }

    flametal_config::save();
    // in windowed mode we can attach console with flag
    if(o_console.get()) {
        initConsole();
    }

    if (*o_single_core) {
        HANDLE hProc = GetCurrentProcess();
        SetProcessAffinityMask(hProc, 1);
    }
    if(o_windowed.get()) {
        // The native Metal host needs GOG's exact D3D3 wrapper even though the
        // compatibility window remains windowed and will eventually be hidden.
        // Ordinary Flametal windowed runs retain the established behavior.
        if (GetEnvironmentVariableA("DK2_METAL_BRIDGE_FILE", nullptr, 0) == 0) {
            o_gog_enabled.set_tmp(false);  // gog is incompatible with windowed mode
        }
        patch::control_windowed_mode::enabled = true;
    }

    patch::alternative_resources::init();
    patch::wine_support::init();
    patch::inspect_tools::init();
    patch::multi_interface_fix::init();
    patch::original_compatible::init();
    patch::protocol_dump::init();
    patch::screen_resolution::init();
    if (GetEnvironmentVariableA("DK2_METAL_BRIDGE_FILE", nullptr, 0) != 0) {
        dk2::installRenderDispatchTargets();
        dk2::installSpatialSamplerHotCallsite();
        dk2::installCameraPhaseProfiler();
        dk2::installFtolSse3();
    }

#if __has_include(<dk2_research.h>)
    bug_hunter::init_keyWatcher();
#endif
    if(*o_gog_enabled) gog::patch_init();
}

void patch::flametalCleanup() {
#if __has_include(<dk2_research.h>)
    bug_hunter::stop_keyWatcher();
#endif
    if (flametal_config::changed()) flametal_config::save();
}

void patch::flametalStaticCleanup() {
    // all global dk2 objects destructors was called
    patch::log::dbg("flametal static cleanup");
    if (flametal_config::changed()) flametal_config::save();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch(fdwReason) {
    case DLL_PROCESS_ATTACH:
        break;
    case DLL_THREAD_ATTACH:
        // Do thread-specific initialization.
        break;
    case DLL_THREAD_DETACH:
        // Do thread-specific cleanup.
        break;
    case DLL_PROCESS_DETACH:
        if (lpvReserved != nullptr) {
            break; // do not do cleanup if process termination scenario
        }
        // Perform any necessary cleanup.
        break;
    }
    return TRUE;
}
