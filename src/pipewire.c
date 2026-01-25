/* pipewire.c
 *
 * Copyright 2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../src/pipewire.h"

#include "formats.h"

#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <util/base.h>
#include <util/darray.h>
#include <util/dstr.h>

#include <fcntl.h>
//#include <glad/glad.h>
#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>

#include <GL/gl.h>
#include <pthread.h>

//#define DEBUG_PIPEWIRE

#if !PW_CHECK_VERSION(0, 3, 62)
enum spa_meta_videotransform_value {
	SPA_META_TRANSFORMATION_None = 0,   /**< no transform */
	SPA_META_TRANSFORMATION_90,         /**< 90 degree counter-clockwise */
	SPA_META_TRANSFORMATION_180,        /**< 180 degree counter-clockwise */
	SPA_META_TRANSFORMATION_270,        /**< 270 degree counter-clockwise */
	SPA_META_TRANSFORMATION_Flipped,    /**< 180 degree flipped around the vertical axis. Equivalent
						  * to a reflexion through the vertical line splitting the
						  * bufffer in two equal sized parts */
	SPA_META_TRANSFORMATION_Flipped90,  /**< flip then rotate around 90 degree counter-clockwise */
	SPA_META_TRANSFORMATION_Flipped180, /**< flip then rotate around 180 degree counter-clockwise */
	SPA_META_TRANSFORMATION_Flipped270, /**< flip then rotate around 270 degree counter-clockwise */
};

#define SPA_META_VideoTransform 8

struct spa_meta_videotransform {
	uint32_t transform; /**< orientation transformation that was applied to the buffer */
};
#endif

#define CURSOR_META_SIZE(width, height) \
	(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + width * height * 4)

struct obs_pw_version {
	int major;
	int minor;
	int micro;
};

struct format_info {
	uint32_t spa_format;
	uint32_t drm_format;
	DARRAY(uint64_t) modifiers;
};

struct _obs_pipewire {
	int pipewire_fd;

	struct pw_thread_loop *thread_loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;
	int sync_id;

	struct obs_pw_version server_version;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list streams;
};

enum buffer_state {
	BUFFER_FREE = 0,
	BUFFER_FULL,
	BUFFER_PRESENTING,
	BUFFER_PRESENTED,
};

struct crop_info {
	bool valid;
	int x, y;
	uint32_t width, height;
};

struct present_buffer {
	enum buffer_state state;
	struct pw_buffer *buf;
	gs_texture_t *texture;

	enum spa_meta_videotransform_value transform;

	struct crop_info crop;

	struct {
		bool valid;
		int x, y;
		int hotspot_x, hotspot_y;
		int width, height;
		gs_texture_t *texture;
	} cursor;

	struct {
		int acquire_syncobj_fd;
		int release_syncobj_fd;
		uint64_t acquire_point;
		uint64_t release_point;
		bool set;
	} sync;
};

struct _obs_pipewire_stream {
	obs_pipewire *obs_pw;
	struct spa_list link;
	obs_source_t *source;
	char *name;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *reneg;
	struct spa_source *rename;

	struct spa_video_info format;

	struct present_buffer buffers[2];
	struct present_buffer presenting_buffer;
	bool new_frame;

	struct obs_video_info video_info;
	bool negotiated;

	DARRAY(struct format_info) format_info;

	bool active;
	bool double_buffering;
	bool cursor_visible;

	struct {
		struct spa_rectangle rect;
		bool set;
	} resolution;

	struct {
		struct spa_fraction fraction;
		bool set;
	} framerate;

	/* Cache of stream info for cross-thread use */
	pthread_mutex_t lock;
	struct crop_info crop;
	enum spa_meta_videotransform_value transform;
	uint32_t width, height;
};

/* auxiliary methods */

static gs_texture_t *pb_texture(struct present_buffer *pb)
{
	struct pw_buffer *b = pb->buf;

	if (!b)
		return NULL;

	if (b->user_data)
		return b->user_data;

	return pb->texture;
}

static bool parse_pw_version(struct obs_pw_version *dst, const char *version)
{
	int n_matches = sscanf(version, "%d.%d.%d", &dst->major, &dst->minor, &dst->micro);
	return n_matches == 3;
}

static bool check_pw_version(const struct obs_pw_version *pw_version, int major, int minor, int micro)
{
	if (pw_version->major != major)
		return pw_version->major > major;
	if (pw_version->minor != minor)
		return pw_version->minor > minor;
	return pw_version->micro >= micro;
}

static void update_pw_versions(obs_pipewire *obs_pw, const char *version)
{
	blog(LOG_INFO, "[pipewire] Server version: %s", version);
	blog(LOG_INFO, "[pipewire] Library version: %s", pw_get_library_version());
	blog(LOG_INFO, "[pipewire] Header version: %s", pw_get_headers_version());

	if (!parse_pw_version(&obs_pw->server_version, version))
		blog(LOG_WARNING, "[pipewire] failed to parse server version");
}

static void teardown_pipewire(obs_pipewire *obs_pw)
{
	if (obs_pw->thread_loop) {
		obs_pipewire_roundtrip(obs_pw);
		pw_thread_loop_stop(obs_pw->thread_loop);
	}

	if (obs_pw->registry) {
		pw_proxy_destroy((struct pw_proxy *)obs_pw->registry);
		obs_pw->registry = NULL;
	}

	if (obs_pw->context) {
		pw_context_destroy(obs_pw->context);
		obs_pw->context = NULL;
	}

	if (obs_pw->thread_loop) {
		pw_thread_loop_destroy(obs_pw->thread_loop);
		obs_pw->thread_loop = NULL;
	}

	if (obs_pw->pipewire_fd >= 0) {
		close(obs_pw->pipewire_fd);
		obs_pw->pipewire_fd = -1;
	}
}

static void pop_buffer(obs_pipewire_stream *obs_pw_stream, struct present_buffer *cur, struct present_buffer *next);
static void destroy_buffer(obs_pipewire_stream *obs_pw_stream, struct present_buffer *buf);

static struct present_buffer *get_buffer(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_in_task_thread(OBS_TASK_GRAPHICS))
		return NULL;

	if (obs_pw_stream->new_frame) {
		/* NB: This should be thread safe as far as I can tell */
		pw_stream_trigger_process(obs_pw_stream->stream);

		if (obs_pw_stream->buffers[0].state != BUFFER_FREE) {
			/* Note: gs_flush() must have been called at some point since last render,
			 * for implicit sync to work. We assume this happens in obs-video.c. */
			pop_buffer(obs_pw_stream, &obs_pw_stream->presenting_buffer, &obs_pw_stream->buffers[0]);
			obs_pw_stream->buffers[0] = obs_pw_stream->buffers[1];
			memset(&obs_pw_stream->buffers[1], 0, sizeof(struct present_buffer));
		}

		if (!obs_pw_stream->active) {
			destroy_buffer(obs_pw_stream, &obs_pw_stream->presenting_buffer);
		}
	}

	obs_pw_stream->new_frame = false;

	struct present_buffer *pb = &obs_pw_stream->presenting_buffer;

	if (pb->state == BUFFER_FREE)
		return NULL;

	if (pb->state == BUFFER_FULL)
		pb->state = BUFFER_PRESENTING;

	/* Cache some data for other threads to use */
	pthread_mutex_lock(&obs_pw_stream->lock);
	obs_pw_stream->crop = pb->crop;
	obs_pw_stream->width = gs_texture_get_width(pb_texture(pb));
	obs_pw_stream->height = gs_texture_get_height(pb_texture(pb));
	obs_pw_stream->transform = pb->transform;
	pthread_mutex_unlock(&obs_pw_stream->lock);

	return pb;
}

static inline bool has_effective_crop(obs_pipewire_stream *obs_pw_stream)
{
	/* Update only */
	get_buffer(obs_pw_stream);

	pthread_mutex_lock(&obs_pw_stream->lock);
	bool ret = obs_pw_stream->crop.valid && (obs_pw_stream->crop.x != 0 || obs_pw_stream->crop.y != 0 ||
						 obs_pw_stream->crop.width < obs_pw_stream->width ||
						 obs_pw_stream->crop.height < obs_pw_stream->height);
	pthread_mutex_unlock(&obs_pw_stream->lock);

	return ret;
}

