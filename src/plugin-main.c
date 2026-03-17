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
#include <obs-properties.h>
#include <obs-source.h>
#include <obs.h>
#include <plugin-support.h>
#include <spa/param/format.h>
#include <stdint.h>
#include <util/base.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <pthread.h>
#include "pipewire.h"

#if !PW_CHECK_VERSION(1, 2, 7)
#define PW_KEY_NODE_SUPPORTS_REQUEST        "node.supports-request"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Generic PipeWire video source";
}

struct pipewire_target {
	uint32_t id;
	uint64_t serial;
	char *node_name;
	char *friendly_name;
	bool unique;
};

struct pipewire_video_capture {
	obs_source_t *source;
	obs_data_t *settings;

	uint32_t pipewire_node;
	bool double_buffering;

	obs_pipewire *obs_pw;
	obs_pipewire_stream *obs_pw_stream;

	DARRAY(struct pipewire_target) targets;
	ssize_t cur_target;
	bool cur_unique;
	pthread_mutex_t targets_lock;
};

static void free_target(struct pipewire_target *tgt)
{
	if (tgt->friendly_name) {
		bfree(tgt->friendly_name);
		tgt->friendly_name = NULL;
	}
	if (tgt->node_name) {
		bfree(tgt->node_name);
		tgt->node_name = NULL;
	}
}

// Call with targets lock held
void update_pipewire_target(struct pipewire_video_capture *capture)
{
	if (!capture->obs_pw_stream)
		return;

	if (capture->cur_target < 0) {
		obs_pipewire_stream_set_target(capture->obs_pw_stream, NULL);
		return;
	}

	struct pipewire_target *tgt = &capture->targets.array[capture->cur_target];

	if (tgt->unique) {
		capture->cur_unique = true;
		blog(LOG_INFO, "[pwvideo] Connect to unique target %s (%d/%" PRId64 ")", tgt->node_name, tgt->id,
		     tgt->serial);
		obs_pipewire_stream_set_target(capture->obs_pw_stream, tgt->node_name);
	} else {
		char buf[32];

		// Target should be the serial (with no prefix) if there are dupes
		sprintf(buf, "%" PRId64, tgt->serial);
		capture->cur_unique = false;
		blog(LOG_INFO, "[pwvideo] Conect to non-unique target %s (%d/%" PRId64 ")", tgt->node_name, tgt->id,
		     tgt->serial);
		obs_pipewire_stream_set_target(capture->obs_pw_stream, buf);
	}
}

static void on_registry_global_cb(void *user_data, uint32_t id, uint32_t permissions, const char *type,
				  uint32_t version, const struct spa_dict *props)
{
	struct pipewire_video_capture *capture = user_data;

	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);

	struct pipewire_target target = {.id = id, .unique = true};
	const char *node_name;
	const char *friendly_name;
	const char *serial_str;
	const char *media_role;
	const char *media_class;
	const char *media_type;

	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	media_type = spa_dict_lookup(props, PW_KEY_MEDIA_TYPE);
	media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	media_role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE);

	/*
	 * We enumerate targets for obs-pwvideo by requiring:
	 *
	 * media.type == "Video"
	 * media.class == "Stream/Output/Video"
	 * media.role == "Production" or unset (backwards compat)
	 *
	 * This filters out physical devices.
	 * If the media.type requirement is relaxed, this would pick
	 * up stuff like KWin/mutter screencasts. However, this is
	 * mostly useful only for testing right now.
	 */

	if ((!media_type || strcmp(media_type, "Video")) ||
	    (!media_class || strcmp(media_class, "Stream/Output/Video")) ||
	    (media_role && strcmp(media_role, "Production")))
		return;

	serial_str = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
	if (!serial_str) {
		blog(LOG_WARNING, "[pwvideo] Node %d does not have a node.serial, ignoring", id);
		return;
	}
	target.serial = atoll(serial_str);

	node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (!node_name) {
		blog(LOG_WARNING, "[pwvideo] Node %d does not have a node.name, ignoring", id);
		return;
	}

	friendly_name = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
	if (!friendly_name)
		friendly_name = spa_dict_lookup(props, PW_KEY_NODE_NICK);
	if (!friendly_name)
		friendly_name = node_name;

	target.node_name = bstrdup(node_name);
	target.friendly_name = bstrdup(friendly_name);

	blog(LOG_INFO, "[pwvideo] Found new target %s [%s], id %d, serial %" PRId64, node_name, friendly_name,
	     target.id, target.serial);

	pthread_mutex_lock(&capture->targets_lock);

	for (size_t i = 0; i < capture->targets.num; i++) {
		struct pipewire_target *tgt = &capture->targets.array[i];
		if (tgt->id == PW_ID_ANY && tgt->unique && !strcmp(tgt->node_name, target.node_name)) {
			// Source came back, restore it in-place
			free_target(tgt);
			*tgt = target;
			goto ret;
		} else if (!strcmp(tgt->node_name, node_name)) {
			blog(LOG_INFO, "[pwvideo] New target name %s is not unique (id %d, serial %" PRId64 " matches)",
			     node_name, tgt->id, tgt->serial);
			tgt->unique = false;
			target.unique = false;
		}
	}

	da_push_back(capture->targets, &target);

