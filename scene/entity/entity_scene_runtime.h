#pragma once

#include "entity_world.h"

#include "core/input/input_event.h"
#include "core/os/main_loop.h"
#include "scene/resources/texture.h"

class EntitySceneRuntime : public MainLoop {
	GDCLASS(EntitySceneRuntime, MainLoop);

	EntityCatalog catalog;
	EntityWorld *world = nullptr;
	RID viewport;
	Ref<Texture2D> vrs_texture;
	Vector<Ref<InputEvent>> input_events;
	bool callbacks_connected = false;
	bool quit_requested = false;
	bool interpolation_enabled = false;

	void _window_resized();
	void _window_event(int p_event);
	void _input_event(const Ref<InputEvent> &p_event);
	void _release();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	Error setup();
	EntityWorld *get_world() { return world; }
	EntityCatalog &get_catalog() { return catalog; }
	RID get_viewport() const { return viewport; }
	Vector<Ref<InputEvent>> take_input_events();
	void quit(int p_exit_code = 0);
	void initialize() override;
	void iteration_prepare() override;
	bool physics_process(double p_time) override;
	void iteration_end() override;
	bool process(double p_time) override;
	void finalize() override;
	~EntitySceneRuntime() override;
};
