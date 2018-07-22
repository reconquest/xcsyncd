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
#include <xcb/xfixes.h>

#include "lib/log.h"
#include "lib/memory.h"
#include "cmdline.h"

/*
 * Most of the code I got from xcmenu, so there we can found some answers to
 * some questions we would have.
 */

#define LENGTH(x) (sizeof(x)/sizeof(*(x)))
#define ERRORS_NBR      256

#ifndef MAX_INCR
#define MAX_INCR        UINT_MAX
#endif

// types definitions

/**
 * @brief This structure handles imformation about incremential transfer.
 */
struct incrtransfer {
	xcb_window_t requestor;
	xcb_atom_t property;
	xcb_atom_t selection;
	unsigned int time;
	xcb_atom_t target;
	int format;
	void* data;
	size_t size;
	size_t offset;
	size_t max_size;
	size_t chunk;
	struct incrtransfer* next;
};
typedef struct incrtransfer incrtransfer_t;

struct selection {
	char* name;
	xcb_atom_t sel_atom;
	xcb_atom_t* targets;
	unsigned int targets_count;
	xcb_window_t owner;
	struct selection* sync_sel;
	bool watch;
};
typedef struct selection selection_t;

enum xcsync_mode {
	BOTH_WAYS,
	PRIM_TO_CLIP,
	CLIP_TO_PRIM,
};
typedef enum xcsync_mode xcsync_mode_t;

struct configuration {
	xcsync_mode_t mode;
};
typedef struct configuration configuration_t;

// global variables

static xcb_connection_t* xcb;
static xcb_window_t xcbw;
static xcb_screen_t* xcb_screen;
static configuration_t conf;

static uint8_t xcb_extension_first_event = 0;

// selections

enum {
	SEL_PRIM = 0,
	SEL_CLIP,
	SELECTIONS_COUNT,
};

static selection_t selections[SELECTIONS_COUNT];

// xcb atoms
enum {
	UTF8_STRING = 0,
	STRING,
	XSEL_DATA,
	INCR,
	TIMESTAMP,
	TARGETS,
	MULTIPLE,
	TEXT,
	ATOMS_COUNT,
};

const char* atoms_names[] = {
	"UTF8_STRING",
	"STRING",
	"XSEL_DATA",
	"INCR",
	"TIMESTAMP",
	"TARGETS",
	"MULTIPLE",
	"TEXT",
};

static xcb_atom_t atoms[ATOMS_COUNT];

// incremential transfers
static incrtransfer_t* transfers;

// selection data buffer
void* clipboard_data;


//static functions declarations

static int handle_property_notify(xcb_property_notify_event_t* event);
static void deinit_selections(void);


// functions implementation

static void init_window(void)
{
	uint32_t mask = XCB_CW_BACK_PIXEL |
	                XCB_CW_OVERRIDE_REDIRECT |
	                XCB_CW_EVENT_MASK;
	uint32_t value[3] = {0};

	xcb_screen = xcb_setup_roots_iterator(xcb_get_setup(xcb)).data;

	value[0] = xcb_screen->black_pixel;
	value[1] = 1;
	value[2] = XCB_EVENT_MASK_PROPERTY_CHANGE;

	xcbw = xcb_generate_id(xcb);

	xcb_create_window(xcb, xcb_screen->root_depth, xcbw, xcb_screen->root,
	                  0, 0, 1, 1, 0, XCB_COPY_FROM_PARENT,
	                  xcb_screen->root_visual, mask, value);

	xcb_map_window(xcb, xcbw);
	xcb_flush(xcb);
}

static int get_atom(const char* atom_name, xcb_atom_t* atom)
{
	xcb_intern_atom_cookie_t cookie;
	xcb_intern_atom_reply_t* reply = NULL;

	cookie = xcb_intern_atom(xcb, 0, strlen(atom_name),
				     atom_name);
	if (!(reply = xcb_intern_atom_reply(xcb, cookie, NULL))) {
		return 1;
	}

	*atom = reply->atom;
	DEBUG("%s = 0x%x", atom_name, reply->atom);
	free(reply);

	return 0;
}

static int init_selections(void)
{
	selection_t* prim = &selections[SEL_PRIM];
	selection_t* clip = &selections[SEL_CLIP];

	prim->name = strdup("PRIMARY");
	if (get_atom(prim->name, &prim->sel_atom)) {
		goto err_out;
	}
	if (conf.mode == BOTH_WAYS ||
	    conf.mode == PRIM_TO_CLIP) {
		prim->sync_sel = clip;
		prim->watch = true;
	}

	clip->name = strdup("CLIPBOARD");
	if (get_atom(clip->name, &clip->sel_atom)) {
		goto err_out;
	}
	if (conf.mode == BOTH_WAYS ||
	    conf.mode == CLIP_TO_PRIM) {
		clip->sync_sel = prim;
		clip->watch = true;
	}

	return 0;

err_out:
	deinit_selections();
	return 1;
}

