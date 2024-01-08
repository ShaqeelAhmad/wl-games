#include <assert.h>
#include <cairo.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-decoration-unstable-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "shm.h"
#include "main.h"

#define XRES_NO_LOG
#define XRES_IMPLEMENTATION
#include "rgb.h"
#include "xres.h"

#if HOTRELOAD
static struct GameInterface *games;
static size_t games_len;
void (*selectUpdateDraw)(struct State *state, struct Input input, double dt);
#else
extern struct GameInterface games[];
extern size_t games_len;
void selectUpdateDraw(struct State *state, struct Input input, double dt);
#endif

#if HOTRELOAD
void
reload_games(void)
{
	static void *h = NULL;
	if (h) dlclose(h);

	h = dlopen("./libgames.so", RTLD_NOW | RTLD_LOCAL);
	assert(h != NULL);

	games_len = *(size_t *)dlsym(h, "games_len");
	games = dlsym(h, "games");
	selectUpdateDraw = dlsym(h, "selectUpdateDraw");
}
#endif

struct Buffer
newBuffer(int width, int height, struct wl_shm *shm)
{
	struct Buffer buf = {0};
	int stride = width * 4;

	int shm_pool_size = height * stride;

	buf.width = width;
	buf.height = height;
	buf.stride = stride;
	buf.data_sz = shm_pool_size;

	buf.fd = allocate_shm_file(shm_pool_size);
	if (buf.fd < 0) {
		fprintf(stderr, "Failed to allocate shm file\n");
		exit(1);
	}

	buf.data = mmap(NULL, shm_pool_size, PROT_READ | PROT_WRITE,
			MAP_SHARED, buf.fd, 0);
	if (buf.data == MAP_FAILED) {
		perror("mmap");
		close(buf.fd);
		exit(1);
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, buf.fd, shm_pool_size);
	if (pool == NULL) {
		fprintf(stderr, "Failed to create pool\n");
		close(buf.fd);
		munmap(buf.data, shm_pool_size);
		exit(1);
	}

	buf.wl_buf = wl_shm_pool_create_buffer(pool, 0, width,
			height, stride, WL_SHM_FORMAT_ARGB8888);
	if (buf.wl_buf == NULL) {
		fprintf(stderr, "Failed to create buffer\n");
		exit(1);
	}
	wl_shm_pool_destroy(pool);

	buf.surf = cairo_image_surface_create_for_data(
			(unsigned char *)buf.data, CAIRO_FORMAT_ARGB32,
			width, height, stride);
	if (cairo_surface_status(buf.surf) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "cairo: %s\n",
				cairo_status_to_string(cairo_surface_status(buf.surf)));
		exit(1);
	}
	buf.cr = cairo_create(buf.surf);
	if (cairo_status(buf.cr) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "cairo: %s\n",
				cairo_status_to_string(cairo_status(buf.cr)));
		exit(1);
	}

	return buf;
}

void
freeBuffer(struct Buffer *buf)
{
	if (buf->data == NULL) {
		return;
	}
	cairo_surface_finish(buf->surf);
	cairo_surface_destroy(buf->surf);
	cairo_destroy(buf->cr);

	wl_buffer_destroy(buf->wl_buf);
	close(buf->fd);

	munmap(buf->data, buf->data_sz);
	buf->data = NULL;
	buf->data_sz = 0;
	buf->surf = NULL;
	buf->cr = NULL;
	buf->wl_buf = NULL;
	buf->fd = -1;
}

void
xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_handle_ping,
};

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface,
		uint32_t serial)
{
	struct State *state = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	state->configured = true;
	state->redraw = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	struct State *state = data;
	state->quit = true;
}

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *xdg_toplevel,
		int32_t width, int32_t height, struct wl_array *states)
{
	struct State *state = data;
	if (width <= 0)
		state->width = 700;
	else
		state->width = width;

	if (height <= 0)
		state->height = 500;
	else
		state->height = height;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};
