// selsync: mirror the PRIMARY selection (highlight) into the CLIPBOARD.
// Self-contained: speaks the Wayland data-control protocol (or X11 XFIXES)
// directly. No external binaries (no wl-clipboard/xclip/xsel) are spawned.
//
// Backends, chosen at runtime:
//   - Wayland ext-data-control-v1        (preferred; niri/KWin/new wlroots)
//   - Wayland wlr-data-control-unstable-v1 v2 (fallback; sway/hyprland/river/...)
//   - X11 XFIXES + ICCCM selection ownership  (Xorg sessions)
//
// Limitation: GNOME/Mutter Wayland exposes no data-control protocol; clipboard
// management is impossible there for any client (wl-clipboard fails too).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>
#include "ext-data-control-v1-client.h"
#include "wlr-data-control-unstable-v1-client.h"

// Opcodes are identical across ext_* and zwlr_* (verified against both XMLs).
enum {
	MGR_CREATE_SOURCE = 0,
	MGR_GET_DEVICE = 1,
};
enum {
	DEV_SET_SELECTION = 0,
	DEV_DESTROY = 1,
	DEV_SET_PRIMARY = 2,
};
enum {
	SRC_OFFER = 0,
	SRC_DESTROY = 1,
};
enum {
	OFFER_RECEIVE = 0,
	OFFER_DESTROY = 1,
};

// Text MIME types we read from PRIMARY, best first.
static const char *read_mimes[] = {
	"text/plain;charset=utf-8", "text/plain",
	"UTF8_STRING", "STRING", "TEXT",
};
// Text MIME types we re-advertise on the CLIPBOARD; same bytes served for each.
// Broad set so the clipboard pastes everywhere (cf. the glfw private-type issue
// where a terminal's private type alone left the clipboard unpastable).
static const char *offer_mimes[] = {
	"text/plain;charset=utf-8", "text/plain",
	"UTF8_STRING", "STRING", "TEXT",
};

struct app {
	struct wl_display *dpy;
	struct wl_proxy *manager; // ext or wlr manager
	struct wl_proxy *device;
	const struct wl_interface *source_iface;
	const struct wl_interface *offer_iface; // unused directly; libwayland uses device iface
};

// Per-offer accumulated MIME list (hung off the offer proxy's user_data).
struct offer_data {
	char **mimes;
	int n;
};

// Per-source clipboard payload (hung off the source proxy's user_data).
struct src_data {
	char *buf;
	size_t len;
};

static struct app app;

static int write_all(int fd, const char *buf, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, buf + off, len - off);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1; // EPIPE etc.: receiver went away
		}
		off += (size_t)w;
	}
	return 0;
}

// Pick the best available read MIME from an offer's advertised list.
static const char *pick_read_mime(struct offer_data *od) {
	for (size_t i = 0; i < sizeof(read_mimes) / sizeof(*read_mimes); i++)
		for (int j = 0; j < od->n; j++)
			if (strcmp(read_mimes[i], od->mimes[j]) == 0) return read_mimes[i];
	// Fallback: any text/* type.
	for (int j = 0; j < od->n; j++)
		if (strncmp(od->mimes[j], "text/", 5) == 0) return od->mimes[j];
	return NULL;
}

// Synchronously pull an offer's data for `mime` into a malloc'd buffer.
static char *recv_offer(struct wl_proxy *offer, const char *mime, size_t *out_len) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) return NULL;
	wl_proxy_marshal_flags(offer, OFFER_RECEIVE, NULL,
	                       wl_proxy_get_version(offer), 0, mime, fds[1]);
	wl_display_flush(app.dpy);
	close(fds[1]);

	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(fds[0]);
		return NULL;
	}
	for (;;) {
		if (len == cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				close(fds[0]);
				return NULL;
			}
			buf = nb;
		}
		ssize_t r = read(fds[0], buf + len, cap - len);
		if (r < 0) {
			if (errno == EINTR) continue;
			free(buf);
			close(fds[0]);
			return NULL;
		}
		if (r == 0) break;
		len += (size_t)r;
	}
	close(fds[0]);
	*out_len = len;
	return buf;
}