static int init_clipboard_protocol(void)
{
	// init atoms
	memset(atoms, 0, sizeof(atoms));
	for (int i = 0; i < ATOMS_COUNT; ++i) {
		if (get_atom(atoms_names[i], &atoms[i])) {
			return 1;
		}
	}

	// init selections
	if (init_selections()) {
		return 1;
	}

	return 0;
}

static void deinit_selections(void) {
	for (int i = 0; i < SELECTIONS_COUNT; ++i) {
		if (selections[i].name) {
			free(selections[i].name);
			selections[i].name = NULL;
		}
	}
}

void* fetch_selection(xcb_window_t win, xcb_atom_t property, xcb_atom_t type,
                      size_t* len)
{
	xcb_get_property_reply_t* reply = NULL;
	xcb_get_property_cookie_t cookie;
	void* data, *string = NULL;
	size_t val_len;
	*len = 0;

	if (!property) {
		return NULL;
	}

	cookie = xcb_get_property(xcb, 0, win, property, type, 0, UINT_MAX);

	if (!(reply = xcb_get_property_reply(xcb, cookie, 0))) {
		return NULL;
	}

	val_len = xcb_get_property_value_length(reply);

	if (val_len == 0) {
		goto out;
	}

	data = xcb_get_property_value(reply);

	if (data == NULL) {
		goto out;
	}

	uint8_t format = reply->format;

	if (format < 8) {
		format = 8;
	}

	size_t rlen = val_len * format / 8;

	if (!(string = calloc(1, rlen + 1))) {
		goto out;
	}

	memcpy(string, data, rlen);
	*len = rlen;
	xcb_delete_property(xcb, win, property);

out:
	free(reply);
	return string;
}

static selection_t* we_handle_selection(xcb_atom_t selection)
{
	for (int i = 0; i < SELECTIONS_COUNT; ++i) {
		if (selections[i].sel_atom == selection) {
			return &selections[i];
		}
	}

	return NULL;
}

static int sync_selection(xcb_selection_notify_event_t* event)
{
	int ret = 0;
	void* data = NULL;
	size_t data_len = 0;

	selection_t* sel = we_handle_selection(event->selection);
	if (sel == NULL || !sel->watch) {
		return 0;
	}
	selection_t* sync_sel = sel->sync_sel;
	DEBUG("Selection: 0x%x; Sync selection: 0x%x",
	      sel->sel_atom, sync_sel->sel_atom);

	xcb_get_selection_owner_reply_t* reply = NULL;

	// own the clipboard
	xcb_set_selection_owner(xcb, xcbw, sync_sel->sel_atom, XCB_CURRENT_TIME);
	reply = xcb_get_selection_owner_reply(
	            xcb,
	            xcb_get_selection_owner(xcb,
	                                    sync_sel->sel_atom),
	            NULL);

	if (reply == NULL || reply->owner != xcbw) {
		DEBUG("Unable to own clipboard.");
		ret = 1;
		goto out;
	}
	sync_sel->owner = reply->owner;

	// get selection data
	data = fetch_selection(event->requestor, event->property,
	                                 event->target, &data_len);

	// If no data in new selection -- let things be as they are.
	if (data == NULL) {
		DEBUG("Clipboard data is empty");
		ret = 1;
		goto out;
	}

	// If there is new data in selection, replace old data with new.
	DEBUG("Freeing old clipboard data: %s", clipboard_data);

	free(clipboard_data);
	clipboard_data = data;

	DEBUG("New clipboard data is: %s", clipboard_data);

out:
	free(reply);
	return ret;
}

static void add_incr(incrtransfer_t* incr)
{
	incrtransfer_t* i;
	incr->next = NULL;

	for (i = transfers; i && i->next; i = i->next) {}

	if (!i) {
		i = transfers = malloc(sizeof(incrtransfer_t));
	}
	else {
		i = i->next = malloc(sizeof(incrtransfer_t));
	}

	if (!i) {
		return;
	}

	memcpy(i, incr, sizeof(incrtransfer_t));
}