void
keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size)
{
	struct State *state = data;

	if (state->xkb_context == NULL)
		state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	char *s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (s == MAP_FAILED) {
		fprintf(stderr, "failed to get keyboard keymap\n");
		close(fd);
		exit(1);
	}
	if (state->xkb_keymap != NULL) {
		xkb_keymap_unref(state->xkb_keymap);
		state->xkb_keymap = NULL;
	}
	if (state->xkb_state != NULL) {
		xkb_state_unref(state->xkb_state);
		state->xkb_state = NULL;
	}

	state->xkb_keymap = xkb_keymap_new_from_string(state->xkb_context, s,
			XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

	state->xkb_state = xkb_state_new(state->xkb_keymap);

	munmap(s, size);
	close(fd);
}

// returns a boolean indicating if the key was handled, this would be useful
// when deciding to handle key repeat.
bool
handle_key(struct State *state, xkb_keysym_t keysym, bool released)
{
	if (!released && (keysym == XKB_KEY_q || keysym == XKB_KEY_Escape)) {
		if (state->cur_game < 0) {
			state->quit = true;
		} else {
			games[state->cur_game].fini(state);
			state->cur_game = -1;
		}
		state->redraw = true;

		return false;
	}
	if (!released && keysym == XKB_KEY_F5) {
#if HOTRELOAD
		reload_games();
#endif
		state->redraw = true;
		return false;
	}

	assert(state->input.keys_len+1 < MAX_INPUT_KEYS);
	state->input.keys[state->input.keys_len].keysym = keysym;
	if (released) {
		state->input.keys[state->input.keys_len].state = KEY_RELEASED;
	} else {
		state->input.keys[state->input.keys_len].state = KEY_PRESSED;
	}
	state->input.keys_len += 1;

	return true;
}

void
keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t
		serial, uint32_t time, uint32_t key, uint32_t keyState)
{
	struct State *state = data;

	const struct itimerspec zero_value = {0};
	if (state->repeat_key.fd != -1 &&
			timerfd_settime(state->repeat_key.fd, 0, &zero_value, NULL) < 0) {
		perror("timerfd_settime: stopping key repeat");
		exit(1);
	}

	if (state->xkb_state == NULL)
		return;

	// the scancode from this event is the Linux evdev scancode. To convert
	// this to an XKB scancode, we must add 8 to the evdev scancode.
	key += 8;

	xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state, key);

	if (keyState != WL_KEYBOARD_KEY_STATE_PRESSED) {
		handle_key(state, keysym, true);
		return;
	}

	if (handle_key(state, keysym, false) && state->repeat_key.fd != -1 &&
			xkb_keymap_key_repeats(state->xkb_keymap, key) &&
			state->repeat_rate != 0) {
		state->repeat_key.keysym = keysym;
		struct itimerspec t = {
			.it_value = {
				.tv_sec = 0,
				.tv_nsec = state->repeat_delay * 1000000,
			},
			.it_interval = {
				.tv_sec = 0,
				.tv_nsec = 1000000000 / state->repeat_rate,
			},
		};

		if (t.it_value.tv_nsec >= 1000000000) {
			t.it_value.tv_sec += t.it_value.tv_nsec / 1000000000;
			t.it_value.tv_nsec %= 1000000000;
		}
		if (t.it_interval.tv_nsec >= 1000000000) {
			t.it_interval.tv_sec += t.it_interval.tv_nsec / 1000000000;
			t.it_interval.tv_nsec %= 1000000000;
		}

		if (timerfd_settime(state->repeat_key.fd, 0, &t, NULL) < 0) {
			perror("timerfd_settime: starting key repeat");
			exit(1);
		}
	}
}

static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay)
{
	struct State *state = data;
	state->repeat_rate = rate;
	state->repeat_delay = delay;
}

void
keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group)
{
	struct State *state = data;
	xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
			mods_locked, 0, 0, group);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
}
static void
keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface)
{
}
struct wl_keyboard_listener keyboard_listener = {
	.enter = keyboard_handle_enter,
	.leave = keyboard_handle_leave,
	.keymap = keyboard_handle_keymap,
	.key = keyboard_handle_key,
	.modifiers = keyboard_handle_modifiers,
	.repeat_info = keyboard_handle_repeat_info,
};