ret:
	pthread_mutex_unlock(&capture->targets_lock);

	obs_source_update_properties(capture->source);
}

static void on_registry_global_remove_cb(void *user_data, uint32_t id)
{
	struct pipewire_video_capture *capture = user_data;
	struct pipewire_target *tgt = NULL;
	size_t idx;

	pthread_mutex_lock(&capture->targets_lock);

	for (idx = 0; idx < capture->targets.num; idx++) {
		tgt = &capture->targets.array[idx];

		if (tgt->id == id && tgt->id != PW_ID_ANY)
			break;
	}

	if (idx >= capture->targets.num) {
		pthread_mutex_unlock(&capture->targets_lock);
		return;
	}

	blog(LOG_INFO, "[pwvideo] Removing target %s [%s], id %d, serial %" PRId64, tgt->node_name, tgt->friendly_name,
	     tgt->id, tgt->serial);

	if (!tgt->unique) {
		ssize_t dupe_tgt = -1;
		for (size_t i = 0; i < capture->targets.num; i++) {
			struct pipewire_target *p = &capture->targets.array[i];
			if (tgt != p && !strcmp(tgt->node_name, p->node_name)) {
				if (dupe_tgt < 0) {
					dupe_tgt = (ssize_t)i;
				} else {
					dupe_tgt = -1;
					break;
				}
			}
		}
		if (dupe_tgt >= 0) {
			struct pipewire_target *p = &capture->targets.array[dupe_tgt];
			blog(LOG_INFO, "[pwvideo] Target name %s is now unique (%d/%" PRId64 ")", p->node_name, p->id,
			     p->serial);
			p->unique = true;
			// If the current target is going away, and was duplicate, *and* was targeted as unique,
			// then retarget to the new remaining unique. This follows what WirePlumber will do.
			if (capture->cur_target == (ssize_t)idx && capture->cur_unique) {
				capture->cur_target = dupe_tgt;
			}
		}
	}

	if (capture->cur_target == (ssize_t)idx && tgt->unique) {
		blog(LOG_INFO, "[pwvideo] Current target %s went away (%d/%" PRId64 ")", tgt->node_name, tgt->id,
		     tgt->serial);
		tgt->id = PW_ID_ANY;
	} else {
		if (capture->cur_target == (ssize_t)idx)
			capture->cur_target = -1;
		else if (capture->cur_target >= 0 && capture->cur_target > (ssize_t)idx)
			--capture->cur_target;
		free_target(tgt);
		da_erase(capture->targets, idx);
	}

	pthread_mutex_unlock(&capture->targets_lock);

	obs_source_update_properties(capture->source);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_registry_global_cb,
	.global_remove = on_registry_global_remove_cb,
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

static void *pipewire_video_capture_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct pipewire_video_capture *capture;
	struct obs_pipwire_connect_stream_info connect_info;

	capture = bzalloc(sizeof(struct pipewire_video_capture));
	capture->source = source;
	capture->double_buffering = obs_data_get_bool(settings, "DoubleBuffering");

	capture->obs_pw = obs_pipewire_connect(&registry_events, capture);
	if (!capture->obs_pw) {
		bfree(capture);
		return NULL;
	}

	const char *uuid = obs_source_get_uuid(source);
	blog(LOG_INFO, "[pwvideo] uuid: %s\n", uuid);
	struct dstr node_name = {0};
	dstr_printf(&node_name, "obs_pwvideo.%s", uuid);

	connect_info = (struct obs_pipwire_connect_stream_info){
		.stream_name = obs_source_get_name(source),
		// clang-format off
		.stream_properties = pw_properties_new(
			PW_KEY_NODE_NAME, node_name.array,
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Production",
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

	const char *target = obs_data_get_string(settings, "target");

	da_init(capture->targets);
	if (target && target[0] && target[0] != '#') {
		connect_info.stream_target = target;

		struct pipewire_target *tgt = da_push_back_new(capture->targets);
		memset(tgt, 0, sizeof(*tgt));
		tgt->friendly_name = bstrdup(target);
		tgt->node_name = bstrdup(target);
		tgt->unique = true;
		tgt->id = PW_ID_ANY;
		capture->cur_target = 0;
		capture->cur_unique = true;
	} else {
		obs_data_set_string(settings, "target", NULL);
		capture->cur_target = -1;
	}

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

	if (capture->obs_pw_stream) {
		obs_pipewire_stream_destroy(capture->obs_pw_stream);
		capture->obs_pw_stream = NULL;
	}

	obs_pipewire_destroy(capture->obs_pw);

	for (size_t i = 0; i < capture->targets.num; i++) {
		struct pipewire_target *tgt = &capture->targets.array[i];
		free_target(tgt);
	}
	da_free(capture->targets);

	bfree(capture);
}

static void pipewire_video_capture_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "target", NULL);
}

