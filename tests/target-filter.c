#include "target-filter.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression)                                                              \
	do {                                                                           \
		if (!(expression)) {                                                   \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
			return EXIT_FAILURE;                                           \
		}                                                                      \
	} while (0)

int main(void)
{
	CHECK(is_production_video_stream("Video", "Stream/Output/Video", "Production"));
	CHECK(is_production_video_stream("Video", "Stream/Output/Video", NULL));
	CHECK(!is_production_video_stream(NULL, "Stream/Output/Video", "Production"));
	CHECK(!is_production_video_stream("Audio", "Stream/Output/Video", "Production"));
	CHECK(!is_production_video_stream("Video", NULL, "Production"));
	CHECK(!is_production_video_stream("Video", "Video/Source", "Production"));
	CHECK(!is_production_video_stream("Video", "Stream/Output/Video", "Camera"));
	CHECK(is_video_source("Video/Source"));
	CHECK(!is_video_source(NULL));
	CHECK(!is_video_source("Stream/Output/Video"));
	CHECK(!is_video_source("Audio/Source"));
	return EXIT_SUCCESS;
}