static int get_buffer_flip(obs_pipewire_stream *obs_pw_stream)
{
	struct present_buffer *pb = get_buffer(obs_pw_stream);
	int flip = 0;

	if (!pb)
		return 0;

	switch (pb->transform) {
	case SPA_META_TRANSFORMATION_Flipped:
	case SPA_META_TRANSFORMATION_Flipped180:
		flip = GS_FLIP_U;
		break;
	case SPA_META_TRANSFORMATION_Flipped90:
	case SPA_META_TRANSFORMATION_Flipped270:
		flip = GS_FLIP_V;
		break;
	case SPA_META_TRANSFORMATION_None:
	case SPA_META_TRANSFORMATION_90:
	case SPA_META_TRANSFORMATION_180:
	case SPA_META_TRANSFORMATION_270:
		break;
	}

	return flip;
}

static bool push_rotation(obs_pipewire_stream *obs_pw_stream)
{
	struct present_buffer *pb = get_buffer(obs_pw_stream);

	if (!pb)
		return false;

	double offset_x = 0;
	double offset_y = 0;
	double rotation = 0;
	bool has_crop;

	has_crop = has_effective_crop(obs_pw_stream);

	switch (pb->transform) {
	case SPA_META_TRANSFORMATION_Flipped:
	case SPA_META_TRANSFORMATION_None:
		rotation = 0;
		break;
	case SPA_META_TRANSFORMATION_Flipped90:
	case SPA_META_TRANSFORMATION_90:
		rotation = 90;
		offset_x = 0;
		offset_y = has_crop ? pb->crop.height : obs_pw_stream->format.info.raw.size.height;
		break;
	case SPA_META_TRANSFORMATION_Flipped180:
	case SPA_META_TRANSFORMATION_180:
		rotation = 180;
		offset_x = has_crop ? pb->crop.width : obs_pw_stream->format.info.raw.size.width;
		offset_y = has_crop ? pb->crop.height : obs_pw_stream->format.info.raw.size.height;
		break;
	case SPA_META_TRANSFORMATION_Flipped270:
	case SPA_META_TRANSFORMATION_270:
		rotation = 270;
		offset_x = has_crop ? pb->crop.width : obs_pw_stream->format.info.raw.size.width;
		offset_y = 0;
		break;
	}

	if (rotation != 0) {
		gs_matrix_push();
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(rotation));
		gs_matrix_translate3f(-offset_x, -offset_y, 0.0f);
	}

	return rotation != 0;
}

static const uint32_t supported_formats_async[] = {
	SPA_VIDEO_FORMAT_RGBA,
	SPA_VIDEO_FORMAT_YUY2,
};

#define N_SUPPORTED_FORMATS_ASYNC (sizeof(supported_formats_async) / sizeof(supported_formats_async[0]))

static const uint32_t supported_formats_sync[] = {
	SPA_VIDEO_FORMAT_BGRA,       SPA_VIDEO_FORMAT_RGBA,       SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
#if PW_CHECK_VERSION(0, 3, 41)
	SPA_VIDEO_FORMAT_ABGR_210LE, SPA_VIDEO_FORMAT_xBGR_210LE,
#endif
};

#define N_SUPPORTED_FORMATS_SYNC (sizeof(supported_formats_sync) / sizeof(supported_formats_sync[0]))

static void swap_texture_red_blue(gs_texture_t *texture)
{
	GLuint gl_texure = *(GLuint *)gs_texture_get_obj(texture);

	glBindTexture(GL_TEXTURE_2D, gl_texure);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static inline struct spa_pod *build_format(obs_pipewire_stream *obs_pw_stream, struct spa_pod_builder *b,
					   uint32_t format, uint64_t *modifiers, size_t modifier_count)
{
	struct spa_rectangle max_resolution = SPA_RECTANGLE(8192, 4320);
	struct spa_rectangle min_resolution = SPA_RECTANGLE(1, 1);
	struct spa_rectangle resolution;
	struct spa_pod_frame format_frame;
	struct spa_fraction max_framerate;
	struct spa_fraction min_framerate;
	struct spa_fraction framerate;

	if (obs_pw_stream->framerate.set) {
		framerate = obs_pw_stream->framerate.fraction;
		min_framerate = obs_pw_stream->framerate.fraction;
		max_framerate = obs_pw_stream->framerate.fraction;
	} else {
		framerate = SPA_FRACTION(obs_pw_stream->video_info.fps_num, obs_pw_stream->video_info.fps_den);
		min_framerate = SPA_FRACTION(0, 1);
		max_framerate = SPA_FRACTION(360, 1);
	}

	if (obs_pw_stream->resolution.set) {
		resolution = obs_pw_stream->resolution.rect;
		min_resolution = obs_pw_stream->resolution.rect;
		max_resolution = obs_pw_stream->resolution.rect;
	} else {
		resolution =
			SPA_RECTANGLE(obs_pw_stream->video_info.output_width, obs_pw_stream->video_info.output_height);
		min_resolution = SPA_RECTANGLE(1, 1);
		max_resolution = SPA_RECTANGLE(8192, 4320);
	}

	/* Make an object of type SPA_TYPE_OBJECT_Format and id SPA_PARAM_EnumFormat.
	 * The object type is important because it defines the properties that are
	 * acceptable. The id gives more context about what the object is meant to
	 * contain. In this case we enumerate supported formats. */
	spa_pod_builder_push_object(b, &format_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	/* add media type and media subtype properties */
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);

	/* formats */
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);

	/* modifier */
	if (modifier_count > 0) {
		struct spa_pod_frame modifier_frame;

		/* build an enumeration of modifiers */
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
				     SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);

		spa_pod_builder_push_choice(b, &modifier_frame, SPA_CHOICE_Enum, 0);

		/* The first element of choice pods is the preferred value. Here
		 * we arbitrarily pick the first modifier as the preferred one.
		 */
		spa_pod_builder_long(b, modifiers[0]);

		/* modifiers from  an array */
		for (uint32_t i = 0; i < modifier_count; i++)
			spa_pod_builder_long(b, modifiers[i]);

		spa_pod_builder_pop(b, &modifier_frame);
	}
	/* add size and framerate ranges */
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
			    SPA_POD_CHOICE_RANGE_Rectangle(&resolution, &min_resolution, &max_resolution),
			    SPA_FORMAT_VIDEO_framerate,
			    SPA_POD_CHOICE_RANGE_Fraction(&framerate, &min_framerate, &max_framerate), 0);
	return spa_pod_builder_pop(b, &format_frame);
}

static bool build_format_params(obs_pipewire_stream *obs_pw_stream, struct spa_pod_builder *pod_builder,
				const struct spa_pod ***param_list, uint32_t *n_params)
{
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;
	uint32_t params_count = 0;

	const struct spa_pod **params;

	if (!obs_pw_stream->format_info.num) {
		blog(LOG_ERROR, "[pipewire] No format found while building param pointers");
		return false;
	}

	params = bzalloc(2 * obs_pw_stream->format_info.num * sizeof(struct spa_pod *));

	if (!params) {
		blog(LOG_ERROR, "[pipewire] Failed to allocate memory for param pointers");
		return false;
	}

	if (!check_pw_version(&obs_pw->server_version, 0, 3, 33))
		goto build_shm;

	for (size_t i = 0; i < obs_pw_stream->format_info.num; i++) {
		if (obs_pw_stream->format_info.array[i].modifiers.num == 0) {
			continue;
		}
		params[params_count++] = build_format(obs_pw_stream, pod_builder,
						      obs_pw_stream->format_info.array[i].spa_format,
						      obs_pw_stream->format_info.array[i].modifiers.array,
						      obs_pw_stream->format_info.array[i].modifiers.num);
	}

build_shm:
	for (size_t i = 0; i < obs_pw_stream->format_info.num; i++) {
		params[params_count++] = build_format(obs_pw_stream, pod_builder,
						      obs_pw_stream->format_info.array[i].spa_format, NULL, 0);
	}
	*param_list = params;
	*n_params = params_count;
	return true;
}

static bool drm_format_available(uint32_t drm_format, uint32_t *drm_formats, size_t n_drm_formats)
{
	for (size_t j = 0; j < n_drm_formats; j++) {
		if (drm_format == drm_formats[j]) {
			return true;
		}
	}
	return false;
}

static void init_format_info_async(obs_pipewire_stream *obs_pw_stream)
{
	da_init(obs_pw_stream->format_info);

	for (size_t i = 0; i < N_SUPPORTED_FORMATS_ASYNC; i++) {
		struct obs_pw_video_format obs_pw_video_format;
		struct format_info *info;
		if (!obs_pw_video_format_from_spa_format(supported_formats_async[i], &obs_pw_video_format))
			continue;

		info = da_push_back_new(obs_pw_stream->format_info);
		da_init(info->modifiers);
		info->spa_format = obs_pw_video_format.spa_format;
		info->drm_format = obs_pw_video_format.drm_format;
	}
}