static ssize_t find_target(struct pipewire_video_capture *capture, const char *target_name)
{
	if (!target_name || !target_name[0])
		return -1;

	if (target_name[0] == '#') {
		uint64_t serial = atoll(&target_name[1]);
		for (size_t i = 0; i < capture->targets.num; i++) {
			struct pipewire_target *tgt = &capture->targets.array[i];
			if (tgt->serial == serial)
				return (ssize_t)i;
		}
	} else {
		for (size_t i = 0; i < capture->targets.num; i++) {
			struct pipewire_target *tgt = &capture->targets.array[i];
			if (!strcmp(tgt->node_name, target_name))
				return (ssize_t)i;
		}
	}
	return -1;
}

static void populate_target_list(struct pipewire_video_capture *capture, obs_property_t *list)
{
	obs_property_list_clear(list);

	obs_property_list_add_string(list, "(No autoconnect)", "");

	// If the current target is gone, update the setting
	if (capture->cur_target == -1)
		obs_data_set_string(obs_source_get_settings(capture->source), "target", NULL);

	for (size_t i = 0; i < capture->targets.num; i++) {
		struct pipewire_target *tgt = &capture->targets.array[i];
		size_t prop_idx;
		if (tgt->unique) {
			blog(LOG_INFO, "[pwvideo] Add string %s %s", tgt->friendly_name, tgt->node_name);
			prop_idx = obs_property_list_add_string(list, tgt->friendly_name, tgt->node_name);

			/* If the props window is open, migrate to unique target */
			if (tgt->id != PW_ID_ANY && capture->cur_target == (ssize_t)i) {
				blog(LOG_INFO, "[pwvideo] Retarget to %s", tgt->node_name);
				obs_data_set_string(obs_source_get_settings(capture->source), "target", tgt->node_name);
			}
		} else {
			char *label, *serial;
			assert(asprintf(&label, "%s (%d)", tgt->friendly_name, tgt->id) > 0);
			assert(asprintf(&serial, "#%" PRId64, tgt->serial) > 0);
			blog(LOG_INFO, "[pwvideo] Add string %s %s", label, serial);
			prop_idx = obs_property_list_add_string(list, label, serial);

			/* If the props window is open, migrate to per-serial target or drop if it's gone */
			if (capture->cur_target == (ssize_t)i && tgt->id != PW_ID_ANY) {
				blog(LOG_INFO, "[pwvideo] Retarget to %s", serial);
				obs_data_set_string(obs_source_get_settings(capture->source), "target", serial);
			}
			free(label);
			free(serial);
		}
		if (tgt->id == PW_ID_ANY)
			obs_property_list_item_disable(list, prop_idx, true);
	}
}

static bool source_selected(void *data, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	bool refresh = false;
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct pipewire_video_capture *capture = data;
	const char *target_name;

	target_name = obs_data_get_string(settings, "target");

	pthread_mutex_lock(&capture->targets_lock);

	ssize_t idx = find_target(capture, target_name);
	if (idx < 0 && target_name && target_name[0]) {
		blog(LOG_WARNING, "[pwvideo] could not find target '%s'", target_name);
		pthread_mutex_unlock(&capture->targets_lock);
		return false;
	}

	if (idx == capture->cur_target) {
		pthread_mutex_unlock(&capture->targets_lock);
		return false;
	}

	blog(LOG_INFO, "[pwvideo] Target changed to: '%s'", target_name);

	if (capture->cur_target >= 0) {
		struct pipewire_target *prev_tgt = &capture->targets.array[capture->cur_target];

		if (prev_tgt->id == PW_ID_ANY) {
			blog(LOG_INFO, "[pwvideo] Clear out previous dead target");
			free_target(prev_tgt);
			da_erase(capture->targets, capture->cur_target);

			// Index might have changed
			if (idx >= 0) {
				idx = find_target(capture, target_name);
				assert(idx >= 0);
			}
			refresh = true;
		}
	}

	if (idx >= 0) {
		struct pipewire_target *tgt = &capture->targets.array[idx];

		blog(LOG_INFO, "[pwvideo] selected target '%s' (%d/%" PRId64 ")", tgt->node_name, tgt->id, tgt->serial);
	} else {
		blog(LOG_INFO, "[pwvideo] deselected target (no autoconnection)");
	}

	capture->cur_target = idx;

	update_pipewire_target(capture);
	populate_target_list(capture, property);
	pthread_mutex_unlock(&capture->targets_lock);

	return refresh;
}

static obs_properties_t *pipewire_video_capture_get_properties(void *data)
{
	struct pipewire_video_capture *capture = data;
	obs_properties_t *props;

	props = obs_properties_create();

	obs_property_t *source_list = obs_properties_add_list(props, "target", obs_module_text("Source"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	pthread_mutex_lock(&capture->targets_lock);
	populate_target_list(capture, source_list);
	pthread_mutex_unlock(&capture->targets_lock);

	obs_property_set_modified_callback2(source_list, source_selected, capture);

	obs_properties_add_bool(props, "DoubleBuffering", obs_module_text("DoubleBuffering"));

	blog(LOG_INFO, "[pwvideo] props %p", props);
	return props;
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
