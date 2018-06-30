#include <limits.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// libx
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

// libxcb
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_aux.h>

#include "lib/log.h"
#include "lib/memory.h"

/*
 * Most of the code I got from xcmenu, so there we can found some answers to
 * some questions we would have.
 */

#define LENGTH(x) (sizeof(x)/sizeof(*(x)))
#define ERRORS_NBR	256;

// global variables

static xcb_connection_t* xcb;
static xcb_window_t xcbw;
static xcb_screen_t* xcb_screen;

// xcb atoms
enum {
	UTF8_STRING = 0,
	XSEL_DATA,
	ATOMS_COUNT,
};

const char* atoms_names[] = {
	"UTF8_STRING",
	"XSEL_DATA",
};

static xcb_atom_t atoms[ATOMS_COUNT];

/** timeout to get xsel calls */
static int xcb_timeout_xsel_s = 0;
static int xcb_timeout_xsel_ns = 0;

/** timeout to xcb wait calls from command line */
static int xcb_timeout_daemon = 5000;	/// in nanoseconds
static int xcb_timeout_loop = 25000 * 25;

static void init_window(void)
{
	// XXX: do we really need OVERRIDE_REDIRECT here?
	uint32_t mask = XCB_CW_BACK_PIXEL |
	                XCB_CW_OVERRIDE_REDIRECT |
	                XCB_CW_EVENT_MASK;
	uint32_t value[3] = {0};

	// XXX: no checks of return values.
	xcb_screen = xcb_setup_roots_iterator(xcb_get_setup(xcb)).data;

	value[0] = xcb_screen->black_pixel;
	value[1] = 1;
	value[2] = XCB_EVENT_MASK_PROPERTY_CHANGE;

	xcbw = xcb_generate_id(xcb);

	xcb_create_window(xcb, xcb_screen->root_depth, xcbw, xcb_screen->root,
	                  0, 0, 1, 1, 0, XCB_COPY_FROM_PARENT,
	                  xcb_screen->root_visual, mask, value);

	if (xcb_connection_has_error(xcb)) {
		ERR("create_window error");
		return;
	}

	xcb_map_window(xcb, xcbw);
	xcb_flush(xcb);

	if (xcb_connection_has_error(xcb)) {
		ERR("map_window error");
		return;
	}
}

static int init_clipboard_protocol(void)
{
	xcb_intern_atom_reply_t* reply;
	xcb_intern_atom_cookie_t cookies[ATOMS_COUNT];

	memset(atoms, 0, sizeof(atoms));

	for (int i = 0; i < ATOMS_COUNT; ++i) {
		cookies[i] = xcb_intern_atom(xcb, 0, strlen(atoms_names[i]),
		                             atoms_names[i]);
	}

	for (int i = 0; i < ATOMS_COUNT; ++i) {
		if (!(reply = xcb_intern_atom_reply(xcb, cookies[i], NULL))) {
			continue;
		}

		atoms[i] = reply->atom;
		DEBUG("[%d] %s = 0x%x", i, atoms_names[i], reply->atom);
		free(reply);
	}

	return 0;
}

static int handle_selection_notify(xcb_generic_event_t* event)
{
	xcb_selection_notify_event_t* event_notify = NULL;

	DEBUG("handling selection_notify");

	event_notify = (xcb_selection_notify_event_t*) event; 
	if (event_notify->selection == XCB_ATOM_PRIMARY
		&& event_notify->property != XCB_NONE) {
		xcb_icccm_get_text_property_reply_t prop;
		xcb_get_property_cookie_t cookie =
		    xcb_icccm_get_text_property(xcb,
		                                event_notify->requestor,
		                                event_notify->property);

		if (xcb_icccm_get_text_property_reply(xcb,
						      cookie, &prop, NULL)) {
			DEBUG("%s", prop.name);

			xcb_icccm_get_text_property_reply_wipe(&prop);

			xcb_delete_property(xcb,
					    event_notify->requestor,
					    event_notify->property);

			free_event(&event);

		}
	}
	return 0;
}

static int handle_property_notify(xcb_generic_event_t* event)
{
	DEBUG("handling property_notify");
	return 0;
}

int selection_get(void)
{
	xcb_generic_error_t* error = NULL;

	xcb_convert_selection(xcb, xcbw,
	                      XCB_ATOM_PRIMARY, atoms[UTF8_STRING],
	                      atoms[XSEL_DATA], XCB_CURRENT_TIME);
	xcb_flush(xcb);

	if (xcb_connection_has_error(xcb)) {
		ERR("convert selection error");
		return 1;
	}

	if (xcb_poll_for_event(xcb) != NULL) {
		ERR("another window manager is already running");
		return 1;
	};

	xcb_flush(xcb);

	xcb_generic_event_t* event;

	DEBUG("Starting while");

	while ((event = xcb_wait_for_event(xcb))) {
		if (event == NULL) {
			DEBUG("xcb I/O error while waiting event");
			return 1;
		}

		if (event->response_type == 0) {
			error = (xcb_generic_error_t*)event;
			ERR("Got X11 error: %s: (%d)",
			    xcb_event_get_error_label(error->error_code),
			    error->error_code);

			if (xcb_connection_has_error(xcb)) {
				ERR("convert selection error");
				return 1;
			}
		}

		DEBUG("Got an event: %s (%d)",
		      xcb_event_get_label(event->response_type),
		      event->response_type);

		switch (XCB_EVENT_RESPONSE_TYPE(event)) {
		case XCB_SELECTION_NOTIFY:
			handle_selection_notify(event);
			break;
		case XCB_PROPERTY_NOTIFY:
			handle_property_notify(event);
			break;
		default:
			DEBUG("Unknown event.");
			free_event(&event);
			continue;
		}

	}

	free_event(&event);
	return 0;
}

int main()
{
	int ret = EXIT_SUCCESS;

	xcb = xcb_connect(NULL, NULL);

	init_window();
	init_clipboard_protocol();
	xcb_flush(xcb);

	if (xcb_connection_has_error(xcb)) {
		goto xcb_fail;
	}

	selection_get();

	goto out;

xcb_fail:
	ERR("XCB connection failed.");
	ret = EXIT_FAILURE;

out:
	xcb_disconnect(xcb);
	return ret;
}

