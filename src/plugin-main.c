/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-source.h>
#include <obs.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include "pipewire.h"
#include <spa/utils/dict.h>

#if !PW_CHECK_VERSION(1, 2, 7)
#define PW_KEY_NODE_SUPPORTS_REQUEST        "node.supports-request"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Generic PipeWire video source";
}

struct _pipewire_source {
	uint32_t id;
	struct dstr name;
};

struct pipewire_video_capture {
	obs_source_t *source;
	obs_data_t *settings;

	uint32_t pipewire_node;
	bool double_buffering;
	DARRAY(uint32_t) video_nodes;
	DARRAY(struct _pipewire_source) available_sources;

	obs_pipewire *obs_pw;
	obs_pipewire_stream *obs_pw_stream;
};

static const char *pipewire_video_capture_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireVideoSource");
}

void pipewire_video_rename(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(data);
	struct pipewire_video_capture *capture = param;

	if (!capture || !capture->obs_pw_stream)
		return;

	obs_pipewire_stream_set_name(capture->obs_pw_stream, calldata_string(data, "new_name"));
}

static void obs_pipewire_registry_event_global(void *data, uint32_t id, uint32_t permissions,
					       const char *type, uint32_t version,
					       const struct spa_dict *props)
{
	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);

	struct pipewire_video_capture *capture = data;

	if (!capture || !props) {
		return;
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
		if (!media_class) {
			return;
		}
		if (strcmp(media_class, "Stream/Output/Video") != 0) {
			return;
		}
		da_push_back(capture->video_nodes, &id);
	} else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
		if (strcmp(direction, "out") != 0) {
			return;
		}

		const char *node_id_str = spa_dict_lookup(props, PW_KEY_NODE_ID);
		if (!node_id_str) {
			return;
		}
		uint32_t node_id = strtoul(node_id_str, NULL, 10);
		if (da_find(capture->video_nodes, &node_id, 0) == DARRAY_INVALID) {
			return;
		}

		struct _pipewire_source source = {
			.id = id,
		};
		dstr_init_copy(&source.name, spa_dict_lookup(props, PW_KEY_PORT_ALIAS));
		da_push_back(capture->available_sources, &source);
	}
}

static void obs_pipewire_registry_event_global_remove(void *data, uint32_t id)
{
	struct pipewire_video_capture *capture = data;
	if (!capture) {
		return;
	}

	da_erase_item(capture->video_nodes, &id);

	for (size_t i = 0; i < capture->available_sources.num; ++i) {
		struct _pipewire_source source = capture->available_sources.array[i];
		if (source.id == id) {
			dstr_free(&source.name);
			da_erase(capture->available_sources, i);
			break;
		}
	}
}

static const struct pw_registry_events registry_events = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = obs_pipewire_registry_event_global,
	.global_remove = obs_pipewire_registry_event_global_remove,
};

static void *pipewire_video_capture_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct pipewire_video_capture *capture;
	struct obs_pipwire_connect_stream_info connect_info;

	capture = bzalloc(sizeof(struct pipewire_video_capture));
	capture->source = source;
	capture->double_buffering = obs_data_get_bool(settings, "DoubleBuffering");
	da_init(capture->video_nodes);
	da_init(capture->available_sources);

	capture->obs_pw = obs_pipewire_connect(&registry_events, capture);
	if (!capture->obs_pw) {
		bfree(capture);
		return NULL;
	}

	const char *uuid = obs_source_get_uuid(source);
	blog(LOG_INFO, "uuid: %s\n", uuid);
	struct dstr node_name = {0};
	dstr_printf(&node_name, "obs_pwvideo.%s", uuid);

	connect_info = (struct obs_pipwire_connect_stream_info){
		.stream_name = obs_source_get_name(source),
		// clang-format off
		.stream_properties = pw_properties_new(
			PW_KEY_NODE_NAME, node_name.array,
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Generic",
			PW_KEY_NODE_SUPPORTS_REQUEST, "1",
			NULL
		),
		// clang-format on
		.screencast =
			{
				.cursor_visible = false,
			},
		.double_buffering = capture->double_buffering,
	};

	dstr_free(&node_name);

	capture->obs_pw_stream =
		obs_pipewire_connect_stream(capture->obs_pw, capture->source, SPA_ID_INVALID, &connect_info);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "rename", pipewire_video_rename, capture);

	return capture;
}