// --- source (clipboard) listener ---
static void on_send(void *data, struct wl_proxy *source, const char *mime, int32_t fd) {
	(void)data;
	(void)mime;
	struct src_data *sd = wl_proxy_get_user_data(source);
	if (sd) write_all(fd, sd->buf, sd->len);
	close(fd);
}
static void on_cancelled(void *data, struct wl_proxy *source) {
	(void)data;
	struct src_data *sd = wl_proxy_get_user_data(source);
	if (sd) {
		free(sd->buf);
		free(sd);
	}
	wl_proxy_marshal_flags(source, SRC_DESTROY, NULL,
	                       wl_proxy_get_version(source), WL_MARSHAL_FLAG_DESTROY);
}
static void (*const source_listener[])(void) = {
	(void (*)(void))on_send,
	(void (*)(void))on_cancelled,
};

// Take ownership of the CLIPBOARD with `buf` (ownership transferred).
static void set_clipboard(char *buf, size_t len) {
	struct wl_proxy *src = wl_proxy_marshal_flags(
	    app.manager, MGR_CREATE_SOURCE, app.source_iface,
	    wl_proxy_get_version(app.manager), 0, NULL);
	if (!src) {
		free(buf);
		return;
	}
	struct src_data *sd = malloc(sizeof *sd);
	sd->buf = buf;
	sd->len = len;
	// add_listener stores its data arg in the same slot as set_user_data, so
	// set_user_data must come last to win.
	wl_proxy_add_listener(src, (void (**)(void))source_listener, &app);
	wl_proxy_set_user_data(src, sd);
	for (size_t i = 0; i < sizeof(offer_mimes) / sizeof(*offer_mimes); i++)
		wl_proxy_marshal_flags(src, SRC_OFFER, NULL,
		                       wl_proxy_get_version(src), 0, offer_mimes[i]);
	wl_proxy_marshal_flags(app.device, DEV_SET_SELECTION, NULL,
	                       wl_proxy_get_version(app.device), 0, src);
	wl_display_flush(app.dpy);
}

// --- offer listener ---
static void on_offer(void *data, struct wl_proxy *offer, const char *mime) {
	(void)data;
	struct offer_data *od = wl_proxy_get_user_data(offer);
	if (!od) return;
	char **nm = realloc(od->mimes, (od->n + 1) * sizeof(char *));
	if (!nm) return;
	od->mimes = nm;
	od->mimes[od->n++] = strdup(mime);
}
static void (*const offer_listener[])(void) = {
	(void (*)(void))on_offer,
};

static void destroy_offer(struct wl_proxy *offer) {
	struct offer_data *od = wl_proxy_get_user_data(offer);
	if (od) {
		for (int i = 0; i < od->n; i++) free(od->mimes[i]);
		free(od->mimes);
		free(od);
	}
	wl_proxy_marshal_flags(offer, OFFER_DESTROY, NULL,
	                       wl_proxy_get_version(offer), WL_MARSHAL_FLAG_DESTROY);
}

// --- device listener ---
static void on_data_offer(void *data, struct wl_proxy *device, struct wl_proxy *offer) {
	(void)data;
	(void)device;
	struct offer_data *od = calloc(1, sizeof *od);
	wl_proxy_add_listener(offer, (void (**)(void))offer_listener, &app);
	wl_proxy_set_user_data(offer, od); // must follow add_listener (same slot)
}
static void on_selection(void *data, struct wl_proxy *device, struct wl_proxy *offer) {
	(void)data;
	(void)device;
	// Regular clipboard changed; we don't mirror clipboard->primary. Discard.
	if (offer) destroy_offer(offer);
}
static void on_finished(void *data, struct wl_proxy *device) {
	(void)data;
	(void)device;
	fprintf(stderr, "selsync: data device finished by compositor\n");
	exit(0);
}
static void on_primary_selection(void *data, struct wl_proxy *device, struct wl_proxy *offer) {
	(void)data;
	(void)device;
	if (!offer) return; // highlight cleared: keep clipboard intact
	struct offer_data *od = wl_proxy_get_user_data(offer);
	const char *mime = od ? pick_read_mime(od) : NULL;
	if (!mime) {
		destroy_offer(offer);
		return;
	}
	size_t len = 0;
	char *buf = recv_offer(offer, mime, &len);
	destroy_offer(offer);
	if (!buf) return;
	if (len == 0) { // skip empty selections
		free(buf);
		return;
	}
	set_clipboard(buf, len);
}
static void (*const device_listener[])(void) = {
	(void (*)(void))on_data_offer,
	(void (*)(void))on_selection,
	(void (*)(void))on_finished,
	(void (*)(void))on_primary_selection,
};