static int _xcb_change_property(xcb_selection_notify_event_t* ev,
                                xcb_atom_t target, int format, size_t size,
                                void* data)
{
	unsigned int bytes;
	uint8_t mode = XCB_PROP_MODE_REPLACE;

	/* not possible */
	if (!data || !size) {
		return -1;
	}

	bytes = size * format / 8;

	if (bytes < MAX_INCR) {
		xcb_change_property(xcb, mode, ev->requestor,
		                    ev->property, target, format,
		                    size, data);
		return 0;
	}

	/* INCR transfer */
	ev->target = atoms[INCR];
	xcb_change_window_attributes(xcb, ev->requestor, XCB_CW_EVENT_MASK,
	&(unsigned int) {
		XCB_EVENT_MASK_PROPERTY_CHANGE
	});
	xcb_change_property(xcb, mode, ev->requestor, ev->property,
	                    atoms[INCR], format, MAX_INCR, data);
	xcb_send_event(xcb, 0, ev->requestor, XCB_EVENT_MASK_NO_EVENT,
	               (char*)ev);
	xcb_flush(xcb);

	incrtransfer_t incr;
	incr.requestor = ev->requestor;
	incr.property  = ev->property;
	incr.selection = ev->selection;
	incr.time      = ev->time;
	incr.target    = target;
	incr.format    = format;
	incr.data      = data;
	incr.size      = size;
	incr.offset    = 0;
	incr.max_size  = MAX_INCR * 8 / format;
	incr.chunk = incr.max_size < incr.size - incr.offset ?
	             incr.max_size : incr.size - incr.offset;
	add_incr(&incr);

	DEBUG("\4INCR transfer! (%zu/%zu)", incr.size, incr.max_size);
	return 1;
}

static int handle_selection_request(xcb_selection_request_event_t* event)
{
	DEBUG("handling selection_request");

	int incr = 0;
	xcb_selection_notify_event_t notify_event;

	if (event->selection != selections[SEL_CLIP].sel_atom) {
		DEBUG("We don't handle requests for this selection: 0x%x.",
		      event->selection);
		return 1;
	}

	notify_event.response_type = XCB_SELECTION_NOTIFY;
	notify_event.target = event->target;
	notify_event.requestor = event->requestor;
	notify_event.selection = event->selection;
	notify_event.time = event->time;
	notify_event.property = event->property;

	if (clipboard_data == NULL) {
		ERR("We don't have anything in clipboard buffer.");
		// TODO: what should we do if we can't send anything or can't
		// convert data to requested type? Sending event without
		// changing property works, but is it good behaviour?
		goto send_event;
	}

	//TODO: write test that will check if daemon properly returns
	//data of different types.
	if (event->target == atoms[UTF8_STRING]) {
		// TODO: this is probably not a good work with utf8 string. I
		// could have characrers bigger than 8-bits. We need to check
		// how it works with 16/32-bits characters.
		incr = _xcb_change_property(&notify_event, atoms[UTF8_STRING],
		                            8, strlen(clipboard_data),
		                            clipboard_data);
		DEBUG("UTF8 string sent: %s [%u]",
		      (char*)clipboard_data, strlen((char*)clipboard_data));
	}
	else if (event->target == atoms[STRING] ||
		 event->target == atoms[TEXT])
	{
		incr = _xcb_change_property(&notify_event, atoms[STRING],
					    8, strlen(clipboard_data),
					    clipboard_data);
		DEBUG("STRING sent: %s [%u]",
		      (char*)clipboard_data, strlen((char*)clipboard_data));
	}
	else {
		DEBUG("We don't handle this target yet (0x%x).", event->target);
		goto send_event;
	}

	if (incr == -1) {
		return 1;
	}

send_event:
	xcb_send_event(xcb, 0, event->requestor,
		       XCB_EVENT_MASK_NO_EVENT, (char*)&notify_event);
	xcb_flush(xcb);

	return 0;
}

static int handle_selection_notify(xcb_selection_notify_event_t* event)
{
	DEBUG("handling selection_notify");

	if (sync_selection(event)) {
		ERR("Can't own clipboard or get data.")
		return 1;
	}

	return 0;
}

static int handle_selection_clear(xcb_selection_clear_event_t* event)
{
	DEBUG("handling selection_clear");
	return 0;
}

static int handle_xfixes_selection_notify(xcb_xfixes_selection_notify_event_t*
        event)
{
	DEBUG("handling xfixes_selection_notify");

	selection_t* sel = we_handle_selection(event->selection);
	if (sel == NULL) {
		return 0;
	}
	DEBUG("Selection: 0x%x", sel->sel_atom);

	// requesting for selection conversion to get the new value. Event
	// will be handled in main event loop.
	xcb_convert_selection(xcb, xcbw,
	                      sel->sel_atom, atoms[UTF8_STRING],
	                      atoms[XSEL_DATA], XCB_CURRENT_TIME);
	xcb_flush(xcb);

	return 0;
}