static struct {
	char *name;
	struct wl_cursor *cursor;
} wayland_cursors[CURSOR_COUNT] = {
	[CURSOR_ARROW] = { .name = "left_ptr" },
	[CURSOR_WATCH] = { .name = "watch" },
	[CURSOR_LEFT]  = { .name = "sb_left_arrow" },
	[CURSOR_RIGHT] = { .name = "sb_right_arrow" },
};

static void
init_cursor(struct State *state)
{
	int i;
	int cursor_size = 24;
	char *env_cursor_size = getenv("XCURSOR_SIZE");
	if (env_cursor_size && *env_cursor_size) {
		char *end;
		int n = strtol(env_cursor_size, &end, 0);
		if (*end == '\0' && n > 0) {
			cursor_size = n;
		}
	}

	char *cursor_theme = getenv("XCURSOR_THEME");
	state->pointer.theme = wl_cursor_theme_load(cursor_theme, cursor_size, state->shm);
	if (!state->pointer.theme) {
		fprintf(stderr, "error: can't load %s cursor theme\n",
				cursor_theme == NULL ? "default" : cursor_theme );
		exit(1);
	}
	for (i = 0; i < (int)ARRAY_LEN(wayland_cursors); i++) {
		wayland_cursors[i].cursor = wl_cursor_theme_get_cursor(state->pointer.theme,
				wayland_cursors[i].name);
	}
	if (wayland_cursors[0].cursor == NULL) {
		fprintf(stderr, "error: %s cursor theme doesn't have cursor left_ptr\n",
				cursor_theme == NULL ? "default" : cursor_theme);
		exit(1);
	}
	state->pointer.cursor = wayland_cursors[0].cursor;
	state->pointer.image = state->pointer.cursor->images[0];
	state->pointer.curimg = 0;
	struct wl_buffer *cursor_buffer = wl_cursor_image_get_buffer(state->pointer.image);
	state->pointer.surface = wl_compositor_create_surface(state->compositor);
	wl_surface_attach(state->pointer.surface, cursor_buffer, 0, 0);
	wl_surface_damage_buffer(state->pointer.surface, 0, 0,
			state->pointer.image->width, state->pointer.image->height);
	wl_surface_commit(state->pointer.surface);
}

static void cursor_callback(void *data, struct wl_callback *cb, uint32_t time);

static struct wl_callback_listener cursor_callback_listener = {
	.done = cursor_callback,
};

static void
render_cursor(struct State *state)
{
	state->pointer.image = state->pointer.cursor->images[state->pointer.curimg];
	struct wl_buffer *cursor_buffer = wl_cursor_image_get_buffer(state->pointer.image);

	wl_surface_attach(state->pointer.surface, cursor_buffer, 0, 0);
	wl_surface_damage_buffer(state->pointer.surface, 0, 0,
			state->pointer.image->width, state->pointer.image->height);
	wl_surface_commit(state->pointer.surface);

	wl_pointer_set_cursor(state->pointer.pointer, state->pointer.serial,
			state->pointer.surface,
			state->pointer.image->hotspot_x,
			state->pointer.image->hotspot_y);
}

static void
set_cursor(struct State *state, int cursor)
{
	if (cursor >= 0 && cursor < (int)ARRAY_LEN(wayland_cursors) &&
			state->pointer.cursor != wayland_cursors[cursor].cursor &&
			wayland_cursors[cursor].cursor != NULL) {
		state->pointer.curimg = 0;
		state->pointer.cursor = wayland_cursors[cursor].cursor;
	}
	render_cursor(state);
}