// --- registry ---
struct globals {
	struct wl_registry *registry;
	uint32_t ext_name, ext_ver;
	uint32_t wlr_name, wlr_ver;
	uint32_t seat_name, seat_ver;
};
static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version) {
	(void)reg;
	struct globals *g = data;
	if (strcmp(iface, ext_data_control_manager_v1_interface.name) == 0) {
		g->ext_name = name;
		g->ext_ver = version;
	} else if (strcmp(iface, zwlr_data_control_manager_v1_interface.name) == 0) {
		g->wlr_name = name;
		g->wlr_ver = version;
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		g->seat_name = name;
		g->seat_ver = version;
	}
}
static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
	(void)data;
	(void)reg;
	(void)name;
}
static const struct wl_registry_listener registry_listener = {
	on_global, on_global_remove,
};

static int wayland_main(void) {
	app.dpy = wl_display_connect(NULL);
	if (!app.dpy) {
		fprintf(stderr, "selsync: cannot connect to Wayland display\n");
		return 1;
	}
	struct globals g = {0};
	g.registry = wl_display_get_registry(app.dpy);
	wl_registry_add_listener(g.registry, &registry_listener, &g);
	wl_display_roundtrip(app.dpy);

	if (!g.seat_name) {
		fprintf(stderr, "selsync: no wl_seat\n");
		return 1;
	}
	struct wl_seat *seat =
	    wl_registry_bind(g.registry, g.seat_name, &wl_seat_interface,
	                     g.seat_ver < 7 ? g.seat_ver : 7);

	const struct wl_interface *dev_iface, *offer_iface;
	if (g.ext_name) {
		app.manager = wl_registry_bind(g.registry, g.ext_name,
		                               &ext_data_control_manager_v1_interface, 1);
		app.source_iface = &ext_data_control_source_v1_interface;
		dev_iface = &ext_data_control_device_v1_interface;
		offer_iface = &ext_data_control_offer_v1_interface;
		fprintf(stderr, "selsync: backend ext-data-control-v1\n");
	} else if (g.wlr_name && g.wlr_ver >= 2) {
		app.manager = wl_registry_bind(g.registry, g.wlr_name,
		                               &zwlr_data_control_manager_v1_interface, 2);
		app.source_iface = &zwlr_data_control_source_v1_interface;
		dev_iface = &zwlr_data_control_device_v1_interface;
		offer_iface = &zwlr_data_control_offer_v1_interface;
		fprintf(stderr, "selsync: backend wlr-data-control v2\n");
	} else {
		fprintf(stderr,
		        "selsync: compositor exposes no usable data-control protocol "
		        "(GNOME/Mutter is unsupported)\n");
		return 1;
	}
	app.offer_iface = offer_iface;

	app.device = wl_proxy_marshal_flags(
	    app.manager, MGR_GET_DEVICE, dev_iface,
	    wl_proxy_get_version(app.manager), 0, NULL, seat);
	wl_proxy_add_listener(app.device, (void (**)(void))device_listener, &app);

	while (wl_display_dispatch(app.dpy) != -1)
		;
	return 0;
}

#ifdef SELSYNC_X11
int x11_main(void);
#endif

int main(void) {
	signal(SIGPIPE, SIG_IGN);
	if (getenv("WAYLAND_DISPLAY")) return wayland_main();
#ifdef SELSYNC_X11
	if (getenv("DISPLAY")) return x11_main();
	fprintf(stderr, "selsync: neither WAYLAND_DISPLAY nor DISPLAY set\n");
#else
	fprintf(stderr, "selsync: WAYLAND_DISPLAY not set (X11 backend not built)\n");
#endif
	return 1;
}