static void init_format_info_sync(obs_pipewire_stream *obs_pw_stream)
{
	da_init(obs_pw_stream->format_info);

	obs_enter_graphics();

	enum gs_dmabuf_flags dmabuf_flags;
	uint32_t *drm_formats = NULL;
	size_t n_drm_formats = 0;

	bool capabilities_queried = gs_query_dmabuf_capabilities(&dmabuf_flags, &drm_formats, &n_drm_formats);

	for (size_t i = 0; i < N_SUPPORTED_FORMATS_SYNC; i++) {
		struct obs_pw_video_format obs_pw_video_format;
		struct format_info *info;
		if (!obs_pw_video_format_from_spa_format(supported_formats_sync[i], &obs_pw_video_format) ||
		    obs_pw_video_format.gs_format == GS_UNKNOWN)
			continue;

		info = da_push_back_new(obs_pw_stream->format_info);
		da_init(info->modifiers);
		info->spa_format = obs_pw_video_format.spa_format;
		info->drm_format = obs_pw_video_format.drm_format;

		if (!capabilities_queried ||
		    !drm_format_available(obs_pw_video_format.drm_format, drm_formats, n_drm_formats))
			continue;

		size_t n_modifiers;
		uint64_t *modifiers = NULL;
		if (gs_query_dmabuf_modifiers_for_format(obs_pw_video_format.drm_format, &modifiers, &n_modifiers)) {
			da_push_back_array(info->modifiers, modifiers, n_modifiers);
		}
		bfree(modifiers);

		if (dmabuf_flags & GS_DMABUF_FLAG_IMPLICIT_MODIFIERS_SUPPORTED) {
			uint64_t modifier_implicit = DRM_FORMAT_MOD_INVALID;
			da_push_back(info->modifiers, &modifier_implicit);
		}
	}
	obs_leave_graphics();

	bfree(drm_formats);
}

static void init_format_info(obs_pipewire_stream *obs_pw_stream)
{
	uint32_t output_flags;

	output_flags = obs_source_get_output_flags(obs_pw_stream->source);

	if (output_flags & OBS_SOURCE_VIDEO) {
		if (output_flags & OBS_SOURCE_ASYNC)
			init_format_info_async(obs_pw_stream);
		else
			init_format_info_sync(obs_pw_stream);
	}
}

static void clear_format_info(obs_pipewire_stream *obs_pw_stream)
{
	for (size_t i = 0; i < obs_pw_stream->format_info.num; i++) {
		da_free(obs_pw_stream->format_info.array[i].modifiers);
	}
	da_free(obs_pw_stream->format_info);
}

static void remove_modifier_from_format(obs_pipewire_stream *obs_pw_stream, uint32_t spa_format, uint64_t modifier)
{
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;

	for (size_t i = 0; i < obs_pw_stream->format_info.num; i++) {
		if (obs_pw_stream->format_info.array[i].spa_format != spa_format)
			continue;

		if (!check_pw_version(&obs_pw->server_version, 0, 3, 40)) {
			da_erase_range(obs_pw_stream->format_info.array[i].modifiers, 0,
				       obs_pw_stream->format_info.array[i].modifiers.num - 1);
			continue;
		}

		int idx = da_find(obs_pw_stream->format_info.array[i].modifiers, &modifier, 0);
		while (idx != -1) {
			da_erase(obs_pw_stream->format_info.array[i].modifiers, idx);
			idx = da_find(obs_pw_stream->format_info.array[i].modifiers, &modifier, 0);
		}
	}
}

static void renegotiate_format(void *data, uint64_t expirations)
{
	UNUSED_PARAMETER(expirations);
	obs_pipewire_stream *obs_pw_stream = (obs_pipewire_stream *)data;
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;
	const struct spa_pod **params = NULL;

	blog(LOG_INFO, "[pipewire] Renegotiating stream");

	pw_thread_loop_lock(obs_pw->thread_loop);

	uint8_t params_buffer[4096];
	struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	uint32_t n_params;
	if (!build_format_params(obs_pw_stream, &pod_builder, &params, &n_params)) {
		teardown_pipewire(obs_pw);
		pw_thread_loop_unlock(obs_pw->thread_loop);
		return;
	}

	pw_stream_update_params(obs_pw_stream->stream, params, n_params);
	pw_thread_loop_unlock(obs_pw->thread_loop);
	bfree(params);
}

static void sanitize_name(char *name)
{
	/* Replace dots, colons and spaces by underscores */
	char *aux = name;
	while ((aux = strchr(name, '.')) != NULL)
		*aux = '_';
	aux = name;
	while ((aux = strchr(name, ' ')) != NULL)
		*aux = '_';
	aux = name;
	while ((aux = strchr(name, ':')) != NULL)
		*aux = '_';
}

static void set_name_properties(struct pw_properties *props, const char *name)
{
	struct dstr media_name = {0};
	dstr_printf(&media_name, "%s Source", name);

	pw_properties_set(props, PW_KEY_NODE_NICK, name);
	pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, name);
	pw_properties_set(props, PW_KEY_MEDIA_NAME, media_name.array);

	dstr_free(&media_name);
}

static void rename_node(void *data, uint64_t expirations)
{
	UNUSED_PARAMETER(expirations);
	obs_pipewire_stream *obs_pw_stream = (obs_pipewire_stream *)data;
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;
	struct pw_properties *props;

	/* NB: NULL NULL to silence a spurious compiler warning */
	props = pw_properties_new(NULL, NULL);
	if (!props) {
		blog(LOG_ERROR, "Failed to alloc name properties");
		return;
	}

	pthread_mutex_lock(&obs_pw_stream->lock);
	set_name_properties(props, obs_pw_stream->name);
	pthread_mutex_unlock(&obs_pw_stream->lock);

	pw_thread_loop_lock(obs_pw->thread_loop);
	pw_stream_update_properties(obs_pw_stream->stream, &props->dict);
	pw_thread_loop_unlock(obs_pw->thread_loop);
	pw_properties_free(props);
}

/* ------------------------------------------------- */

static void return_unused_pw_buffer(struct pw_stream *stream, struct pw_buffer *b)
{
#if PW_CHECK_VERSION(1, 2, 0)
	struct spa_buffer *buffer = b->buffer;
	struct spa_data *last_data = &buffer->datas[buffer->n_datas - 1];
	struct spa_meta_sync_timeline *synctimeline =
		spa_buffer_find_meta_data(buffer, SPA_META_SyncTimeline, sizeof(struct spa_meta_sync_timeline));

	if (synctimeline && (last_data->type == SPA_DATA_SyncObj))
		gs_sync_signal_syncobj_timeline_point(last_data->fd, synctimeline->release_point);
#endif
	pw_stream_queue_buffer(stream, b);
}

static inline struct pw_buffer *find_latest_buffer(struct pw_stream *stream)
{
	struct pw_buffer *b;

	/* Find the most recent buffer */
	b = NULL;
	while (true) {
		struct pw_buffer *aux = pw_stream_dequeue_buffer(stream);
		if (!aux)
			break;
		if (b) {
			blog(LOG_DEBUG, "[pipewire] Drop buffer!");
			return_unused_pw_buffer(stream, b);
		}
		b = aux;
	}

	blog(LOG_DEBUG, "[pipewire] Got buffer! %p", b);

	return b;
}

static uint32_t get_spa_buffer_plane_count(const struct spa_buffer *buffer)
{
	uint32_t plane_count = 0;

	while (plane_count < buffer->n_datas) {
		if (buffer->datas[plane_count].type == SPA_DATA_DmaBuf)
			plane_count++;
		else
			break;
	}

	return plane_count;
}

static enum video_colorspace video_colorspace_from_spa_color_matrix(enum spa_video_color_matrix matrix)
{
	switch (matrix) {
	case SPA_VIDEO_COLOR_MATRIX_RGB:
		return VIDEO_CS_DEFAULT;
	case SPA_VIDEO_COLOR_MATRIX_BT601:
		return VIDEO_CS_601;
	case SPA_VIDEO_COLOR_MATRIX_BT709:
		return VIDEO_CS_709;
	default:
		return VIDEO_CS_DEFAULT;
	}
}