static void pipewire_video_capture_destroy(void *data)
{
	struct pipewire_video_capture *capture = data;

	if (!capture)
		return;

	signal_handler_t *sh = obs_source_get_signal_handler(capture->source);
	signal_handler_disconnect(sh, "rename", pipewire_video_rename, capture);

	da_free(capture->video_nodes);
	for (size_t i = 0; i < capture->available_sources.num; ++i) {
		struct _pipewire_source source = capture->available_sources.array[i];
		dstr_free(&source.name);
	}
	da_free(capture->available_sources);

	if (capture->obs_pw_stream) {
		obs_pipewire_stream_destroy(capture->obs_pw_stream);
		capture->obs_pw_stream = NULL;
	}

	obs_pipewire_destroy(capture->obs_pw);
	bfree(capture);
}

static void pipewire_video_capture_get_defaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
}

static obs_properties_t *pipewire_video_capture_get_properties(void *data)
{
	obs_properties_t *properties;

	properties = obs_properties_create();

	obs_property_t *sources_list = obs_properties_add_list(properties, "CaptureSource", obs_module_text("Capture Source"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	struct pipewire_video_capture *capture = data;
	if (capture) {
		for (size_t i = 0; i < capture->available_sources.num; ++i) {
			struct _pipewire_source source = capture->available_sources.array[i];
			struct dstr name;
			dstr_init(&name);
			dstr_printf(&name, "%s (id: %u)", source.name.array, source.id);
			obs_property_list_add_int(sources_list, name.array, source.id);
			dstr_free(&name);
		}
	}

	obs_properties_add_bool(properties, "DoubleBuffering", obs_module_text("DoubleBuffering"));

	return properties;
}

static void pipewire_video_capture_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct pipewire_video_capture *capture = data;

	capture->double_buffering = obs_data_get_bool(settings, "DoubleBuffering");

	if (capture->obs_pw_stream)
		obs_pipewire_stream_set_double_buffering(capture->obs_pw_stream, capture->double_buffering);
}

static void pipewire_video_capture_show(void *data)
{
	struct pipewire_video_capture *capture = data;

	if (capture->obs_pw_stream)
		obs_pipewire_stream_show(capture->obs_pw_stream);
}

static void pipewire_video_capture_hide(void *data)
{
	struct pipewire_video_capture *capture = data;

	if (capture->obs_pw_stream)
		obs_pipewire_stream_hide(capture->obs_pw_stream);
}

static uint32_t pipewire_video_capture_get_width(void *data)
{
	struct pipewire_video_capture *capture = data;

	if (capture->obs_pw_stream)
		return obs_pipewire_stream_get_width(capture->obs_pw_stream);
	else
		return 0;
}

static uint32_t pipewire_video_capture_get_height(void *data)
{
	struct pipewire_video_capture *capture = data;

	if (capture->obs_pw_stream)
		return obs_pipewire_stream_get_height(capture->obs_pw_stream);
	else
		return 0;
}

static void pipewire_video_capture_video_render(void *data, gs_effect_t *effect)
{
	struct pipewire_video_capture *capture = data;

	if (capture->obs_pw_stream)
		obs_pipewire_stream_video_render(capture->obs_pw_stream, effect);
}

static void pipewire_video_capture_video_tick(void *data, float seconds)
{
	struct pipewire_video_capture *capture = data;

	if (capture->obs_pw_stream)
		obs_pipewire_stream_video_tick(capture->obs_pw_stream, seconds);
}

void pipewire_video_load(void)
{
	// Desktop capture
	const struct obs_source_info pipewire_video_capture_info = {
		.id = "pipewire-video-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = pipewire_video_capture_get_name,
		.create = pipewire_video_capture_create,
		.destroy = pipewire_video_capture_destroy,
		.get_defaults = pipewire_video_capture_get_defaults,
		.get_properties = pipewire_video_capture_get_properties,
		.update = pipewire_video_capture_update,
		.show = pipewire_video_capture_show,
		.hide = pipewire_video_capture_hide,
		.get_width = pipewire_video_capture_get_width,
		.get_height = pipewire_video_capture_get_height,
		.video_render = pipewire_video_capture_video_render,
		.video_tick = pipewire_video_capture_video_tick,
		.icon_type = OBS_ICON_TYPE_MEDIA,
	};
	obs_register_source(&pipewire_video_capture_info);
}

bool obs_module_load(void)
{
	pw_init(NULL, NULL);

	pipewire_video_load();

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
#if PW_CHECK_VERSION(0, 3, 49)
	pw_deinit();
#endif

	obs_log(LOG_INFO, "plugin unloaded");
}