static void
cursor_callback(void *data, struct wl_callback *cb, uint32_t time)
{
	static uint32_t prevtime = 0;
	struct State *state = data;
	wl_callback_destroy(cb);

	cb = wl_surface_frame(state->pointer.surface);
	wl_callback_add_listener(cb, &cursor_callback_listener, state);

	render_cursor(state);

	if (state->pointer.cursor->image_count > 1) {
		if (time - prevtime >= state->pointer.image->delay) {
			state->pointer.curimg = (state->pointer.curimg+1) % state->pointer.cursor->image_count;
			prevtime = time;
		}
	}
}

void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t btnState)
{
	struct State *state = data;
	set_cursor(state, CURSOR_NONE);
}

static void
pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct State *state = data;
	state->pointer.serial = serial;

	wl_pointer_set_cursor(wl_pointer, serial, state->pointer.surface,
			state->pointer.image->hotspot_x, state->pointer.image->hotspot_y);
}
static void
pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface)
{
}
static void
pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
}
static void
pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
}
static void
pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis_source)
{
}
static void
pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis)
{
}
static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, int32_t discrete)
{
}
static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value)
{
}

struct wl_pointer_listener pointer_listener = {
	.enter         = pointer_handle_enter,
	.leave         = pointer_handle_leave,
	.motion        = pointer_handle_motion,
	.button        = pointer_handle_button,
	.axis          = pointer_handle_axis,
	.frame         = pointer_handle_frame,
	.axis_source   = pointer_handle_axis_source,
	.axis_stop     = pointer_handle_axis_stop,
	.axis_discrete = pointer_handle_axis_discrete,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities)
{
	struct State *state = data;
	if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
		struct wl_pointer *pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_listener, state);
		state->pointer.pointer = pointer;
	}

	if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, state);
	}
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.name = seat_handle_name,
	.capabilities = seat_handle_capabilities,
};