static enum video_range_type video_color_range_from_spa_color_range(enum spa_video_color_range colorrange)
{
	switch (colorrange) {
	case SPA_VIDEO_COLOR_RANGE_0_255:
		return VIDEO_RANGE_FULL;
	case SPA_VIDEO_COLOR_RANGE_16_235:
		return VIDEO_RANGE_PARTIAL;
	default:
		return VIDEO_RANGE_DEFAULT;
	}
}

static bool prepare_obs_frame(obs_pipewire_stream *obs_pw_stream, struct obs_source_frame *frame)
{
	struct obs_pw_video_format obs_pw_video_format;

	frame->width = obs_pw_stream->format.info.raw.size.width;
	frame->height = obs_pw_stream->format.info.raw.size.height;

	video_format_get_parameters(video_colorspace_from_spa_color_matrix(obs_pw_stream->format.info.raw.color_matrix),
				    video_color_range_from_spa_color_range(obs_pw_stream->format.info.raw.color_range),
				    frame->color_matrix, frame->color_range_min, frame->color_range_max);

	if (!obs_pw_video_format_from_spa_format(obs_pw_stream->format.info.raw.format, &obs_pw_video_format) ||
	    obs_pw_video_format.video_format == VIDEO_FORMAT_NONE)
		return false;

	frame->format = obs_pw_video_format.video_format;
	frame->linesize[0] = SPA_ROUND_UP_N(frame->width * obs_pw_video_format.bpp, 4);
	return true;
}

static void process_video_async(obs_pipewire_stream *obs_pw_stream)
{
	struct spa_buffer *buffer;
	struct pw_buffer *b;
	bool has_buffer;

	b = find_latest_buffer(obs_pw_stream->stream);
	if (!b) {
		blog(LOG_DEBUG, "[pipewire] Out of buffers!");
		return;
	}

	buffer = b->buffer;
	has_buffer = buffer->datas[0].chunk->size != 0;

	if (!has_buffer)
		goto done;

#ifdef DEBUG_PIPEWIRE
	blog(LOG_DEBUG, "[pipewire] Buffer has memory texture");
#endif

	struct obs_source_frame out = {0};
	if (!prepare_obs_frame(obs_pw_stream, &out)) {
		blog(LOG_ERROR, "[pipewire] Couldn't prepare frame");
		goto done;
	}

	for (uint32_t i = 0; i < buffer->n_datas && i < MAX_AV_PLANES; i++) {
		out.data[i] = buffer->datas[i].data;
		if (out.data[i] == NULL) {
			blog(LOG_ERROR, "[pipewire] Failed to access data");
			goto done;
		}
	}

#ifdef DEBUG_PIPEWIRE
	blog(LOG_DEBUG, "[pipewire] Frame info: Format: %s, Planes: %u", get_video_format_name(out.format),
	     buffer->n_datas);
	for (uint32_t i = 0; i < buffer->n_datas && i < MAX_AV_PLANES; i++) {
		blog(LOG_DEBUG, "[pipewire] Plane %u: Dataptr:%p, Linesize:%d", i, out.data[i], out.linesize[i]);
	}
#endif

	obs_source_output_video(obs_pw_stream->source, &out);

done:
	pw_stream_queue_buffer(obs_pw_stream->stream, b);
}

static void import_dmabuf(obs_pipewire_stream *obs_pw_stream, struct pw_buffer *b)
{
	assert(!b->user_data);
	struct spa_buffer *buffer = b->buffer;

	struct obs_pw_video_format obs_pw_video_format;
	bool has_buffer = true;

	// Workaround for kwin behaviour pre 5.27.5
	// Workaround for mutter behaviour pre GNOME 43
	// Only check this if !SPA_META_Header, once supported platforms update.
	has_buffer = buffer->datas[0].chunk->size != 0;
	if (!has_buffer)
		return;

	if (buffer->datas[0].type == SPA_DATA_DmaBuf) {
		uint32_t planes = get_spa_buffer_plane_count(buffer);
		uint32_t *offsets = alloca(sizeof(uint32_t) * planes);
		uint32_t *strides = alloca(sizeof(uint32_t) * planes);
		uint64_t *modifiers = alloca(sizeof(uint64_t) * planes);
		int *fds = alloca(sizeof(int) * planes);
		bool use_modifiers;

#ifdef DEBUG_PIPEWIRE
		blog(LOG_DEBUG, "[pipewire] DMA-BUF info: fd:%ld, stride:%d, offset:%u, size:%dx%d",
		     buffer->datas[0].fd, buffer->datas[0].chunk->stride, buffer->datas[0].chunk->offset,
		     obs_pw_stream->format.info.raw.size.width, obs_pw_stream->format.info.raw.size.height);
#endif

		if (!obs_pw_video_format_from_spa_format(obs_pw_stream->format.info.raw.format, &obs_pw_video_format) ||
		    obs_pw_video_format.gs_format == GS_UNKNOWN) {
			blog(LOG_ERROR, "[pipewire] unsupported DMA buffer format: %d",
			     obs_pw_stream->format.info.raw.format);
			return;
		}

		for (uint32_t plane = 0; plane < planes; plane++) {
			fds[plane] = buffer->datas[plane].fd;
			offsets[plane] = buffer->datas[plane].chunk->offset;
			strides[plane] = buffer->datas[plane].chunk->stride;
			modifiers[plane] = obs_pw_stream->format.info.raw.modifier;
		}

		use_modifiers = obs_pw_stream->format.info.raw.modifier != DRM_FORMAT_MOD_INVALID;
		blog(LOG_INFO, "[pipewire] Importing dmabuf early");
		gs_texture_t *texture = gs_texture_create_from_dmabuf(
			obs_pw_stream->format.info.raw.size.width, obs_pw_stream->format.info.raw.size.height,
			obs_pw_video_format.drm_format, obs_pw_video_format.gs_format, planes, fds, strides, offsets,
			use_modifiers ? modifiers : NULL);

		b->user_data = texture;
	}
}

