// selsync X11 backend: mirror PRIMARY -> CLIPBOARD via XFIXES + ICCCM.
// Watches PRIMARY ownership changes (XFIXES), pulls the new content, then takes
// CLIPBOARD ownership and serves it on demand. Uses xcb only; no xclip/xsel.
//
// Limitation: INCR (chunked) transfers are not handled — typical highlight text
// is small and fits a single property. Text targets only.
#ifdef SELSYNC_X11
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

static xcb_connection_t *c;
static xcb_window_t win;
static xcb_atom_t A_PRIMARY, A_CLIPBOARD, A_UTF8, A_STRING, A_TEXT, A_TARGETS,
    A_PROP, A_INCR;

static char *clip_buf;
static size_t clip_len;

static xcb_atom_t intern(const char *name) {
	xcb_intern_atom_cookie_t ck =
	    xcb_intern_atom(c, 0, (uint16_t)strlen(name), name);
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck, NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

// Read our PROP property (the converted PRIMARY content) into clip_buf.
static void capture_primary(xcb_timestamp_t t) {
	xcb_get_property_cookie_t ck =
	    xcb_get_property(c, 1 /*delete*/, win, A_PROP, XCB_ATOM_ANY, 0, UINT32_MAX);
	xcb_get_property_reply_t *r = xcb_get_property_reply(c, ck, NULL);
	if (!r) return;
	if (r->type == A_INCR) { // chunked transfer unsupported
		fprintf(stderr, "selsync: INCR transfer skipped (selection too large)\n");
		free(r);
		return;
	}
	int len = xcb_get_property_value_length(r);
	if (r->type == XCB_ATOM_NONE || len <= 0) {
		free(r);
		return; // owner offered no UTF8_STRING, or empty: keep clipboard
	}
	char *buf = malloc((size_t)len);
	if (!buf) {
		free(r);
		return;
	}
	memcpy(buf, xcb_get_property_value(r), (size_t)len);
	free(r);
	free(clip_buf);
	clip_buf = buf;
	clip_len = (size_t)len;
	xcb_set_selection_owner(c, win, A_CLIPBOARD, t);
	xcb_flush(c);
}

static void serve_request(xcb_selection_request_event_t *req) {
	xcb_selection_notify_event_t ev = {0};
	ev.response_type = XCB_SELECTION_NOTIFY;
	ev.requestor = req->requestor;
	ev.selection = req->selection;
	ev.target = req->target;
	ev.time = req->time;
	ev.property = req->property ? req->property : req->target;

	if (req->target == A_TARGETS) {
		xcb_atom_t targets[] = {A_TARGETS, A_UTF8, A_STRING, A_TEXT};
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, ev.property,
		                    XCB_ATOM_ATOM, 32, 4, targets);
	} else if (req->target == A_UTF8 || req->target == A_STRING ||
	           req->target == A_TEXT) {
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, ev.property,
		                    req->target == A_UTF8 ? A_UTF8 : A_STRING, 8,
		                    (uint32_t)clip_len, clip_buf);
	} else {
		ev.property = XCB_ATOM_NONE; // unsupported target
	}
	xcb_send_event(c, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	xcb_flush(c);
}

int x11_main(void) {
	int screen_num;
	c = xcb_connect(NULL, &screen_num);
	if (!c || xcb_connection_has_error(c)) {
		fprintf(stderr, "selsync: cannot connect to X display\n");
		return 1;
	}
	const xcb_setup_t *setup = xcb_get_setup(c);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_num; i++) xcb_screen_next(&it);
	xcb_screen_t *screen = it.data;

	win = xcb_generate_id(c);
	xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0, 1, 1, 0,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);

	A_PRIMARY = XCB_ATOM_PRIMARY;
	A_CLIPBOARD = intern("CLIPBOARD");
	A_UTF8 = intern("UTF8_STRING");
	A_STRING = XCB_ATOM_STRING;
	A_TEXT = intern("TEXT");
	A_TARGETS = intern("TARGETS");
	A_PROP = intern("SELSYNC_PROP");
	A_INCR = intern("INCR");

	const xcb_query_extension_reply_t *xf =
	    xcb_get_extension_data(c, &xcb_xfixes_id);
	if (!xf || !xf->present) {
		fprintf(stderr, "selsync: XFIXES extension unavailable\n");
		return 1;
	}
	xcb_xfixes_query_version_reply_t *qv = xcb_xfixes_query_version_reply(
	    c, xcb_xfixes_query_version(c, 5, 0), NULL);
	free(qv);
	xcb_xfixes_select_selection_input(
	    c, win, A_PRIMARY,
	    XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER);
	xcb_flush(c);

	uint8_t xf_event = xf->first_event;
	fprintf(stderr, "selsync: backend X11 XFIXES\n");

	xcb_generic_event_t *e;
	while ((e = xcb_wait_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		if (type == xf_event + XCB_XFIXES_SELECTION_NOTIFY) {
			xcb_xfixes_selection_notify_event_t *ev =
			    (xcb_xfixes_selection_notify_event_t *)e;
			if (ev->selection == A_PRIMARY && ev->owner != XCB_NONE &&
			    ev->owner != win) {
				xcb_convert_selection(c, win, A_PRIMARY, A_UTF8, A_PROP,
				                      ev->timestamp);
				xcb_flush(c);
			}
		} else if (type == XCB_SELECTION_NOTIFY) {
			xcb_selection_notify_event_t *ev = (xcb_selection_notify_event_t *)e;
			if (ev->property != XCB_ATOM_NONE) capture_primary(ev->time);
		} else if (type == XCB_SELECTION_REQUEST) {
			serve_request((xcb_selection_request_event_t *)e);
		} else if (type == XCB_SELECTION_CLEAR) {
			free(clip_buf); // lost CLIPBOARD ownership
			clip_buf = NULL;
			clip_len = 0;
		}
		free(e);
	}
	return 0;
}
#endif