static void
handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct State *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		uint32_t wantVersion = 4;
		if (wantVersion > version) {
			fprintf(stderr, "error: wl_seat: want version %d got %d",
					wantVersion, version);
			exit(1);
		}
		state->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, wantVersion);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		state->xdg_wm_base = wl_registry_bind(registry, name,
				&xdg_wm_base_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		uint32_t wantVersion = 5;
		if (wantVersion > version) {
			fprintf(stderr, "error: wl_seat: want version %d got %d",
					wantVersion, version);
			exit(1);
		}

		struct wl_seat *seat = wl_registry_bind(registry, name,
				&wl_seat_interface, wantVersion);
		state->seat = seat;
		wl_seat_add_listener(seat, &seat_listener, state);
	} else if (strcmp(interface, wl_output_interface.name) == 0 &&
			state->output == NULL) {
		state->output = wl_registry_bind(registry, name,
				&wl_output_interface, 4);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		state->decor_manager = wl_registry_bind(registry, name,
				&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time);

static struct wl_callback_listener wl_surface_frame_listener = {
	.done = wl_surface_frame_done,
};

static void
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	static uint32_t prevTime = 0;
	struct State *state = data;

	wl_callback_destroy(cb);

	cb = wl_surface_frame(state->surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

	if (state->configured) {
		struct Buffer prev = state->buffer;
		state->buffer = newBuffer(state->width, state->height, state->shm);
		freeBuffer(&prev);

		state->configured = false;
		state->redraw = true;
	}

	double dt = (time - prevTime) / 1000.0;
	if (prevTime == 0) {
		dt = 1.0 / 60.0;
	}
	prevTime = time;

	int g = state->cur_game;
	assert(g < GAMES_COUNT);

	if (g < 0) {
		selectUpdateDraw(state, state->input, dt);
	} else {
		games[g].updateDraw(state, state->input, dt);
	}
	state->input.keys_len = 0;

	if (state->redraw) {
		wl_surface_attach(state->surface, state->buffer.wl_buf, 0, 0);
		wl_surface_damage_buffer(state->surface, 0, 0,
				state->width, state->height);
	}
	state->redraw = false;
	wl_surface_commit(state->surface);

	if (g < 0 && state->cur_game >= 0) {
		state->redraw = true;
	}
}

void
wayland_init(struct State *state)
{
	state->display = wl_display_connect(NULL);
	if (state->display == NULL) {
		fprintf(stderr, "Can't connect to the display\n");
		exit(1);
	}
	state->registry = wl_display_get_registry(state->display);
	wl_registry_add_listener(state->registry, &registry_listener, state);
	wl_display_roundtrip(state->display);

	if (state->shm == NULL || state->compositor == NULL || state->xdg_wm_base == NULL) {
		fprintf(stderr, "no wl_shm, xdg_wm_base or wl_compositor\n");
		exit(1);
	}
}

void
wayland_open(struct State *state, char *name)
{
	state->surface = wl_compositor_create_surface(state->compositor);
	state->xdg_surface = xdg_wm_base_get_xdg_surface(state->xdg_wm_base, state->surface);
	state->xdg_toplevel = xdg_surface_get_toplevel(state->xdg_surface);

	xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener, state);
	xdg_toplevel_add_listener(state->xdg_toplevel, &xdg_toplevel_listener, state);
	xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, state);

	xdg_toplevel_set_title(state->xdg_toplevel, name);
	xdg_toplevel_set_app_id(state->xdg_toplevel, name);

	if (state->decor_manager != NULL) {
		state->top_decor =
			zxdg_decoration_manager_v1_get_toplevel_decoration(
					state->decor_manager, state->xdg_toplevel);
		zxdg_toplevel_decoration_v1_set_mode(
				state->top_decor,
				ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}

	init_cursor(state);

	state->buffer = newBuffer(state->width, state->height, state->shm);

	wl_surface_commit(state->surface);
	wl_display_roundtrip(state->display);

	struct wl_callback *cb = wl_surface_frame(state->surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

	wl_surface_damage_buffer(state->surface, 0, 0, state->width, state->height);
	wl_surface_commit(state->surface);

	wl_display_roundtrip(state->display);
	wl_surface_attach(state->surface, state->buffer.wl_buf, 0, 0);
	wl_surface_commit(state->surface);

	cb = wl_surface_frame(state->pointer.surface);
	wl_callback_add_listener(cb, &cursor_callback_listener, state);
}


void
wayland_fini(struct State *state)
{
	if (state->top_decor)
		zxdg_toplevel_decoration_v1_destroy(state->top_decor);
	if (state->decor_manager)
		zxdg_decoration_manager_v1_destroy(state->decor_manager);

	wl_cursor_theme_destroy(state->pointer.theme);
	wl_surface_destroy(state->pointer.surface);

	freeBuffer(&state->buffer);

	if (state->xkb_keymap)
		xkb_keymap_unref(state->xkb_keymap);
	if (state->xkb_state)
		xkb_state_unref(state->xkb_state);
	if (state->xkb_context)
		xkb_context_unref(state->xkb_context);

	xdg_toplevel_destroy(state->xdg_toplevel);
	xdg_surface_destroy(state->xdg_surface);
	xdg_wm_base_destroy(state->xdg_wm_base);
	wl_surface_destroy(state->surface);
	wl_registry_destroy(state->registry);
	wl_display_disconnect(state->display);
}

bool
hasSuffix(char *s, int len, char *suffix, int suffixlen)
{
	if (suffixlen > len)
		return false;

	return memcmp(s + len - suffixlen, suffix, suffixlen) == 0;
}

bool
parseColor(struct Color *c, char *s)
{
	uint8_t r, g, b, a;
	if (!xres_color(s, &r, &g, &b, &a))
		return false;

	c->r = r / 255.0;
	c->g = g / 255.0;
	c->b = b / 255.0;
	c->a = a / 255.0;

	return true;
}

void
initState(struct State *state)
{
	state->width  = 640;
	state->height = 480;

	xres_load(NULL);
	if (!parseColor(&state->fg, xres_get(".foreground"))) {
		state->fg = (struct Color){
			.r = 1,
			.g = 1,
			.b = 1,
			.a = 1,
		};
	}

	if (!parseColor(&state->bg, xres_get(".background"))) {
		state->bg = (struct Color){
			.r = 0,
			.g = 0,
			.b = 0,
			.a = 1,
		};
	}

	char *colors_str[] = {
		"#1c1f24",
		"#ff4444",
		"#98fe65",
		"#dede00",
		"#2587cc",
		"#9f185f",
		"#2acea7",
		"#dfdfdf",
	};

	char s[] = ".colorN";
	int len = sizeof(s)-1;
	_Static_assert(COLORS_COUNT < 10, "update this code to handle double digits");
	for (int i = 0; i < COLORS_COUNT; i++) {
		s[len-1] = i + '0';
		if (!parseColor(&state->colors[i], xres_get(s))) {
			if (!parseColor(&state->colors[i], colors_str[i])) {
				assert(0 && "unreachable");
			}
		}
	}

	xres_unload();
}

enum Game
gameFromArg(char *arg, int arg_len)
{
	for (size_t i = 0; i < games_len; i++) {
		if (hasSuffix(arg, arg_len, games[i].name, strlen(games[i].name))) {
			return i;
		}
	}
	return -1;
}

int
main(int argc, char *argv[])
{
#if HOTRELOAD
	reload_games();
#endif

	srand(time(NULL));

	char *argv0 = argv[0];
	argv++;
	argc--;

	struct State state = {0};
	initState(&state);
	if (argc > 0) {
		int n = gameFromArg(*argv, strlen(*argv));
		if (n < 0) {
			fprintf(stderr, "unknown game %s\n", *argv);
			fprintf(stderr, "available games:\n");
			for (size_t i = 0; i < games_len; i++) {
				fprintf(stderr, "\t%s\n", games[i].name);
			}
			exit(1);
		}
		state.cur_game = n;
	} else {
		state.cur_game = gameFromArg(argv0, strlen(argv0));
	}
	if (state.cur_game >= 0) {
		games[state.cur_game].init(&state);
	}

	wayland_init(&state);
	wayland_open(&state, "wl-games");

	state.repeat_key.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (state.repeat_key.fd < 0) {
		perror("Failed to create timerfd, can't handle key repeats");
	}

	fd_set fds;
	int nfds = 0;
	int wl_fd = wl_display_get_fd(state.display);

	int ret = 1;
	while (ret > 0) {
		ret = wl_display_dispatch_pending(state.display);
		wl_display_flush(state.display);
	}
	if (ret < 0) {
		perror(" wl_display_dispatch_pending");
		exit(1);
	}

	while (!state.quit) {
		FD_ZERO(&fds);
		FD_SET(wl_fd, &fds);
		nfds = wl_fd;
		if (state.repeat_key.fd != -1) {
			FD_SET(state.repeat_key.fd, &fds);
			if (nfds < state.repeat_key.fd)
				nfds = state.repeat_key.fd;
		}

		select(nfds + 1, &fds, 0, 0, NULL);
		if (state.repeat_key.fd != -1 && FD_ISSET(state.repeat_key.fd, &fds)) {
			uint64_t expiration_count;
			ssize_t ret = read(state.repeat_key.fd,
					&expiration_count,
					sizeof(expiration_count));
			if (ret < 0) {
				if (errno != EAGAIN) {
					perror("key repeat error");
				}
			// silently ignore keypress if we don't have enough space.
			} else if (state.input.keys_len+1 < MAX_INPUT_KEYS) {
				xkb_keysym_t keysym = state.repeat_key.keysym;
				state.input.keys[state.input.keys_len].keysym = keysym;
				state.input.keys[state.input.keys_len].state = KEY_REPEAT;
				state.input.keys_len += 1;
			}
		}

		if (FD_ISSET(wl_fd, &fds)) {
			if (wl_display_dispatch(state.display) == -1) {
				perror("wl_display_dispatch");
				exit(1);
			}
			if (wl_display_flush(state.display) == -1 ) {
				perror("wl_display_flush");
				exit(1);
			}
		}
	}

	wayland_fini(&state);
	return 0;
}