static void prepare_sync_buffer(obs_pipewire_stream *obs_pw_stream, struct present_buffer *pb)
{
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;
	struct spa_meta_cursor *cursor;
	struct spa_meta_region *region;
	struct spa_meta_videotransform *video_transform;
	struct obs_pw_video_format obs_pw_video_format;
	struct pw_buffer *b = pb->buf;
	struct spa_buffer *buffer = b->buffer;
	bool has_buffer = true;

	assert(pb->state == BUFFER_FREE);
	assert(!pb->texture);

	// Workaround for kwin behaviour pre 5.27.5
	// Workaround for mutter behaviour pre GNOME 43
	// Only check this if !SPA_META_Header, once supported platforms update.
	has_buffer = buffer->datas[0].chunk->size != 0;
	if (!has_buffer)
		goto read_metadata;

	if (buffer->datas[0].type == SPA_DATA_DmaBuf) {
		uint32_t planes = get_spa_buffer_plane_count(buffer);
		bool corrupt = false;
#if PW_CHECK_VERSION(1, 2, 0)
		struct spa_meta_sync_timeline *synctimeline =
			spa_buffer_find_meta_data(buffer, SPA_META_SyncTimeline, sizeof(struct spa_meta_sync_timeline));
#endif

#ifdef DEBUG_PIPEWIRE
		blog(LOG_DEBUG, "[pipewire] DMA-BUF info: fd:%ld, stride:%d, offset:%u, size:%dx%d",
		     buffer->datas[0].fd, buffer->datas[0].chunk->stride, buffer->datas[0].chunk->offset,
		     obs_pw_stream->format.info.raw.size.width, obs_pw_stream->format.info.raw.size.height);
#endif

		if (!obs_pw_video_format_from_spa_format(obs_pw_stream->format.info.raw.format, &obs_pw_video_format) ||
		    obs_pw_video_format.gs_format == GS_UNKNOWN) {
			blog(LOG_ERROR, "[pipewire] unsupported DMA buffer format: %d",
			     obs_pw_stream->format.info.raw.format);
			goto read_metadata;
		}

		for (uint32_t plane = 0; plane < planes; plane++) {
			corrupt |= (buffer->datas[plane].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) > 0;
		}

#if PW_CHECK_VERSION(1, 2, 0)
		if (synctimeline && (buffer->n_datas == (planes + 2))) {
			assert(buffer->datas[planes].type == SPA_DATA_SyncObj);
			assert(buffer->datas[planes + 1].type == SPA_DATA_SyncObj);

			pb->sync.acquire_syncobj_fd = buffer->datas[planes].fd;
			pb->sync.acquire_point = synctimeline->acquire_point;

			pb->sync.release_syncobj_fd = buffer->datas[planes + 1].fd;
			pb->sync.release_point = synctimeline->release_point;

			pb->sync.set = true;
		} else {
			pb->sync.set = false;
		}
#else
		obs_pw_stream->sync.set = false;
#endif

		if (corrupt) {
			blog(LOG_DEBUG, "[pipewire] buffer contains corrupted data");
			goto read_metadata;
		}

		if (pb_texture(pb) == NULL) {
			remove_modifier_from_format(obs_pw_stream, obs_pw_stream->format.info.raw.format,
						    obs_pw_stream->format.info.raw.modifier);
			pw_loop_signal_event(pw_thread_loop_get_loop(obs_pw->thread_loop), obs_pw_stream->reneg);
			goto read_metadata;
		}
	} else {
		blog(LOG_DEBUG, "[pipewire] Buffer has memory texture");

		if (!obs_pw_video_format_from_spa_format(obs_pw_stream->format.info.raw.format, &obs_pw_video_format) ||
		    obs_pw_video_format.gs_format == GS_UNKNOWN) {
			blog(LOG_ERROR, "[pipewire] unsupported buffer format: %d",
			     obs_pw_stream->format.info.raw.format);
			goto read_metadata;
		}

		if ((buffer->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) > 0) {
			blog(LOG_DEBUG, "[pipewire] buffer contains corrupted data");
			goto read_metadata;
		}

		if (buffer->datas[0].chunk->size == 0) {
			blog(LOG_DEBUG, "[pipewire] buffer contains empty data");
			goto read_metadata;
		}

		pb->texture = gs_texture_create(obs_pw_stream->format.info.raw.size.width,
						obs_pw_stream->format.info.raw.size.height,
						obs_pw_video_format.gs_format, 1,
						(const uint8_t **)&buffer->datas[0].data, GS_DYNAMIC);
	}

	if (obs_pw_video_format.swap_red_blue)
		swap_texture_red_blue(pb_texture(pb));

	/* Video Crop */
	region = spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop, sizeof(*region));
	if (region && spa_meta_region_is_valid(region)) {
#ifdef DEBUG_PIPEWIRE
		blog(LOG_DEBUG, "[pipewire] Crop Region available (%dx%d+%d+%d)", region->region.position.x,
		     region->region.position.y, region->region.size.width, region->region.size.height);
#endif

		pb->crop.x = region->region.position.x;
		pb->crop.y = region->region.position.y;
		pb->crop.width = region->region.size.width;
		pb->crop.height = region->region.size.height;
		pb->crop.valid = true;
	} else {
		pb->crop.valid = false;
	}

	/* Video Transform */
	video_transform = spa_buffer_find_meta_data(buffer, SPA_META_VideoTransform, sizeof(*video_transform));
	if (video_transform)
		pb->transform = video_transform->transform;
	else
		pb->transform = SPA_META_TRANSFORMATION_None;

read_metadata:

	/* Cursor */
	cursor = spa_buffer_find_meta_data(buffer, SPA_META_Cursor, sizeof(*cursor));
	pb->cursor.valid = cursor && spa_meta_cursor_is_valid(cursor);
	if (obs_pw_stream->cursor_visible && pb->cursor.valid) {
		struct spa_meta_bitmap *bitmap = NULL;

		if (cursor->bitmap_offset)
			bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset, struct spa_meta_bitmap);

		if (bitmap && bitmap->size.width > 0 && bitmap->size.height > 0 &&
		    obs_pw_video_format_from_spa_format(bitmap->format, &obs_pw_video_format) &&
		    obs_pw_video_format.gs_format != GS_UNKNOWN) {
			const uint8_t *bitmap_data;

			bitmap_data = SPA_MEMBER(bitmap, bitmap->offset, uint8_t);
			pb->cursor.hotspot_x = cursor->hotspot.x;
			pb->cursor.hotspot_y = cursor->hotspot.y;
			pb->cursor.width = bitmap->size.width;
			pb->cursor.height = bitmap->size.height;

			assert(pb->cursor.texture == NULL);
			pb->cursor.texture = gs_texture_create(pb->cursor.width, pb->cursor.height,
							       obs_pw_video_format.gs_format, 1, &bitmap_data,
							       GS_DYNAMIC);

			if (obs_pw_video_format.swap_red_blue)
				swap_texture_red_blue(pb->cursor.texture);
		}

		pb->cursor.x = cursor->position.x;
		pb->cursor.y = cursor->position.y;
	}

	pb->state = BUFFER_FULL;
}

static void destroy_buffer(obs_pipewire_stream *obs_pw_stream, struct present_buffer *pb)
{
	struct pw_stream *stream = obs_pw_stream->stream;

	if (pb->texture)
		gs_texture_destroy(pb->texture);

	if (pb->cursor.texture)
		gs_texture_destroy(pb->cursor.texture);

	if (pb->buf) {
		blog(LOG_DEBUG, "[pipewire] return buffer");
		if (pb->state < BUFFER_PRESENTED)
			return_unused_pw_buffer(stream, pb->buf);
		else
			pw_stream_queue_buffer(stream, pb->buf);
	}

	memset(pb, 0, sizeof(*pb));
}

static void pop_buffer(obs_pipewire_stream *obs_pw_stream, struct present_buffer *cur, struct present_buffer *next)
{
	obs_enter_graphics();

	if (cur->state == BUFFER_FREE) {
		blog(LOG_DEBUG, "[pipewire] buffer was free");

		*cur = *next;
		memset(next, 0, sizeof(*next));

		obs_leave_graphics();
		return;
	}

	// If the next buffer does not have a texture but does have a cursor,
	// this is a buffer without data (cursor only)
	if (next->state != BUFFER_FREE && next->cursor.texture && !next->texture) {
		// PipeWire buffer should never be held for cursor-only updates
		assert(!next->buf);
		if (cur->cursor.texture) {
			gs_texture_destroy(cur->cursor.texture);
		}
		cur->cursor = next->cursor;
		memset(next, 0, sizeof(*next));
		obs_leave_graphics();
		blog(LOG_DEBUG, "[pipewire] buffer was cursor");
		return;
	}

	blog(LOG_DEBUG, "[pipewire] destroy buffer");
	destroy_buffer(obs_pw_stream, cur);
	*cur = *next;
	memset(next, 0, sizeof(*next));

	obs_leave_graphics();
}

static void process_video_sync(obs_pipewire_stream *obs_pw_stream)
{
	struct spa_meta_header *header;
	struct pw_buffer *b;
	struct pw_stream *stream = obs_pw_stream->stream;
	struct present_buffer *pb;

	blog(LOG_DEBUG, "[pipewire] Process sync");

	b = find_latest_buffer(stream);
	if (!b) {
		blog(LOG_ERROR, "[pipewire] Out of buffers!");
		return;
	}

	struct spa_buffer *buffer = b->buffer;
	header = spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(*header));
	if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED) > 0) {
		blog(LOG_ERROR, "[pipewire] buffer is corrupt");
		return_unused_pw_buffer(obs_pw_stream->stream, b);
		return;
	}

	/* Skip double buffering if OBS is the timing source, since it makes no sense */
	bool no_double_buffering = pw_stream_is_driving(obs_pw_stream->stream) ||
				   pw_stream_is_lazy(obs_pw_stream->stream);

	obs_enter_graphics();

	bool double_buffering = obs_pw_stream->double_buffering && !no_double_buffering;

	/* If we are not double buffering, drop any queued buffers */
	while (!double_buffering && obs_pw_stream->buffers[0].state != BUFFER_FREE) {
		blog(LOG_DEBUG, "[pipewire] Dropped buffer (SB)!");
		pop_buffer(obs_pw_stream, &obs_pw_stream->buffers[0], &obs_pw_stream->buffers[1]);
	}

	/* If we have (at least) one buffer... */
	if (obs_pw_stream->buffers[0].state != BUFFER_FREE) {
		/* And we actually have two, then drop one */
		if (obs_pw_stream->buffers[1].state != BUFFER_FREE || !obs_pw_stream->double_buffering) {
			blog(LOG_DEBUG, "[pipewire] Dropped buffer (DB)!");
			pop_buffer(obs_pw_stream, &obs_pw_stream->buffers[0], &obs_pw_stream->buffers[1]);
		}
		assert(obs_pw_stream->buffers[1].state == BUFFER_FREE);
		pb = &obs_pw_stream->buffers[1];
	} else {
		pb = &obs_pw_stream->buffers[0];
	}

	assert(pb->state == BUFFER_FREE);
	pb->buf = b;
	prepare_sync_buffer(obs_pw_stream, pb);

	obs_leave_graphics();
}

