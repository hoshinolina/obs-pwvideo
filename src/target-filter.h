#pragma once

#include <stdbool.h>
#include <string.h>

/* A Video/Source node can be an application, a camera or a screencast.
 * It need not set media.type or media.role (e.g. Gamescope). */
static inline bool is_video_source(const char *media_class)
{
	return media_class && strcmp(media_class, "Video/Source") == 0;
}

static inline bool is_production_video_stream(const char *media_type, const char *media_class, const char *media_role)
{
	if (!media_type || strcmp(media_type, "Video") != 0)
		return false;
	if (!media_class || strcmp(media_class, "Stream/Output/Video") != 0)
		return false;
	return !media_role || strcmp(media_role, "Production") == 0;
}