static int handle_property_notify(xcb_property_notify_event_t* event)
{
	DEBUG("handling property_notify");

	if (event->atom == atoms[XSEL_DATA]) {
		DEBUG("XSEL_DATA property changed");
	}
	else {
		DEBUG("Some other property changed: 0x%x", event->atom);
	}

	return 0;
}

static int handle_events(xcb_generic_event_t* event)
{
	DEBUG("Got an event: %s (%d)",
	      xcb_event_get_label(event->response_type),
	      event->response_type);

	switch (XCB_EVENT_RESPONSE_TYPE(event)) {
	case XCB_SELECTION_REQUEST:
		handle_selection_request((xcb_selection_request_event_t*)event);
		break;

	case XCB_SELECTION_NOTIFY:
		handle_selection_notify((xcb_selection_notify_event_t*)event);
		break;

	case XCB_SELECTION_CLEAR:
		handle_selection_clear((xcb_selection_clear_event_t*)event);

	case XCB_PROPERTY_NOTIFY:
		handle_property_notify((xcb_property_notify_event_t*)event);
		break;

	default:
		if (XCB_EVENT_RESPONSE_TYPE(event) ==
		        xcb_extension_first_event + XCB_XFIXES_SELECTION_NOTIFY) {
			handle_xfixes_selection_notify((xcb_xfixes_selection_notify_event_t*)event);
		}
		else {
			DEBUG("Unknown event.");
		}

		break;
	}

	return 0;
}

int event_loop()
{
	xcb_generic_error_t* error = NULL;

	xcb_generic_event_t* event;

	xcb_discard_reply(xcb, xcb_xfixes_query_version(xcb, 1, 0).sequence);

	uint32_t mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
	                XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
	                XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
	for (int i = 0; i < SELECTIONS_COUNT; ++i) {
		if (selections[i].sel_atom) {
			xcb_xfixes_select_selection_input(xcb, xcbw,
			                                  selections[i].sel_atom,
			                                  mask);
		}
	}
	xcb_flush(xcb);

	while ((event = xcb_wait_for_event(xcb))) {
		// handling errors if any
		if (event == NULL) {
			ERR("xcb I/O error while waiting event");
			return 1;
		}

		if (event->response_type == 0) {
			error = (xcb_generic_error_t*)event;
			ERR("Got X11 error: %s: (%d)",
			    xcb_event_get_error_label(error->error_code),
			    error->error_code);

			if (xcb_connection_has_error(xcb)) {
				ERR("XCB connection error");
				return 1;
			}
		}

		// handling events
		handle_events(event);

		// freeing event object
		free_event(&event);
	}

	return 0;
}

int check_xfixes(void)
{
	const xcb_query_extension_reply_t* reply;

	reply = xcb_get_extension_data(xcb, &xcb_xfixes_id);

	if (!reply || !reply->present) {
		return -1;
	}

	xcb_extension_first_event = reply->first_event;

	return 0;
}

static int init_configuration(struct gengetopt_args_info* args_info)
{
	if (args_info->mode_given) {
		switch (args_info->mode_arg) {
			case mode_arg_BOTH_WAYS:
				conf.mode = BOTH_WAYS;
				break;
			case mode_arg_PRIM_TO_CLIP:
				conf.mode = PRIM_TO_CLIP;
				break;
			case mode_arg_CLIP_TO_PRIM:
				conf.mode = CLIP_TO_PRIM;
				break;
			default:
				return 1;
		}
	}

	return 0;
}

int main(int argc, char* argv[])
{
	int ret = EXIT_SUCCESS;
	struct gengetopt_args_info args_info;

	// read command line parameters to configuration
	if (cmdline_parser(argc, argv, &args_info)) {
		ERR("Command line parsing failure.");
		goto err_out;
	}
	if (init_configuration(&args_info)) {
		ERR("Wrong command line parameters.");
		goto err_out;
	}
	// free ggo structure
	cmdline_parser_free(&args_info);

	xcb = xcb_connect(NULL, NULL);

	if (xcb_connection_has_error(xcb)) {
		ERR("XCB connection failed.");
		goto err_out;
	}

	if (check_xfixes() != 0) {
		ERR("XFixes extension is not present!");
		goto err_out;
	}

	init_window();
	init_clipboard_protocol();

	event_loop();

	goto out;

err_out:
	ret = EXIT_FAILURE;
out:
	if (xcb) {
		xcb_disconnect(xcb);
	}

	return ret;
}

