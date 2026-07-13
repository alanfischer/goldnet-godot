#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "goldnet_link.h"
#include "goldnet_multiplayer.h"

using namespace godot;

void initialize_goldnet_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	ClassDB::register_class<GoldNetLink>();
	ClassDB::register_class<GoldNetMultiplayer>();

	// godot-cpp only pre-registers instance-binding callbacks for classes it explicitly touches.
	// Marshaling an object whose nearest native class is bare Node/Object — e.g. an @rpc on a
	// GDScript autoload that extends Node directly (NetworkManager, SoundSystem) into our
	// _rpc(Object*) override — makes ClassDB::get_instance_binding_callbacks() walk to the root and
	// emit "Cannot find instance binding callbacks for class 'Node'". object.cpp already recovers
	// with the Object fallback, so this lookup is only log noise; seed Node/Object with those same
	// fallback callbacks up front so the lookup hits and the recovery is silent. (Subclass RPCs —
	// Node3D & co. — resolve fine and are untouched.)
	internal::register_engine_class(Node::get_class_static(), &Object::_gde_binding_callbacks);
	internal::register_engine_class(Object::get_class_static(), &Object::_gde_binding_callbacks);
}

void uninitialize_goldnet_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT goldnet_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_goldnet_module);
	init_obj.register_terminator(uninitialize_goldnet_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