static void on_process_cb(void *user_data)
{
	obs_pipewire_stream *obs_pw_stream = user_data;
	uint32_t output_flags;

	output_flags = obs_source_get_output_flags(obs_pw_stream->source);

	if (output_flags & OBS_SOURCE_VIDEO) {
		if (output_flags & OBS_SOURCE_ASYNC)
			process_video_async(obs_pw_stream);
		else
			process_video_sync(obs_pw_stream);
	}
}

static void on_param_changed_cb(void *user_data, uint32_t id, const struct spa_pod *param)
{
	obs_pipewire_stream *obs_pw_stream = user_data;
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[7];
	const char *format_name;
	uint32_t n_params = 0;
	uint32_t buffer_types;
	uint32_t output_flags;
	uint8_t params_buffer[1024];
	int result;
#if PW_CHECK_VERSION(1, 2, 0)
	bool supports_explicit_sync = false;
#endif

	if (!param || id != SPA_PARAM_Format)
		return;

	result = spa_format_parse(param, &obs_pw_stream->format.media_type, &obs_pw_stream->format.media_subtype);
	if (result < 0)
		return;

	if (obs_pw_stream->format.media_type != SPA_MEDIA_TYPE_video ||
	    obs_pw_stream->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_format_video_raw_parse(param, &obs_pw_stream->format.info.raw);

	output_flags = obs_source_get_output_flags(obs_pw_stream->source);

	buffer_types = 1 << SPA_DATA_MemPtr;
	bool has_modifier = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier) != NULL;
	if ((has_modifier || check_pw_version(&obs_pw->server_version, 0, 3, 24)) &&
	    (output_flags & OBS_SOURCE_ASYNC_VIDEO) != OBS_SOURCE_ASYNC_VIDEO) {
		buffer_types |= 1 << SPA_DATA_DmaBuf;
#if PW_CHECK_VERSION(1, 2, 0)
		obs_enter_graphics();
		supports_explicit_sync = gs_query_sync_capabilities();
		obs_leave_graphics();
#endif
	}

	blog(LOG_INFO, "[pipewire] Negotiated format:");

	format_name = spa_debug_type_find_name(spa_type_video_format, obs_pw_stream->format.info.raw.format);
	blog(LOG_INFO, "[pipewire]     Format: %d (%s)", obs_pw_stream->format.info.raw.format,
	     format_name ? format_name : "unknown format");

	if (has_modifier) {
		blog(LOG_INFO, "[pipewire]     Modifier: 0x%" PRIx64, obs_pw_stream->format.info.raw.modifier);
	}

	blog(LOG_INFO, "[pipewire]     Size: %dx%d", obs_pw_stream->format.info.raw.size.width,
	     obs_pw_stream->format.info.raw.size.height);

	blog(LOG_INFO, "[pipewire]     Framerate: %d/%d", obs_pw_stream->format.info.raw.framerate.num,
	     obs_pw_stream->format.info.raw.framerate.denom);

	/* Video crop */
	pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	params[n_params++] = spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
							SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
							SPA_PARAM_META_size,
							SPA_POD_Int(sizeof(struct spa_meta_region)));

	/* Cursor */
	params[n_params++] =
		spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
					   SPA_POD_Id(SPA_META_Cursor), SPA_PARAM_META_size,
					   SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1),
								    CURSOR_META_SIZE(1024, 1024)));

	/* Buffer options */
#if PW_CHECK_VERSION(1, 2, 0)
	if (supports_explicit_sync) {
		struct spa_pod_frame dmabuf_explicit_sync_frame;

		spa_pod_builder_push_object(&pod_builder, &dmabuf_explicit_sync_frame, SPA_TYPE_OBJECT_ParamBuffers,
					    SPA_PARAM_Buffers);
		spa_pod_builder_add(&pod_builder, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_DmaBuf), 0);
		spa_pod_builder_prop(&pod_builder, SPA_PARAM_BUFFERS_metaType, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_int(&pod_builder, 1 << SPA_META_SyncTimeline);

		params[n_params++] = spa_pod_builder_pop(&pod_builder, &dmabuf_explicit_sync_frame);
	}
#endif

	params[n_params++] = spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
							SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffer_types));

	/* Sync timeline */
#if PW_CHECK_VERSION(1, 2, 0)
	if (supports_explicit_sync) {
		params[n_params++] = spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
								SPA_PARAM_META_type, SPA_POD_Id(SPA_META_SyncTimeline),
								SPA_PARAM_META_size,
								SPA_POD_Int(sizeof(struct spa_meta_sync_timeline)));
	}
#endif

	/* Meta header */
	params[n_params++] = spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
							SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
							SPA_PARAM_META_size,
							SPA_POD_Int(sizeof(struct spa_meta_header)));

#if PW_CHECK_VERSION(0, 3, 62)
	if (check_pw_version(&obs_pw->server_version, 0, 3, 62)) {
		/* Video transformation */
		params[n_params++] = spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
								SPA_PARAM_META_type,
								SPA_POD_Id(SPA_META_VideoTransform),
								SPA_PARAM_META_size,
								SPA_POD_Int(sizeof(struct spa_meta_videotransform)));
	}
#endif
	pw_stream_update_params(obs_pw_stream->stream, params, n_params);
	obs_pw_stream->negotiated = true;
}

static void on_state_changed_cb(void *user_data, enum pw_stream_state old, enum pw_stream_state state,
				const char *error)
{
	UNUSED_PARAMETER(old);

	obs_pipewire_stream *obs_pw_stream = user_data;

	blog(LOG_INFO, "[pipewire] Stream %p state: \"%s\" (error: %s)", obs_pw_stream->stream,
	     pw_stream_state_as_string(state), error ? error : "none");
}

static void on_add_buffer(void *user_data, struct pw_buffer *pwbuffer)
{
	obs_pipewire_stream *obs_pw_stream = user_data;

	pwbuffer->user_data = NULL;

	blog(LOG_INFO, "[pipewire] Stream %p add_buffer: %p", obs_pw_stream->stream, pwbuffer);

	obs_enter_graphics();
	import_dmabuf(obs_pw_stream, pwbuffer);
	obs_leave_graphics();
}

static void on_remove_buffer(void *user_data, struct pw_buffer *pwbuffer)
{
	obs_pipewire_stream *obs_pw_stream = user_data;

	blog(LOG_INFO, "[pipewire] Stream %p remove_buffer: %p", obs_pw_stream->stream, pwbuffer);

	/* Lock to avoid racing buffer returns in draw callback */
	obs_enter_graphics();
	if (obs_pw_stream->buffers[0].buf == pwbuffer)
		obs_pw_stream->buffers[0].buf = NULL;
	if (obs_pw_stream->buffers[1].buf == pwbuffer)
		obs_pw_stream->buffers[1].buf = NULL;
	if (obs_pw_stream->presenting_buffer.buf == pwbuffer)
		obs_pw_stream->presenting_buffer.buf = NULL;

	if (pwbuffer->user_data)
		gs_texture_destroy(pwbuffer->user_data);

	pwbuffer->user_data = NULL;

	obs_leave_graphics();
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.add_buffer = on_add_buffer,
	.remove_buffer = on_remove_buffer,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
	.process = on_process_cb,
};

static void on_core_info_cb(void *user_data, const struct pw_core_info *info)
{
	obs_pipewire *obs_pw = user_data;

	update_pw_versions(obs_pw, info->version);
}

static void on_core_error_cb(void *user_data, uint32_t id, int seq, int res, const char *message)
{
	obs_pipewire *obs_pw = user_data;

	blog(LOG_ERROR, "[pipewire] Error id:%u seq:%d res:%d (%s): %s", id, seq, res, spa_strerror(res), message);

	pw_thread_loop_signal(obs_pw->thread_loop, false);
}

