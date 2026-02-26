#include <gdextension_interface.h>

#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/node.hpp>

#include <godot_cpp/core/object.hpp>

#include "godot/cambang_server.h"
#include "godot/cambang_server_tick_node.h"
#include "godot/cambang_state_snapshot.h"

#if defined(CAMBANG_ENABLE_DEV_NODES)
#include "godot/dev/cambang_dev_node.h"
#include "godot/dev/cambang_dev_frameview_node.h"
#endif

static cambang::CamBANGServer* g_server = nullptr;
static cambang::CamBANGServerTickNode* g_tick = nullptr;

static void cambang_gde_initialize(godot::ModuleInitializationLevel p_level) {
    if (p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    godot::ClassDB::register_class<cambang::CamBANGServer>();
    godot::ClassDB::register_class<cambang::CamBANGServerTickNode>();
    godot::ClassDB::register_class<cambang::CamBANGStateSnapshotGD>();

#if defined(CAMBANG_ENABLE_DEV_NODES)
    godot::ClassDB::register_class<cambang::CamBANGDevNode>();
    godot::ClassDB::register_class<cambang::CamBANGDevFrameViewNode>();
#endif

    // Create and register the Engine singleton.
    // Note: Engine singletons are not part of the scene tree; they do not receive _process.
    // We install an internal tick node under /root to drive main-thread polling/signal emission.
    g_server = memnew(cambang::CamBANGServer);
    godot::Engine::get_singleton()->register_singleton("CamBANGServer", g_server);

    // Install tick node.
    godot::MainLoop* ml = godot::Engine::get_singleton()->get_main_loop();
    godot::SceneTree* tree = godot::Object::cast_to<godot::SceneTree>(ml);
    if (tree) {
        godot::Window* root = tree->get_root();
        if (root) {
            g_tick = memnew(cambang::CamBANGServerTickNode);
            g_tick->set_name("__CamBANGServerTick");
            g_tick->set_server(g_server);
            root->add_child(g_tick);
        }
    }
}

static void cambang_gde_uninitialize(godot::ModuleInitializationLevel p_level) {
    if (p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    if (g_tick) {
        g_tick->set_server(nullptr);
        if (auto* parent = g_tick->get_parent()) {
            parent->remove_child(g_tick);
        }
        memdelete(g_tick);
        g_tick = nullptr;
    }

    if (g_server) {
        g_server->stop();
        godot::Engine::get_singleton()->unregister_singleton("CamBANGServer");
        memdelete(g_server);
        g_server = nullptr;
    }
}

extern "C" GDExtensionBool GDE_EXPORT cambang_gdextension_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization
) {
    godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

    init_obj.register_initializer(cambang_gde_initialize);
    init_obj.register_terminator(cambang_gde_uninitialize);
    init_obj.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);

    return init_obj.init();
}