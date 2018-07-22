#ifndef __CLIP_LOG_H__
#define __CLIP_LOG_H__

#include <stdio.h>

#define ERR(format, ...) \
	fprintf(stderr, "%s:%d Error:" format "\n", \
	        __FILE__, __LINE__, ##__VA_ARGS__); \
	fflush(stderr);

#ifdef DEBUG_BUILD
#define DEBUG(format, ...) \
	fprintf(stdout, "%s:%d DEBUG:" format "\n", \
	        __FILE__, __LINE__, ##__VA_ARGS__); \
	fflush(stdout);
#else
#define DEBUG(format, ...) while(0){}
#endif

#endif // __CLIP_LOG_H__