static void on_core_done_cb(void *user_data, uint32_t id, int seq)
{
	obs_pipewire *obs_pw = user_data;

	if (id == PW_ID_CORE && obs_pw->sync_id == seq)
		pw_thread_loop_signal(obs_pw->thread_loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info_cb,
	.done = on_core_done_cb,
	.error = on_core_error_cb,
};

/* obs_source_info methods */

obs_pipewire *obs_pipewire_connect(const struct pw_registry_events *registry_events, void *user_data)
{
	return obs_pipewire_connect_fd(-1, registry_events, user_data);
}

obs_pipewire *obs_pipewire_connect_fd(int pipewire_fd, const struct pw_registry_events *registry_events,
				      void *user_data)
{
	obs_pipewire *obs_pw;

	obs_pw = bzalloc(sizeof(obs_pipewire));
	obs_pw->pipewire_fd = pipewire_fd;
	obs_pw->thread_loop = pw_thread_loop_new("PipeWire thread loop", NULL);
	obs_pw->context = pw_context_new(pw_thread_loop_get_loop(obs_pw->thread_loop), NULL, 0);

	if (pw_thread_loop_start(obs_pw->thread_loop) < 0) {
		blog(LOG_WARNING, "Error starting threaded mainloop");
		bfree(obs_pw);
		return NULL;
	}

	pw_thread_loop_lock(obs_pw->thread_loop);

	/* Core */
	if (obs_pw->pipewire_fd >= 0)
		obs_pw->core =
			pw_context_connect_fd(obs_pw->context, fcntl(obs_pw->pipewire_fd, F_DUPFD_CLOEXEC, 5), NULL, 0);
	else
		obs_pw->core = pw_context_connect(obs_pw->context, NULL, 0);

	if (!obs_pw->core) {
		blog(LOG_WARNING, "Error creating PipeWire core: %m");
		pw_thread_loop_unlock(obs_pw->thread_loop);
		bfree(obs_pw);
		return NULL;
	}

	pw_core_add_listener(obs_pw->core, &obs_pw->core_listener, &core_events, obs_pw);

	// Dispatch to receive the info core event
	obs_pw->sync_id = pw_core_sync(obs_pw->core, PW_ID_CORE, obs_pw->sync_id);
	pw_thread_loop_wait(obs_pw->thread_loop);

	/* Registry */
	if (registry_events) {
		obs_pw->registry = pw_core_get_registry(obs_pw->core, PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(obs_pw->registry, &obs_pw->registry_listener, registry_events, user_data);
		blog(LOG_INFO, "[pipewire] Created registry %p", obs_pw->registry);
	}

	pw_thread_loop_unlock(obs_pw->thread_loop);

	spa_list_init(&obs_pw->streams);

	return obs_pw;
}

struct pw_registry *obs_pipewire_get_registry(obs_pipewire *obs_pw)
{
	return obs_pw->registry;
}

void obs_pipewire_roundtrip(obs_pipewire *obs_pw)
{
	pw_thread_loop_lock(obs_pw->thread_loop);

	obs_pw->sync_id = pw_core_sync(obs_pw->core, PW_ID_CORE, obs_pw->sync_id);
	pw_thread_loop_wait(obs_pw->thread_loop);

	pw_thread_loop_unlock(obs_pw->thread_loop);
}

void obs_pipewire_destroy(obs_pipewire *obs_pw)
{
	if (!obs_pw)
		return;
	obs_pipewire_stream *p;

	spa_list_consume(p, &obs_pw->streams, link)
	{
		spa_list_remove(&p->link);
		obs_pipewire_stream_destroy(p);
	}

	teardown_pipewire(obs_pw);
	bfree(obs_pw);
}

obs_pipewire_stream *obs_pipewire_connect_stream(obs_pipewire *obs_pw, obs_source_t *source, uint32_t pipewire_node,
						 const struct obs_pipwire_connect_stream_info *connect_info)
{
	struct spa_pod_builder pod_builder;
	const struct spa_pod **params = NULL;
	obs_pipewire_stream *obs_pw_stream;
	uint32_t n_params;
	uint8_t params_buffer[4096];

	assert(connect_info != NULL);

	obs_pw_stream = bzalloc(sizeof(obs_pipewire_stream));
	obs_pw_stream->name = strdup(connect_info->stream_name);
	obs_pw_stream->obs_pw = obs_pw;
	obs_pw_stream->source = source;
	obs_pw_stream->cursor_visible = connect_info->screencast.cursor_visible;
	obs_pw_stream->double_buffering = connect_info->double_buffering;
	obs_pw_stream->framerate.set = connect_info->video.framerate != NULL;
	obs_pw_stream->resolution.set = connect_info->video.resolution != NULL;
	pthread_mutex_init(&obs_pw_stream->lock, NULL);

	set_name_properties(connect_info->stream_properties, obs_pw_stream->name);

	if (obs_pw_stream->framerate.set)
		obs_pw_stream->framerate.fraction = *connect_info->video.framerate;

	if (obs_pw_stream->resolution.set)
		obs_pw_stream->resolution.rect = *connect_info->video.resolution;

	init_format_info(obs_pw_stream);

	pw_thread_loop_lock(obs_pw->thread_loop);

	/* Signal to renegotiate */
	obs_pw_stream->reneg =
		pw_loop_add_event(pw_thread_loop_get_loop(obs_pw->thread_loop), renegotiate_format, obs_pw_stream);
	blog(LOG_DEBUG, "[pipewire] registered event %p", obs_pw_stream->reneg);

	/* Signal to rename */
	obs_pw_stream->rename =
		pw_loop_add_event(pw_thread_loop_get_loop(obs_pw->thread_loop), rename_node, obs_pw_stream);
	blog(LOG_DEBUG, "[pipewire] registered event %p", obs_pw_stream->rename);

	/* Stream */
	obs_pw_stream->stream = pw_stream_new(obs_pw->core, connect_info->stream_name, connect_info->stream_properties);
	pw_stream_add_listener(obs_pw_stream->stream, &obs_pw_stream->stream_listener, &stream_events, obs_pw_stream);
	blog(LOG_INFO, "[pipewire] Created stream %p", obs_pw_stream->stream);

	/* Stream parameters */
	pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

	obs_get_video_info(&obs_pw_stream->video_info);

	if (!build_format_params(obs_pw_stream, &pod_builder, &params, &n_params)) {
		pw_thread_loop_unlock(obs_pw->thread_loop);
		bfree(obs_pw_stream);
		return NULL;
	}

	enum pw_stream_flags flags = PW_STREAM_FLAG_MAP_BUFFERS;
	if (pipewire_node != SPA_ID_INVALID)
		flags |= PW_STREAM_FLAG_AUTOCONNECT;

	pw_stream_connect(obs_pw_stream->stream, PW_DIRECTION_INPUT, pipewire_node, flags, params, n_params);

	blog(LOG_INFO, "[pipewire] Playing stream %p", obs_pw_stream->stream);

	pw_thread_loop_unlock(obs_pw->thread_loop);
	bfree(params);

	spa_list_append(&obs_pw->streams, &obs_pw_stream->link);

	return obs_pw_stream;
}

void obs_pipewire_stream_show(obs_pipewire_stream *obs_pw_stream)
{
	if (obs_pw_stream->stream) {
		pw_thread_loop_lock(obs_pw_stream->obs_pw->thread_loop);
		pw_stream_set_active(obs_pw_stream->stream, true);
		obs_pw_stream->active = true;
		pw_thread_loop_unlock(obs_pw_stream->obs_pw->thread_loop);
	}
}

void obs_pipewire_stream_hide(obs_pipewire_stream *obs_pw_stream)
{
	if (obs_pw_stream->stream) {
		pw_thread_loop_lock(obs_pw_stream->obs_pw->thread_loop);
		pw_stream_set_active(obs_pw_stream->stream, false);

		/* Clear out any queued frames */
		obs_enter_graphics();
		for (int i = 0; i < 2; i++)
			destroy_buffer(obs_pw_stream, &obs_pw_stream->buffers[i]);
		obs_pw_stream->active = false;
		obs_leave_graphics();

		pw_thread_loop_unlock(obs_pw_stream->obs_pw->thread_loop);
	}
}

uint32_t obs_pipewire_stream_get_width(obs_pipewire_stream *obs_pw_stream)
{
	bool has_crop;
	uint32_t ret = 0;

	if (!obs_pw_stream->negotiated)
		return 0;

	has_crop = has_effective_crop(obs_pw_stream);

	pthread_mutex_lock(&obs_pw_stream->lock);

	switch (obs_pw_stream->transform) {
	case SPA_META_TRANSFORMATION_Flipped:
	case SPA_META_TRANSFORMATION_None:
	case SPA_META_TRANSFORMATION_Flipped180:
	case SPA_META_TRANSFORMATION_180:
		ret = has_crop ? obs_pw_stream->crop.width : obs_pw_stream->width;
		break;
	case SPA_META_TRANSFORMATION_Flipped90:
	case SPA_META_TRANSFORMATION_90:
	case SPA_META_TRANSFORMATION_Flipped270:
	case SPA_META_TRANSFORMATION_270:
		ret = has_crop ? obs_pw_stream->crop.height : obs_pw_stream->height;
		break;
	default:
		ret = 0;
	}

	pthread_mutex_unlock(&obs_pw_stream->lock);

	return ret;
}

uint32_t obs_pipewire_stream_get_height(obs_pipewire_stream *obs_pw_stream)
{
	bool has_crop;
	uint32_t ret = 0;

	if (!obs_pw_stream->negotiated)
		return 0;

	has_crop = has_effective_crop(obs_pw_stream);

	pthread_mutex_lock(&obs_pw_stream->lock);

	switch (obs_pw_stream->transform) {
	case SPA_META_TRANSFORMATION_Flipped:
	case SPA_META_TRANSFORMATION_None:
	case SPA_META_TRANSFORMATION_Flipped180:
	case SPA_META_TRANSFORMATION_180:
		ret = has_crop ? obs_pw_stream->crop.height : obs_pw_stream->height;
		break;
	case SPA_META_TRANSFORMATION_Flipped90:
	case SPA_META_TRANSFORMATION_90:
	case SPA_META_TRANSFORMATION_Flipped270:
	case SPA_META_TRANSFORMATION_270:
		ret = has_crop ? obs_pw_stream->crop.width : obs_pw_stream->width;
		break;
	default:
		ret = 0;
	}

	pthread_mutex_unlock(&obs_pw_stream->lock);

	return ret;
}

void obs_pipewire_stream_video_render(obs_pipewire_stream *obs_pw_stream, gs_effect_t *effect)
{
	struct present_buffer *pb = get_buffer(obs_pw_stream);
	bool rotated;
	int flip = 0;

	gs_eparam_t *image;

	if (!pb)
		return;

	assert(pb->state != BUFFER_FREE);

	gs_texture_t *texture = pb_texture(pb);

	if (!texture || !pb->buf)
		return;

	if (pb->sync.set) {
		gs_sync_t *acquire_sync =
			gs_sync_create_from_syncobj_timeline_point(pb->sync.acquire_syncobj_fd, pb->sync.acquire_point);
		gs_sync_wait(acquire_sync);
		gs_sync_destroy(acquire_sync);
	}

	image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	rotated = push_rotation(obs_pw_stream);

	flip = get_buffer_flip(obs_pw_stream);

	/* There is a SPA_VIDEO_FLAG_PREMULTIPLIED_ALPHA flag, but it does not
	 * seem to be fully implemented nor ever set. Just assume premultiplied
	 * always, which seems to be the default convention.
	 *
	 * See https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/3126
	 *
	 * The cursor bitmap alpha mode is also not specified, and the
	 * convention there also seems to be premultiplied. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (has_effective_crop(obs_pw_stream)) {
		gs_draw_sprite_subregion(texture, flip, pb->crop.x, pb->crop.y, pb->crop.width, pb->crop.height);
	} else {
		gs_draw_sprite(texture, flip, 0, 0);
	}

	if (rotated)
		gs_matrix_pop();

	if (obs_pw_stream->cursor_visible && pb->cursor.valid && pb->cursor.texture) {
		float cursor_x = pb->cursor.x - pb->cursor.hotspot_x;
		float cursor_y = pb->cursor.y - pb->cursor.hotspot_y;

		gs_matrix_push();
		gs_matrix_translate3f(cursor_x, cursor_y, 0.0f);

		gs_effect_set_texture(image, pb->cursor.texture);
		gs_draw_sprite(pb->cursor.texture, 0, pb->cursor.width, pb->cursor.height);

		gs_matrix_pop();
	}

	gs_blend_state_pop();

	if (pb->sync.set) {
		gs_sync_t *release_sync = gs_sync_create();
		gs_sync_export_syncobj_timeline_point(release_sync, pb->sync.release_syncobj_fd,
						      pb->sync.release_point);
		gs_sync_destroy(release_sync);
	}

	assert(pb->state >= BUFFER_PRESENTING);
	pb->state = BUFFER_PRESENTED;
}

void obs_pipewire_stream_video_tick(obs_pipewire_stream *obs_pw_stream, float seconds)
{
	UNUSED_PARAMETER(seconds);
	obs_pw_stream->new_frame = true;
}

void obs_pipewire_stream_set_cursor_visible(obs_pipewire_stream *obs_pw_stream, bool cursor_visible)
{
	obs_pw_stream->cursor_visible = cursor_visible;
}

void obs_pipewire_stream_set_double_buffering(obs_pipewire_stream *obs_pw_stream, bool double_buffering)
{
	obs_pw_stream->double_buffering = double_buffering;
}

void obs_pipewire_stream_destroy(obs_pipewire_stream *obs_pw_stream)
{
	uint32_t output_flags;

	if (!obs_pw_stream)
		return;

	output_flags = obs_source_get_output_flags(obs_pw_stream->source);
	if (output_flags & OBS_SOURCE_ASYNC_VIDEO)
		obs_source_output_video(obs_pw_stream->source, NULL);

	pw_thread_loop_lock(obs_pw_stream->obs_pw->thread_loop);
	spa_list_remove(&obs_pw_stream->link);

	obs_enter_graphics();
	for (int i = 0; i < 2; i++)
		destroy_buffer(obs_pw_stream, &obs_pw_stream->buffers[i]);
	destroy_buffer(obs_pw_stream, &obs_pw_stream->presenting_buffer);
	obs_leave_graphics();

	if (obs_pw_stream->stream) {
		pw_stream_disconnect(obs_pw_stream->stream);
		pw_stream_destroy(obs_pw_stream->stream);
		obs_pw_stream->stream = NULL;
	}
	pw_thread_loop_unlock(obs_pw_stream->obs_pw->thread_loop);

	if (obs_pw_stream->name)
		free(obs_pw_stream->name);

	clear_format_info(obs_pw_stream);
	pthread_mutex_destroy(&obs_pw_stream->lock);

	bfree(obs_pw_stream);
}

void obs_pipewire_stream_set_framerate(obs_pipewire_stream *obs_pw_stream, const struct spa_fraction *framerate)
{
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;

	if ((!obs_pw_stream->framerate.set && !framerate) ||
	    (obs_pw_stream->framerate.set && framerate && obs_pw_stream->framerate.fraction.num == framerate->num &&
	     obs_pw_stream->framerate.fraction.denom == framerate->denom))
		return;

	if (framerate) {
		obs_pw_stream->framerate.fraction = *framerate;
		obs_pw_stream->framerate.set = true;
	} else {
		obs_pw_stream->framerate.fraction = SPA_FRACTION(0, 0);
		obs_pw_stream->framerate.set = false;
	}

	/* Signal to renegotiate */
	pw_loop_signal_event(pw_thread_loop_get_loop(obs_pw->thread_loop), obs_pw_stream->reneg);
}

void obs_pipewire_stream_set_resolution(obs_pipewire_stream *obs_pw_stream, const struct spa_rectangle *resolution)
{
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;

	if ((!obs_pw_stream->resolution.set && !resolution) ||
	    (obs_pw_stream->resolution.set && resolution && obs_pw_stream->resolution.rect.width == resolution->width &&
	     obs_pw_stream->resolution.rect.height == resolution->height))
		return;

	if (resolution) {
		obs_pw_stream->resolution.rect = *resolution;
		obs_pw_stream->resolution.set = true;
	} else {
		obs_pw_stream->resolution.rect = SPA_RECTANGLE(0, 0);
		obs_pw_stream->resolution.set = false;
	}

	/* Signal to renegotiate */
	pw_loop_signal_event(pw_thread_loop_get_loop(obs_pw->thread_loop), obs_pw_stream->reneg);
}

void obs_pipewire_stream_set_name(obs_pipewire_stream *obs_pw_stream, const char *name)
{
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;

	pthread_mutex_lock(&obs_pw_stream->lock);

	if (obs_pw_stream->name)
		free(obs_pw_stream->name);
	obs_pw_stream->name = strdup(name);

	pthread_mutex_unlock(&obs_pw_stream->lock);

	/* Signal to rename */
	pw_loop_signal_event(pw_thread_loop_get_loop(obs_pw->thread_loop), obs_pw_stream->rename);
}
