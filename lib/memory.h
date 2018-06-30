#ifndef __CLIP_MEMORY_H__
#define __CLIP_MEMORY_H__

#include <stdlib.h>
#include <xcb/xcb_event.h>

static inline void free_event(xcb_generic_event_t** event)
{
	if (*event) {
		free(*event);
		*event = NULL;
	}
}

#endif //__CLIP_MEMORY_H__
