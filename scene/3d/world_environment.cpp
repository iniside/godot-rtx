/**************************************************************************/
/*  world_environment.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "world_environment.h"

#include "core/object/class_db.h"

void WorldEnvironment::set_environment(const Ref<Environment> &p_environment) {
	environment = p_environment;
}

Ref<Environment> WorldEnvironment::get_environment() const {
	return environment;
}

void WorldEnvironment::set_camera_attributes(const Ref<CameraAttributes> &p_camera_attributes) {
	camera_attributes = p_camera_attributes;
}

Ref<CameraAttributes> WorldEnvironment::get_camera_attributes() const {
	return camera_attributes;
}

void WorldEnvironment::set_compositor(const Ref<Compositor> &p_compositor) {
	compositor = p_compositor;
}

Ref<Compositor> WorldEnvironment::get_compositor() const {
	return compositor;
}

void WorldEnvironment::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment", "env"), &WorldEnvironment::set_environment);
	ClassDB::bind_method(D_METHOD("get_environment"), &WorldEnvironment::get_environment);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment", PROPERTY_HINT_RESOURCE_TYPE, Environment::get_class_static()), "set_environment", "get_environment");

	ClassDB::bind_method(D_METHOD("set_camera_attributes", "camera_attributes"), &WorldEnvironment::set_camera_attributes);
	ClassDB::bind_method(D_METHOD("get_camera_attributes"), &WorldEnvironment::get_camera_attributes);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "camera_attributes", PROPERTY_HINT_RESOURCE_TYPE, "CameraAttributesPractical,CameraAttributesPhysical"), "set_camera_attributes", "get_camera_attributes");

	ClassDB::bind_method(D_METHOD("set_compositor", "compositor"), &WorldEnvironment::set_compositor);
	ClassDB::bind_method(D_METHOD("get_compositor"), &WorldEnvironment::get_compositor);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "compositor", PROPERTY_HINT_RESOURCE_TYPE, Compositor::get_class_static()), "set_compositor", "get_compositor");
}

WorldEnvironment::WorldEnvironment() {
}
