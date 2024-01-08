#include <assert.h>
#include <cairo.h>
#include <cairo-svg.h>
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

#include "main.h"

_Static_assert(GAMES_COUNT == 6, "update this");
#define LIST_OF_GAMES \
	X(snake) \
	X(sudoku) \
	X(pong) \
	X(tetris) \
	X(car_race) \
	X(breakout)

#define X(name) \
	static void name ## _UpdateDraw(struct State *state, struct Input input, double dt); \
	static void name ## _Init(struct State *state); \
	static void name ## _Fini(struct State *state); \
	static void name ## _Preview(struct State *state, int x, int y, int size);

LIST_OF_GAMES

#undef X

struct GameInterface games[GAMES_COUNT] = {
#define X(name) {\
		# name, \
		name ## _UpdateDraw, \
		name ## _Init, \
		name ## _Fini, \
		name ## _Preview, \
	},

	LIST_OF_GAMES

#undef X
};
size_t games_len = ARRAY_LEN(games);

static bool
hasIntersectionF(struct FRect a, struct FRect b)
{
	return (a.x < b.x + b.w && a.x + a.w > b.x) &&
			(a.y < b.y + b.h && a.y + a.h > b.y);
}

static bool
hasIntersectionFLine(struct FLine line_a, struct FLine line_b)
{
	// https://gamedev.stackexchange.com/questions/26004/how-to-detect-2d-line-on-line-collision/26022#26022
	struct FVec2 a = line_a.points[0];
	struct FVec2 b = line_a.points[1];
	struct FVec2 c = line_b.points[0];
	struct FVec2 d = line_b.points[1];

	float denominator = ((b.x - a.x) * (d.y - c.y)) - ((b.y - a.y) * (d.x - c.x));
	float numerator1 = ((a.y - c.y) * (d.x - c.x)) - ((a.x - c.x) * (d.y - c.y));
	float numerator2 = ((a.y - c.y) * (b.x - a.x)) - ((a.x - c.x) * (b.y - a.y));

	// Detect coincident lines (has a problem, read the stackexchange link)
	if (denominator == 0) return numerator1 == 0 && numerator2 == 0;

	float r = numerator1 / denominator;
	float s = numerator2 / denominator;

	return (r >= 0 && r <= 1) && (s >= 0 && s <= 1);
}

static void *
erealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);
	if (p == NULL) {
		perror("realloc: ");
		exit(1);
	}

	return p;
}

static void
clearBuffer(struct Buffer *buf, struct Color c)
{
	cairo_save(buf->cr);
	cairo_set_source_rgba(buf->cr, c.r, c.g, c.b, c.a);
	cairo_set_operator(buf->cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(buf->cr);
	cairo_restore(buf->cr);
}

static double
lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

static float
lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

static struct FVec2
rotate(double x, double y, double angle)
{
	// https://en.wikipedia.org/wiki/Rotation_matrix#In_two_dimensions
	// x' = x*cos(angle) - y*sin(angle);
	// y' = x*sin(angle) + y*cos(angle);
	struct FVec2 result = {
		(float)(x * cos(angle) - y * sin(angle)),
		(float)(x * sin(angle) + y * cos(angle)),
	};
	return result;
}

static void
scaleAndCenterRect(int dst_width, int dst_height, int src_width,
		int src_height, int *xoff, int *yoff, float *scale)
{
	float wscale = (float)dst_width / (float)src_width;
	float hscale = (float)dst_height / (float)src_height;
	if (wscale < hscale) {
		*scale = wscale;
		int h = src_height**scale;
		if (yoff) *yoff = dst_height / 2 - h/2;
	} else {
		*scale = hscale;
		int w = src_width**scale;
		if (xoff) *xoff = dst_width / 2 - w/2;
	}
}

static void
snake_HandleKey(struct State *state, xkb_keysym_t key)
{
	struct Snake *s = &state->snake;
	switch (key) {
	case XKB_KEY_r:
		state->redraw = true;
		s->pause = false;
		s->lost = false;
		s->tails.len = 0;
		break;
	case XKB_KEY_space:
		state->redraw = true;
		s->pause = !s->pause;
		break;
	case XKB_KEY_Left: // fallthrough
	case XKB_KEY_h:
		if (s->dir != DIR_RIGHT)
			s->next_dir = DIR_LEFT;
		break;
	case XKB_KEY_Right: // fallthrough
	case XKB_KEY_l:
		if (s->dir != DIR_LEFT)
			s->next_dir = DIR_RIGHT;
		break;
	case XKB_KEY_Up: // fallthrough
	case XKB_KEY_k:
		if (s->dir != DIR_DOWN)
			s->next_dir = DIR_UP;
		break;
	case XKB_KEY_Down: // fallthrough
	case XKB_KEY_j:
		if (s->dir != DIR_UP)
			s->next_dir = DIR_DOWN;
		break;
	}
}

static void
snake_Draw(struct State *state, struct Snake *s)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	clearBuffer(buf, state->bg);

	int xoff = 0, yoff = 0;
	float scale = 1;
	scaleAndCenterRect(buf->width, buf->height, s->cols, s->rows, &xoff, &yoff, &scale);;
	cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_CYAN]));
	cairo_paint(cr);

	cairo_rectangle(cr, xoff, yoff, s->cols * scale, s->rows * scale);
	cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_GREEN]));
	cairo_fill(cr);

	// TODO: draw a more apple like shape.
	if (s->apple.x >= 0 && s->apple.y >= 0) {
		cairo_rectangle(cr,
				s->apple.x * scale + xoff,
				s->apple.y * scale + yoff,
				scale, scale);
		cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_RED]));
		cairo_fill(cr);
	}

	int n = 16;
	if (n < s->tails.len)
		n = s->tails.len;

	for (int i = s->tails.len-1; i >= 0; i--) {
		struct Vec2 *v = &s->tails.data[i];
		cairo_rectangle(cr,
				v->x * scale + xoff,
				v->y * scale + yoff,
				scale, scale);
		// XXX: can/should we use lerpf()?
		double c = (double)i / (double)n;
		cairo_set_source_rgba(cr, c * 0.8, 0.2, (1 - c) * 0.8 + 0.2, 1);
		cairo_fill(cr);
	}
	double x = s->x * scale + xoff;
	double y = s->y * scale + yoff;

	cairo_rectangle(cr, x, y, scale, scale);
	cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_BLUE]));
	cairo_fill(cr);

	double x1 = 0;
	double y1 = 0;
	double x2 = 0;
	double y2 = 0;
	// XXX: should we use a table for this?
	switch (s->dir) {
	case DIR_UP:
		x1 = x + 0.25 * scale;
		x2 = x + 0.75 * scale;
		y1 = y + 0.2 * scale;
		y2 = y1;
		break;
	case DIR_DOWN:
		x1 = x + 0.25 * scale;
		x2 = x + 0.75 * scale;
		y1 = y + 0.8 * scale;
		y2 = y1;
		break;
	case DIR_LEFT:
		y1 = y + 0.25 * scale;
		y2 = y + 0.75 * scale;
		x1 = x + 0.2 * scale;
		x2 = x1;
		break;
	case DIR_RIGHT:
		x1 = x + 0.8 * scale;
		x2 = x1;
		y1 = y + 0.25 * scale;
		y2 = y + 0.75 * scale;
		break;
	}

	cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_BLACK]));
	cairo_arc(cr, x1, y1, scale * 0.1, 0, PI * 2);
	cairo_fill(cr);
	cairo_arc(cr, x2, y2, scale * 0.1, 0, PI * 2);
	cairo_fill(cr);
}

static void
snake_UpdateDraw(struct State *state, struct Input input, double dt)
{
	for (size_t i = 0; i < input.keys_len; i++) {
		if (input.keys[i].state == KEY_PRESSED) {
			snake_HandleKey(state, input.keys[i].keysym);
		}
	}

	static double accumTime = 0;
	struct Snake *s = &state->snake;

	static int appleSpawn = 0;

render_lost:
	if (s->lost) {
		if (!state->redraw)
			return;
		snake_Draw(state, &state->snake);

		struct Buffer *buf = &state->buffer;
		cairo_t *cr = buf->cr;
		struct Color fg = state->bg;

		cairo_set_source_rgba(cr, COLOR_CAIRO(fg));

		char *text = "You lost";
		float fontSize = 0.25 * (float)buf->width;
		cairo_text_extents_t ext;
		cairo_set_font_size(cr, fontSize);
		cairo_text_extents(cr, text, &ext);
		int tx = buf->width/2 - ext.width / 2;
		int ty = buf->height/2 + ext.height/2;
		cairo_move_to(cr, tx, ty);
		cairo_show_text(cr, text);
		return;
	}

	if (s->pause) {
		if (!state->redraw)
			return;
		snake_Draw(state, &state->snake);

		struct Buffer *buf = &state->buffer;
		cairo_t *cr = buf->cr;

		int w = buf->width;
		int h = buf->height;
		int barw = 0.05 * w;
		int barh = 0.7 * h;

		cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
		cairo_paint(cr);

		cairo_rectangle(cr,
				w / 2 - barw * 2,
				h / 2 - barh/2,
				barw, barh);
		cairo_rectangle(cr,
				w / 2 + barw,
				h / 2 - barh/2,
				barw, barh);
		cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 1);
		cairo_fill(cr);
		return;
	}

	if (appleSpawn > 5) {
		state->redraw = true;

		appleSpawn = 0;

		// TODO: don't spawn inside snake;
		if (s->apple.x < 0 || s->apple.y < 0) {
			s->apple.x = rand() % s->cols;
			s->apple.y = rand() % s->rows;
		}
	}
	static double interval = 0.65;

	if (accumTime > interval) {
		state->redraw = true;
		s->dir = s->next_dir;

		accumTime = 0;
		appleSpawn += 1;

		struct Vec2 newTail = {-1, -1};

		if (s->tails.len > 0) {
			newTail = s->tails.data[s->tails.len-1];
		} else {
			newTail = (struct Vec2){s->x, s->y};
		}

		for (int i = s->tails.len-1; i > 0; i--) {
			struct Vec2 *v1 = &s->tails.data[i-1];
			struct Vec2 *v  = &s->tails.data[i];
			*v = *v1;
		}
		if (s->tails.len > 0) {
			struct Vec2 *v  = &s->tails.data[0];
			struct Vec2 v1 = {
				s->x,
				s->y,
			};
			*v = v1;
		}

		switch (s->dir) {
		case DIR_UP:
			s->y--;
			if (s->y < 0) {
				s->y = s->rows-1;
			}
			break;
		case DIR_DOWN:
			s->y++;
			if (s->y >= s->rows) {
				s->y = 0;
			}
			break;
		case DIR_LEFT:
			s->x--;
			if (s->x < 0) {
				s->x = s->cols-1;
			}
			break;
		case DIR_RIGHT:
			s->x++;
			if (s->x >= s->cols) {
				s->x = 0;
			}
			break;
		}
		for (int i = 0; i < s->tails.len; i++) {
			if (s->x == s->tails.data[i].x && s->y == s->tails.data[i].y) {
				s->lost = true;
				goto render_lost;
			}
		}

		if (s->apple.x == s->x && s->apple.y == s->y) {
			appleSpawn = 0;
			s->apple.x = -1;
			s->apple.y = -1;
			if (interval > 0.15)
				interval *= 0.9;
			APPEND(s->tails, newTail);
		}
	}
	accumTime += dt;

	snake_Draw(state, &state->snake);
}

static void
snake_Init(struct State *state)
{
	state->snake = (struct Snake){
		.x = 8,
		.y = 8,
		.cols = 16,
		.rows = 16,
		.apple = (struct Vec2){
			.x = -1,
			.y = -1,
		},
	};
}

static void
snake_Fini(struct State *state)
{
	struct Snake *s = &state->snake;
	free(s->tails.data);
	memset(&s->tails, 0, sizeof(s->tails));
}

static void
snake_Preview(struct State *state, int x, int y, int size)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	struct Color bg = state->bg;
	struct Color fg = state->fg;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));

	float snakeSize = size * 0.1;
	float xoff = size * 0.1;
	float yoff = size * 0.1;
	cairo_rectangle(cr,
			x + xoff, y + yoff,
			size * 0.5, snakeSize);
	cairo_fill(cr);

	cairo_rectangle(cr,
			x + xoff, y + yoff,
			snakeSize, size * 0.3);
	cairo_fill(cr);
	cairo_rectangle(cr,
			x + xoff, y + yoff + size * 0.3,
			size * 0.3, snakeSize);
	cairo_fill(cr);

	//cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_RED]));
	cairo_rectangle(cr,
			x + xoff + size * 0.3 + snakeSize, y + yoff + size * 0.3,
			snakeSize, snakeSize);
	cairo_fill(cr);

	cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
	cairo_arc(cr,
			x + xoff + size*0.3 - snakeSize/3,
			y + yoff + size * 0.3 + snakeSize/3,
			size * 0.01, 0, PI * 2);
	cairo_fill(cr);
	cairo_arc(cr,
			x + xoff + size*0.3 - snakeSize/3,
			y + yoff + size * 0.3 + snakeSize*2/3,
			size * 0.01, 0, PI * 2);
	cairo_fill(cr);
}

static bool
sudoku_IsValid(struct Sudoku *s, int x, int y, int n)
{
	assert(1 <= n && n <= 9);

	for (int i = 0; i < 9; i++) {
		if (i == x)
			continue;
		if (s->board[y][i].value == n)
			return false;
	}
	for (int i = 0; i < 9; i++) {
		if (i == y)
			continue;
		if (s->board[i][x].value == n)
			return false;
	}

	int box_y = 3 * (y / 3);
	int box_x = 3 * (x / 3);

	for (int dy = 0; dy < 3; dy++) {
		for (int dx = 0; dx < 3; dx++) {
			if (box_x + dx == x && box_y + dy == y)
				continue;
			if (s->board[box_y + dy][box_x + dx].value == n)
				return false;
		}
	}
	return true;
}

static bool
sudoku_FillTheRest(struct Sudoku *s)
{
	int x, y;
	for (y = 0; y < 9; y++) {
		for (x = 0; x < 9; x++) {
			if (s->board[y][x].value == 0)
				goto found_empty;
		}
	}
	// If there's no empty space, assume we finished filling it.
	return true;
found_empty:
	for (int n = 1; n <= 9; n++) {
		if (sudoku_IsValid(s, x, y, n)) {
			s->board[y][x].value = n;
			if (sudoku_FillTheRest(s))
				return true;
			s->board[y][x].value = 0;
		}
	}
	return false;
}

static void
sudoku_RemoveRandom(struct Sudoku *s)
{
	int diff = 40;
	int n = (rand() % diff) + diff;
	for (int i = 0; i < n; i++) {
		int x = rand() % 9;
		int y = rand() % 9;
		s->board[y][x].value = 0;
		s->board[y][x].user_fill = true;
	}
}

static struct Sudoku
sudoku_Gen(void)
{
	struct Sudoku s = {0};
	for (int i = 0; i < 9; i += 3) {
		for (int dy = 0; dy < 3; dy++) {
			for (int dx = 0; dx < 3; dx++) {
				int x = i + dx;
				int y = i + dy;
				int n = rand() % 9 + 1;
				if (!sudoku_IsValid(&s, x, y, n)) {
					for (n = 1; n <= 9; n++) {
						if (sudoku_IsValid(&s, x, y, n)) {
							break;
						}
					}
				}
				assert(n != 10);
				s.board[y][x].value = n;
			}
		}
	}

	if (!sudoku_FillTheRest(&s)) {
		assert(0 && "unreachable");
	}
	sudoku_RemoveRandom(&s);

	return s;
}

static void
sudoku_HandleKey(struct State *state, xkb_keysym_t key, bool repeat)
{
	struct Sudoku *s = &state->sudoku;
	struct SudokuCell *focus = &s->board[s->focus_y][s->focus_x];
	switch (key) {
	case XKB_KEY_r:
		if (repeat) {
			break;
		}
		state->redraw = true;
		*s = sudoku_Gen();
		break;
	case XKB_KEY_0: // fallthrough
	case XKB_KEY_space:
		if (repeat) {
			break;
		}
		state->redraw = true;
		if (focus->user_fill) {
			focus->value = 0;
			memset(focus->values, 0, sizeof(focus->values));
		}
		break;
	case XKB_KEY_1: // fallthrough
	case XKB_KEY_2: // fallthrough
	case XKB_KEY_3: // fallthrough
	case XKB_KEY_4: // fallthrough
	case XKB_KEY_5: // fallthrough
	case XKB_KEY_6: // fallthrough
	case XKB_KEY_7: // fallthrough
	case XKB_KEY_8: // fallthrough
	case XKB_KEY_9:
		if (repeat) {
			break;
		}
		state->redraw = true;
		if (!focus->user_fill) {
			break;
		}
		int n = key - XKB_KEY_0;
		if (focus->value == n) {
			focus->value = 0;
			break;
		}
		if (focus->value == 0) {
			if (focus->values[n-1]) {
				focus->values[n-1] = false;
				int count = 0;
				int last = 0;
				for (int i = 1; i <= 9; i++) {
					if (focus->values[i-1]) {
						count++;
						last = i;
					}
				}
				if (count == 1) {
					focus->values[last-1] = false;
					focus->value = last;
				}
			} else {
				bool exists = false;
				for (int i = 1; i <= 9; i++) {
					if (i == n)
						continue;
					if (focus->values[i-1]) {
						exists = true;
						break;
					}
				}
				if (exists) {
					focus->values[n-1] = true;
				} else {
					focus->value = n;
				}
			}
		} else {
			focus->values[(int)focus->value-1] = true;
			focus->values[n-1] = true;
			focus->value = 0;
		}
		break;
	case XKB_KEY_Left: // fallthrough
	case XKB_KEY_h:
		state->redraw = true;
		if (s->focus_x > 0)
			s->focus_x--;
		break;
	case XKB_KEY_Right: // fallthrough
	case XKB_KEY_l:
		state->redraw = true;
		if (s->focus_x < 8)
			s->focus_x++;
		break;
	case XKB_KEY_Up: // fallthrough
	case XKB_KEY_k:
		state->redraw = true;
		if (s->focus_y > 0)
			s->focus_y--;
		break;
	case XKB_KEY_Down: // fallthrough
	case XKB_KEY_j:
		state->redraw = true;
		if (s->focus_y < 8)
			s->focus_y++;
		break;
	}
}

static void
sudoku_Preview(struct State *state, int x, int y, int size)
{
	int board[9][9] = {
		[0] = {6,0,0,0,4,0,5,0,0},
		[1] = {0,2,0,1,5,0,0,4,0},
		[2] = {0,0,7,8,9,0,0,0,0},
		[3] = {0,0,6,0,1,0,9,8,0},
		[4] = {1,8,0,0,0,0,7,2,0},
		[5] = {0,4,0,0,8,0,3,0,1},
		[6] = {5,1,0,0,0,8,0,9,0},
		[7] = {0,6,2,5,0,0,0,0,3},
		[8] = {0,0,0,0,2,0,0,5,0},
	};
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	//struct Color bg = state->bg;
	struct Color fg = state->fg;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));

	float cellSize = size / 9.0;
	for (int i = 1; i < 9; i++) {
		cairo_move_to(cr, x + i * cellSize, y);
		cairo_line_to(cr, x + i * cellSize, y + size);
		if (i % 3 == 0) {
			cairo_set_line_width(cr, 2);
		} else {
			cairo_set_line_width(cr, 1);
		}
		cairo_stroke(cr);
	}
	for (int i = 1; i < 9; i++) {
		cairo_move_to(cr, x, y + i * cellSize);
		cairo_line_to(cr, x + size, y + i * cellSize);
		if (i % 3 == 0) {
			cairo_set_line_width(cr, 2);
		} else {
			cairo_set_line_width(cr, 1);
		}
		cairo_stroke(cr);
	}

	double fontSize = 0.1 * size;
	for (int cy = 0; cy < 9; cy++) {
		for (int cx = 0; cx < 9; cx++) {
			if (board[cy][cx] == 0) {
				continue;
			}
			char text[] = {board[cy][cx] + '0', '\0'};
			cairo_text_extents_t ext;
			cairo_set_font_size(cr, fontSize);
			cairo_text_extents(cr, text, &ext);
			int tx = x + cx * cellSize + ext.width / 2;
			int ty = y + cy * cellSize + fontSize;
			cairo_move_to(cr, tx, ty);
			cairo_show_text(cr, text);
		}
	}
}

static void
sudoku_UpdateDraw(struct State *state, struct Input input, double dt)
{
	for (size_t i = 0; i < input.keys_len; i++) {
		if (input.keys[i].state == KEY_RELEASED)
			continue;
		sudoku_HandleKey(state, input.keys[i].keysym,
				input.keys[i].state == KEY_REPEAT);
	}

	struct Sudoku *s = &state->sudoku;

	if (!state->redraw)
		return;

	struct Buffer *buf = &state->buffer;
	struct Color *fg = &state->fg;
	struct Color *bg = &state->bg;
	cairo_t *cr = buf->cr;

	cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_BLUE]));
	//cairo_set_source_rgba(cr, COLOR_CAIRO(*bg));
	cairo_paint(cr);

	int xoff = 0;
	int yoff = 0;
	int cols = 9;
	int rows = 9;
	float scale = 1;

	scaleAndCenterRect(buf->width - 10, buf->height - 10, cols, rows,
			&xoff, &yoff, &scale);
	xoff += 5;
	yoff += 5;

	for (int y = 0; y <= rows; y++) {
		cairo_move_to(cr, xoff, y * scale + yoff);
		cairo_line_to(cr, cols * scale + xoff, y * scale + yoff);
		cairo_set_source_rgba(cr, fg->r, fg->g, fg->b, fg->a);
		if (y % 3 == 0) {
			cairo_set_line_width(cr, 4);
		} else {
			cairo_set_line_width(cr, 2);
		}
		cairo_stroke(cr);
	}
	for (int x = 0; x <= cols; x++) {
		cairo_move_to(cr, x * scale + xoff, yoff);
		cairo_line_to(cr, x * scale + xoff, rows * scale + yoff);
		cairo_set_source_rgba(cr, fg->r, fg->g, fg->b, fg->a);
		if (x % 3 == 0) {
			cairo_set_line_width(cr, 4);
		} else {
			cairo_set_line_width(cr, 2);
		}
		cairo_stroke(cr);
	}


	// highlighting row, column and box.
	{
		cairo_set_source_rgba(cr, bg->r, bg->g, bg->b, bg->a);
		cairo_move_to(cr, s->focus_x * scale + xoff, s->focus_y * scale + yoff);
		cairo_rel_line_to(cr, scale, 0);
		cairo_rel_line_to(cr, 0, scale);
		cairo_rel_line_to(cr, -scale, 0);
		cairo_rel_line_to(cr, 0, -scale);
		cairo_set_line_width(cr, 4);
		cairo_stroke(cr);

		struct Color highlight = *bg;
		highlight.a = 0.15;

		cairo_set_source_rgba(cr, COLOR_CAIRO(highlight));

		cairo_move_to(cr, s->focus_x * scale + xoff, yoff);
		cairo_line_to(cr, s->focus_x * scale + xoff, rows * scale + yoff);
		cairo_line_to(cr, s->focus_x * scale + xoff + scale, rows * scale + yoff);
		cairo_line_to(cr, s->focus_x * scale + xoff + scale, yoff);
		cairo_fill(cr);

		cairo_move_to(cr, xoff, s->focus_y * scale + yoff);
		cairo_line_to(cr, cols * scale + xoff, s->focus_y * scale + yoff);
		cairo_line_to(cr, cols * scale + xoff, s->focus_y * scale + yoff + scale);
		cairo_line_to(cr, xoff, s->focus_y * scale + yoff + scale);
		cairo_fill(cr);

		int box_y = 3 * (s->focus_y / 3);
		int box_x = 3 * (s->focus_x / 3);

		cairo_rectangle(cr,
				box_x * scale + xoff,
				box_y * scale + yoff,
				3 * scale, 3 * scale);
		cairo_fill(cr);
	}

	double size = scale * 0.8;
	double subSize = scale * 0.4;
	bool completed = true;
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			struct Color c = *fg;
			if (s->board[y][x].user_fill) {
				c = *bg;
			}

			if (s->board[y][x].value == 0) {
				completed = false;
				int tx = xoff + x*scale;
				bool *values = &s->board[y][x].values[0];
				for (int i = 1; i <= 9; i++) {
					if (values[i-1]) {
						cairo_set_source_rgba(cr, COLOR_CAIRO(c));
						cairo_text_extents_t ext;
						int ty = size + yoff + y*scale;
						char text[] = { i + '0', '\0' } ;
						cairo_set_font_size(cr, subSize);
						cairo_text_extents(cr, text, &ext);
						tx += ext.width;
						cairo_move_to(cr, tx, ty);
						cairo_show_text(cr, text);
					}
				}
				continue;
			}

			if (!sudoku_IsValid(s, x, y, s->board[y][x].value)) {
				completed = false;
				c = state->colors[COLOR_RED];
			}
			cairo_set_source_rgba(cr, COLOR_CAIRO(c));

			cairo_text_extents_t ext;
			int ty = size + yoff + y*scale;
			int tx = xoff + x*scale;
			char text[] = { s->board[y][x].value + '0', '\0' } ;
			cairo_set_font_size(cr, size);
			cairo_text_extents(cr, text, &ext);
			tx += ext.width;
			cairo_move_to(cr, tx, ty);
			cairo_show_text(cr, text);
		}
	}

	if (completed) {
		char *text = "You Won";
		cairo_text_extents_t ext;
		cairo_set_source_rgba(cr, COLOR_CAIRO(*bg));
		cairo_set_font_size(cr, scale * 3);
		cairo_text_extents(cr, text, &ext);
		cairo_move_to(cr, buf->width / 2 - ext.width / 2, buf->height /
				2 + ext.height/2);
		cairo_show_text(cr, text);

		text = "press r to create a new game, or q to quit";
		cairo_set_source_rgba(cr, COLOR_CAIRO(*bg));
		cairo_set_font_size(cr, scale * 0.8);
		cairo_text_extents(cr, text, &ext);
		cairo_move_to(cr, buf->width / 2 - ext.width / 2, ext.height);
		cairo_show_text(cr, text);
	}
}

static void
sudoku_Init(struct State *state)
{
	state->sudoku = sudoku_Gen();
}

static void
sudoku_Fini(struct State *state)
{
	// noop
}

static void
pong_HandleKey(struct State *state, xkb_keysym_t key, bool released)
{
	struct Pong *p = &state->pong;
	switch (key) {
	case XKB_KEY_a:
		if (!released) {
			p->ai = !p->ai;
			p->player2_dy = 0;
		}
		break;
	case XKB_KEY_Up: // fallthrough
	case XKB_KEY_k:
		p->player1_dy = -PONG_PLAYER_DY;
		if (released)
			p->player1_dy = 0;
		break;
	case XKB_KEY_Down: // fallthrough
	case XKB_KEY_j:
		p->player1_dy = +PONG_PLAYER_DY;
		if (released)
			p->player1_dy = 0;
		break;
	case XKB_KEY_w:
		if (!p->ai) {
			p->player2_dy = -PONG_PLAYER_DY;
			if (released)
				p->player2_dy = 0;
		}
		break;
	case XKB_KEY_s:
		if (!p->ai) {
			p->player2_dy = +PONG_PLAYER_DY;
			if (released)
				p->player2_dy = 0;
		}
		break;
	}
}

static void
pong_UpdateDraw(struct State *state, struct Input input, double dt)
{
	for (size_t i = 0; i < input.keys_len; i++) {
		pong_HandleKey(state, input.keys[i].keysym,
				input.keys[i].state == KEY_RELEASED);
	}

	struct Buffer *buf = &state->buffer;
	struct Pong *p = &state->pong;
	struct Color *fg = &state->fg;
	struct Color *bg = &state->bg;
	cairo_t *cr = buf->cr;

	// We almost always want a redraw.
	state->redraw = true;

	if (p->ai) {
		if (p->player2_y < p->ball.y) {
			p->player2_dy = 0.5 * PONG_PLAYER_DY;
		} else {
			p->player2_dy = 0.5 * -PONG_PLAYER_DY;
		}
	}

	p->ball.y += dt * (double)p->ball_velocity.y;
	p->ball.x += dt * (double)p->ball_velocity.x;

	if (p->ball.y + PONG_BALL_RADIUS > PONG_HEIGHT || p->ball.y - PONG_BALL_RADIUS < 0) {
		p->ball_velocity.y *= -1;
	}

	if (p->ball.x + PONG_BALL_RADIUS > PONG_WIDTH) {
		p->score_left += 1;
		p->ball_velocity.x = -PONG_BALL_DX;
		p->ball.y = PONG_HEIGHT / 2;
		p->ball.x = PONG_WIDTH / 2;
	}
	if (p->ball.x - PONG_BALL_RADIUS < 0) {
		p->score_right += 1;
		p->ball_velocity.x = PONG_BALL_DX;
		p->ball.y = PONG_HEIGHT / 2;
		p->ball.x = PONG_WIDTH / 2;
	}


	p->player1_y += dt * p->player1_dy;
	p->player2_y += dt * p->player2_dy;

	if (p->player1_y - PONG_PLAYER_HEIGHT/2 < 0) {
		p->player1_y = PONG_PLAYER_HEIGHT/2;
	}
	if (p->player1_y + PONG_PLAYER_HEIGHT/2 > PONG_HEIGHT) {
		p->player1_y = PONG_HEIGHT - PONG_PLAYER_HEIGHT/2;
	}
	if (p->player2_y - PONG_PLAYER_HEIGHT/2 < 0) {
		p->player2_y = PONG_PLAYER_HEIGHT/2;
	}
	if (p->player2_y + PONG_PLAYER_HEIGHT/2 > PONG_HEIGHT) {
		p->player2_y = PONG_HEIGHT - PONG_PLAYER_HEIGHT/2;
	}


	struct FRect fBall = {
		.x = p->ball.x - PONG_BALL_RADIUS,
		.y = p->ball.y - PONG_BALL_RADIUS,
		.w = PONG_BALL_RADIUS * 2,
		.h = PONG_BALL_RADIUS * 2,
	};
	struct FRect fPlayer = {
		.x = PONG_PLAYER_X,
		.y = p->player1_y - (PONG_PLAYER_HEIGHT)/2,
		.w = PONG_PLAYER_WIDTH,
		.h = PONG_PLAYER_HEIGHT,
	};
	if (hasIntersectionF(fPlayer, fBall)) {
		float y = fBall.y - fPlayer.y;
		p->ball_velocity.y = PONG_BALL_DX * (2.0 * ((y / fPlayer.y) - 0.5));
		p->ball_velocity.x *= -1.1;
	};
	fPlayer.x = (PONG_WIDTH - PONG_PLAYER_X - PONG_PLAYER_WIDTH);
	fPlayer.y = p->player2_y - (PONG_PLAYER_HEIGHT)/2;
	if (hasIntersectionF(fPlayer, fBall)) {
		float y = fBall.y - fPlayer.y;
		p->ball_velocity.y = PONG_BALL_DX * (2.0 * ((y / fPlayer.y) - 0.5));

		p->ball_velocity.x *= -1.1;
	};
	if (fabsf(p->ball_velocity.x) > PONG_BALL_MAX_DX) {
		if (p->ball_velocity.x < PONG_BALL_MAX_DX) {
			p->ball_velocity.x = -PONG_BALL_MAX_DX;
		} else {
			p->ball_velocity.x = PONG_BALL_MAX_DX;
		}
	}

	cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_BLACK]));
	cairo_paint(cr);

	int xoff = 0, yoff = 0;
	float scale = 1;
	scaleAndCenterRect(buf->width, buf->height, PONG_WIDTH,
			PONG_HEIGHT, &xoff, &yoff, &scale);

	cairo_set_source_rgba(cr, COLOR_CAIRO(*bg));
	cairo_rectangle(cr, xoff, yoff, PONG_WIDTH * scale, PONG_HEIGHT * scale);
	cairo_fill(cr);

	cairo_rectangle(cr,
			PONG_PLAYER_X * scale + xoff,
			(p->player1_y - PONG_PLAYER_HEIGHT/2) * scale + yoff,
			PONG_PLAYER_WIDTH * scale,
			PONG_PLAYER_HEIGHT * scale);
	cairo_rectangle(cr,
			(PONG_WIDTH - PONG_PLAYER_X - PONG_PLAYER_WIDTH) * scale + xoff,
			(p->player2_y - PONG_PLAYER_HEIGHT/2) * scale + yoff,
			PONG_PLAYER_WIDTH * scale,
			PONG_PLAYER_HEIGHT * scale);
	cairo_set_source_rgba(cr, COLOR_CAIRO(*fg));
	cairo_fill(cr);

	cairo_set_source_rgba(cr, COLOR_CAIRO(*fg));
	cairo_arc(cr, p->ball.x * scale + xoff,
			p->ball.y * scale + yoff,
			PONG_BALL_RADIUS * scale,
			0, PI * 2);
	cairo_fill(cr);


	char score[128] = {0};
	snprintf(score, sizeof(score), "%d:%d", p->score_left, p->score_right);

	double size = scale * 32;

	cairo_set_source_rgba(cr, COLOR_CAIRO(*fg));
	cairo_text_extents_t ext;
	int ty = size + yoff;
	int tx = xoff + (PONG_WIDTH/2)*scale;
	cairo_set_font_size(cr, size);
	cairo_text_extents(cr, score, &ext);
	tx -= ext.width/2;
	cairo_move_to(cr, tx, ty);
	cairo_show_text(cr, score);
}

static void
pong_Init(struct State *state)
{
	state->pong = (struct Pong){
		.player1_y   = 400 / 2,
		.player2_y   = 400 / 2,
		.ai          = true,
		.score_left  = 0,
		.score_right = 0,
		.ball = (struct FVec2){
			.x = 600 / 2,
			.y = 400 / 2,
		},
		.ball_velocity = (struct FVec2) {
			.x = 80,
			.y = 80,
		},
	};
}

static void
pong_Fini(struct State *state)
{
	// noop
}

static void
pong_Preview(struct State *state, int x, int y, int size)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	//struct Color bg = state->bg;
	struct Color fg = state->fg;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
	cairo_arc(cr, x + size * 0.4, y + size * 0.4,
			size * 0.04, 0, PI * 2);
	cairo_fill(cr);

	float xoff = size * 0.1;
	float yoff = size * 0.1;
	float w = size * 0.04;
	float h = size * 0.4;

	cairo_rectangle(cr,
			x + xoff, y + yoff,
			w, h);
	cairo_fill(cr);

	cairo_rectangle(cr,
			x + size - xoff - w / 2, y + size - yoff - h,
			w, h);
	cairo_fill(cr);
}

static void
tetris_CurPiecePoints(struct Tetris *tetris, struct Vec2 points[4])
{
	int y0 = tetris->curPos.y;
	int x0 = tetris->curPos.x;

	switch (tetris->curPiece) {
	case TPIECE_STRAIGHT:
		switch (tetris->rotation) {
		case ROT_0: // fallthrough
		case ROT_180:
			for (int i = 0; i < 4; i++) {
				points[i].x = x0;
				points[i].y = y0 + i;
			}
			break;
		case ROT_90: // fallthrough
		case ROT_270:
			for (int i = 0; i < 4; i++) {
				points[i].x = x0 + i-2;
				points[i].y = y0;
			}
			break;
		default:
			break;
		}
		break;
	case TPIECE_SQUARE:
		// rotation doesn't do anything
		for (int dy = 0, i = 0; dy < 2; dy++) {
			int y = y0 + dy;
			for (int dx = 0; dx < 2; dx++) {
				points[i].x = x0 + dx;
				points[i].y = y;
				i++;
			}
		}
		break;
	case TPIECE_T:
		switch (tetris->rotation) {
		case ROT_0:
			for (int i = 0; i < 3; i++) {
				points[i].x = x0 + i;
				points[i].y = y0;
			}
			points[3].x = x0 + 1;
			points[3].y = y0 + 1;
			break;
		case ROT_90:
			for (int i = 0; i < 3; i++) {
				points[i].x = x0;
				points[i].y = y0+i;
			}
			points[3].x = x0 + 1;
			points[3].y = y0 + 1;
			break;
		case ROT_180:
			for (int i = 0; i < 3; i++) {
				points[i].x = x0 + i;
				points[i].y = y0+1;
			}
			points[3].x = x0 + 1;
			points[3].y = y0;
			break;
		case ROT_270:
			for (int i = 0; i < 3; i++) {
				points[i].x = x0+1;
				points[i].y = y0+i;
			}
			points[3].x = x0;
			points[3].y = y0 + 1;
			break;
		default:
			break;
		}
		break;
	case TPIECE_L:
		switch (tetris->rotation) {
		case ROT_0:
			points[0].x = x0;
			points[0].y = y0;

			points[1].x = x0;
			points[1].y = y0 + 1;

			points[2].x = x0;
			points[2].y = y0 + 2;

			points[3].x = x0 + 1;
			points[3].y = y0 + 2;
			break;
		case ROT_90:
			points[0].x = x0;
			points[0].y = y0 + 1;

			points[1].x = x0 + 1;
			points[1].y = y0 + 1;

			points[2].x = x0 + 2;
			points[2].y = y0 + 1;

			points[3].x = x0 + 2;
			points[3].y = y0;
			break;
		case ROT_180:
			points[0].x = x0+1;
			points[0].y = y0;

			points[1].x = x0+1;
			points[1].y = y0 + 1;

			points[2].x = x0+1;
			points[2].y = y0 + 2;

			points[3].x = x0;
			points[3].y = y0;
			break;
		case ROT_270:
			points[0].x = x0;
			points[0].y = y0;

			points[1].x = x0 + 1;
			points[1].y = y0;

			points[2].x = x0 + 2;
			points[2].y = y0;

			points[3].x = x0;
			points[3].y = y0+1;
			break;
		default:
			break;
		}
		break;
	case TPIECE_SKEW:
		switch (tetris->rotation) {
		case ROT_0: // fallthrough
		case ROT_180:
			points[0].x = x0;
			points[0].y = y0;

			points[1].x = x0+1;
			points[1].y = y0;

			points[2].x = x0+1;
			points[2].y = y0+1;

			points[3].x = x0+2;
			points[3].y = y0+1;
			break;
		case ROT_90: // fallthrough
		case ROT_270:
			y0 -= 1;
			x0 += 1;
			points[0].x = x0+1;
			points[0].y = y0;

			points[1].x = x0;
			points[1].y = y0+1;

			points[2].x = x0+1;
			points[2].y = y0+1;

			points[3].x = x0;
			points[3].y = y0+2;
			break;
		default:
			break;
		}
		break;
	default:
		assert(0 && "unreachable");
	}
}

static void
tetris_RemoveFilledLines(struct Tetris *tetris)
{
	for (int y = 0; y < TETRIS_HEIGHT; y++) {
		int n = 0;
		for (int x = 0; x < TETRIS_WIDTH; x++) {
			n += tetris->board[y][x] > 0;
		}
		if (n == TETRIS_WIDTH) {
			int y0 = y;
			int length = TETRIS_WIDTH * sizeof(tetris->board[0][0]);
			for (int i = y0; i > 0; i--) {
				memmove(tetris->board[i], tetris->board[i-1], length);
			}
			memset(tetris->board[0], 0, length);
		}
	}
}

static bool
tetris_HasCollision(struct Tetris *tetris)
{
	struct Vec2 points[4];
	tetris_CurPiecePoints(tetris, points);
	for (int i = 0; i < 4; i++) {
		if (points[i].y < 0)
			continue;
		if (points[i].x < 0 ||
				points[i].x >= TETRIS_WIDTH ||
				points[i].y >= TETRIS_HEIGHT ||
				tetris->board[points[i].y][points[i].x] != 0) {
			return true;
		}
	}
	return false;
}

static void
tetris_Update(struct State *state, struct Input input, double dt)
{
	struct Tetris *tetris = &state->tetris;
	int dx = 0;
	bool down = false;
	for (size_t i = 0; i < input.keys_len; i++) {
		if (input.keys[i].state == KEY_RELEASED) {
			continue;
		}
		int saved_rotation = tetris->rotation;
		switch (input.keys[i].keysym) {
		case XKB_KEY_x:
			tetris->rotation -= 1;
			if (tetris->rotation < 0)
				tetris->rotation = ROTS_COUNT - 1;

			if (tetris_HasCollision(tetris)) {
				tetris->rotation = saved_rotation;
			} else {
				state->redraw = true;
			}
			break;
		case XKB_KEY_z:
			tetris->rotation = (tetris->rotation + 1) % ROTS_COUNT;
			if (tetris_HasCollision(tetris)) {
				tetris->rotation = saved_rotation;
			} else {
				state->redraw = true;
			}
			break;
		case XKB_KEY_r:
			tetris_Init(state);
			tetris->lost = false;
			return;
		case XKB_KEY_Left: // fallthrough
		case XKB_KEY_h:
			dx = -1;
			break;
		case XKB_KEY_Right: // fallthrough
		case XKB_KEY_l:
			dx = +1;
			break;
		case XKB_KEY_Down: // fallthrough
		case XKB_KEY_j:
			down = true;
			break;
		}
	}
	if (tetris->lost)
		return;

	if (dx != 0) {
		int saved_x = tetris->curPos.x;
		tetris->curPos.x += dx;
		struct Vec2 points[4];
		tetris_CurPiecePoints(tetris, points);
		int dx = 0;
		for (int i = 0; i < 4; i++) {
			if (points[i].x >= TETRIS_WIDTH) {
				int new_dx = TETRIS_WIDTH - points[i].x - 1;
				if (new_dx < dx) {
					dx = new_dx;
				}
			} else if (points[i].x < 0) {
				int new_dx = -points[i].x;
				if (new_dx > dx) {
					dx = new_dx;
				}
			} else if (points[i].y >= 0 && tetris->board[points[i].y][points[i].x] != 0) {
				dx = saved_x - tetris->curPos.x;
				break;
			}
		}
		tetris->curPos.x += dx;

		state->redraw = true;
	}

	static double accumTime = 0;
	accumTime += dt;

	double timeInterval = 0.7;
	if (down) {
		timeInterval = 0.05;
	}

	if (accumTime > timeInterval) {
		accumTime = 0;
		state->redraw = true;

		tetris->curPos.y += 1;

		struct Vec2 points[4];
		tetris_CurPiecePoints(tetris, points);

		int color = (tetris->curPiece + 1) % COLORS_COUNT;
		for (int i = 0; i < 4; i++) {
			if (points[i].y < 0 )
				continue;
			if (points[i].y == TETRIS_HEIGHT ||
					tetris->board[points[i].y][points[i].x] != 0) {
				for (int i = 0; i < 4; i++) {
					if (points[i].y > 0) {
						tetris->board[points[i].y-1][points[i].x] = color;
					}
				}
				tetris_RemoveFilledLines(tetris);

				tetris->curPiece = tetris->nextPiece;
				tetris->rotation = tetris->nextRotation;
				tetris->nextPiece = rand() % TPIECES_COUNT;
				tetris->nextRotation = rand() % ROTS_COUNT;

				tetris->curPos = (struct Vec2){
					.x = rand() % TETRIS_WIDTH,
					.y = 0,
				};
				tetris_CurPiecePoints(tetris, points);
				int dx = 0;
				for (int i = 0; i < 4; i++) {
					if (points[i].x >= TETRIS_WIDTH) {
						int new_dx = TETRIS_WIDTH - points[i].x - 1;
						if (new_dx < dx) {
							dx = new_dx;
						}
					} else if (points[i].x < 0) {
						int new_dx = -points[i].x;
						if (new_dx > dx) {
							dx = new_dx;
						}
					} else if (points[i].y < 0) {
						continue;
					} else if (tetris->board[points[i].y][points[i].x] > 0) {
						tetris->lost = true;
						return;
					}
				}
				tetris->curPos.x += dx;
				return;
			}
		}
	}
}

static void
tetris_UpdateDraw(struct State *state, struct Input input, double dt)
{
	struct Tetris *tetris = &state->tetris;

	tetris_Update(state, input, dt);

	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	struct Color bg = state->bg;
	struct Color fg = state->fg;

	cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[COLOR_BLACK]));
	cairo_paint(cr);

	int xoff = 0;
	int yoff = 0;
	int width = TETRIS_WIDTH;
	int height = TETRIS_HEIGHT;
	float scale = 1;

	int info_width_blk = 4; // "blocks"
	int info_padding = 5;  // pixels

	scaleAndCenterRect(buf->width - info_padding, buf->height,
			width + info_width_blk, height,
			&xoff, &yoff, &scale);

	int info_width = info_width_blk * scale;

	cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
	cairo_rectangle(cr, xoff, yoff, width * scale, height * scale);
	cairo_fill(cr);


	// info bar
	{
		int start_x = xoff + width * scale + info_padding;
		int start_y = yoff;
		int w = info_width;
		int h = height * scale;
		cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
		cairo_rectangle(cr, start_x, start_y, w, h);
		cairo_fill(cr);

		enum TetrisPiece saved_piece = tetris->curPiece;
		enum Rotation saved_rot = tetris->rotation;
		struct Vec2 saved_pos = tetris->curPos;

		tetris->curPiece = tetris->nextPiece;
		tetris->rotation = tetris->nextRotation;
		tetris->curPos.x = 0;
		tetris->curPos.y = 0;

		struct Vec2 points[4];
		tetris_CurPiecePoints(tetris, points);
		int color = (tetris->curPiece + 1) % COLORS_COUNT;

		tetris->curPiece = saved_piece;
		tetris->rotation = saved_rot;
		tetris->curPos   = saved_pos;

		int board[4][4] = {0};
		int min_y = 0;
		int min_x = 0;
		for (int i = 0; i < 4; i++) {
			if (points[i].x < min_x) {
				min_x = points[i].x;
			}
			if (points[i].y < min_y) {
				min_y = points[i].y;
			}
		}

		min_x = -min_x;
		min_y = -min_y;
		for (int i = 0; i < 4; i++) {
			board[points[i].y + min_y][points[i].x + min_x] = color;
		}

		double sz = ceil((double)w / 6.0); // 4 + 2 padding
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++) {
				struct Color c = state->colors[board[y][x]];
				if (board[y][x] == 0) {
					c = bg;
				}
				cairo_set_source_rgba(cr, COLOR_CAIRO(c));
				cairo_rectangle(cr,
						ceil((double)start_x + (x+1) * sz),
						ceil((double)start_y + (y+1) * sz),
						sz, sz);
				cairo_fill(cr);
			}
		}
	}

	double sz = ceil(scale);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			if (tetris->board[y][x] <= 0) {
				continue;
			}
			assert(tetris->board[y][x] < COLORS_COUNT);
			cairo_set_source_rgba(cr,
					COLOR_CAIRO(state->colors[tetris->board[y][x]]));
			cairo_rectangle(cr,
					ceil((double)x * (double)scale + (double)xoff),
					ceil((double)y * (double)scale + (double)yoff),
					sz, sz);
			cairo_fill(cr);
		}
	}

	int color = (tetris->curPiece + 1) % COLORS_COUNT;
	struct Vec2 points[4];
	tetris_CurPiecePoints(tetris, points);
	cairo_set_source_rgba(cr,
			COLOR_CAIRO(state->colors[color]));
	for (int i = 0; i < 4; i++) {
		cairo_rectangle(cr,
				ceil((double)points[i].x * (double)scale + (double)xoff),
				ceil((double)points[i].y * (double)scale + (double)yoff),
				sz, sz);
		cairo_fill(cr);
	}

	if (tetris->lost) {
		char *text = "you lose";
		cairo_text_extents_t ext;
		cairo_set_font_size(cr, scale * 5);
		cairo_text_extents(cr, text, &ext);
		int ty = buf->height/2;
		int tx = buf->width/2 - ext.width/2;

		cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
		cairo_rectangle (cr, tx + ext.x_bearing, ty + ext.y_bearing,
				ext.width, ext.height);
		cairo_fill(cr);

		cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
		cairo_move_to(cr, tx, ty);
		cairo_show_text(cr, text);
	}
}

static void
tetris_Init(struct State *state)
{
	struct Tetris *tetris = &state->tetris;
	struct Vec2 points[4];
	int dx = 0;

	memset(tetris, 0, sizeof(*tetris));
	tetris->nextPiece    = rand() % TPIECES_COUNT;
	tetris->curPiece     = rand() % TPIECES_COUNT;
	tetris->nextRotation = rand() % ROTS_COUNT;
	tetris->rotation     = rand() % ROTS_COUNT;
	state->tetris.curPos = (struct Vec2){
		.x = rand() % TETRIS_WIDTH,
		.y = 0,
	};

	tetris_CurPiecePoints(tetris, points);
	for (int i = 0; i < 4; i++) {
		if (points[i].x >= TETRIS_WIDTH) {
			int new_dx = TETRIS_WIDTH - points[i].x - 1;
			if (new_dx < dx) {
				dx = new_dx;
			}
		} else if (points[i].x < 0) {
			int new_dx = -points[i].x;
			if (new_dx > dx) {
				dx = new_dx;
			}
		}
	}
	tetris->curPos.x += dx;
}

static void
tetris_Fini(struct State *state)
{
	// noop
}

static void
tetris_Preview(struct State *state, int x, int y, int size)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	struct Color fg = state->fg;
	double blockSize = size * 0.1;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));

	cairo_rectangle(cr, x, y, blockSize*3, blockSize);
	cairo_fill(cr);
	cairo_rectangle(cr, x + blockSize, y+blockSize, blockSize, blockSize);
	cairo_fill(cr);

	cairo_rectangle(cr, x + blockSize*4, y + blockSize*4, blockSize*2, blockSize);
	cairo_fill(cr);
	cairo_rectangle(cr, x + blockSize*5, y + blockSize*2, blockSize, blockSize*2);
	cairo_fill(cr);

	cairo_rectangle(cr, x + blockSize*7, y + blockSize*7, blockSize, blockSize*2);
	cairo_fill(cr);
	cairo_rectangle(cr, x + blockSize*8, y + blockSize*6, blockSize, blockSize*2);
	cairo_fill(cr);

	cairo_rectangle(cr, x + blockSize, y + blockSize*8, blockSize*2, blockSize*2);
	cairo_fill(cr);
}

static bool
car_race_Update(struct State *state, struct Input input, double dt)
{
	struct CarRace *car = &state->car;
	enum {
		CAR_FORWARD,
		CAR_BACKWARD,
		CAR_LEFT,
		CAR_RIGHT,
		CAR_BREAK,
		CAR_KEYS_COUNT,
	};
	static bool pressed_keys[CAR_KEYS_COUNT] = {0};
	static bool pause = false;

#define CAR_VELOCITY 2.0
#define CAR_TURN_ANGLE 0.05
	for (size_t i = 0; i < input.keys_len; i++) {
		bool b = input.keys[i].state != KEY_RELEASED;
		switch (input.keys[i].keysym) {
		case XKB_KEY_p:
			if (input.keys[i].state == KEY_PRESSED)
				pause = !pause;
			break;
		case XKB_KEY_k:
			pressed_keys[CAR_FORWARD] = b;
			break;
		case XKB_KEY_j:
			pressed_keys[CAR_BACKWARD] = b;
			break;
		case XKB_KEY_h:
			pressed_keys[CAR_LEFT] = b;
			break;
		case XKB_KEY_l:
			pressed_keys[CAR_RIGHT] = b;
			break;
		case XKB_KEY_space:
			pressed_keys[CAR_BREAK] = b;
			break;
		}
	}
	if (pause) {
		return false;
	}

	if (!pressed_keys[CAR_BREAK]) {
		if (pressed_keys[CAR_FORWARD]) {
			car->accel = CAR_VELOCITY;
		} else if (pressed_keys[CAR_BACKWARD]) {
			car->accel = -CAR_VELOCITY;
		}
	}

	if (pressed_keys[CAR_LEFT]) {
		car->angle += -CAR_TURN_ANGLE;
	} else if (pressed_keys[CAR_RIGHT]) {
		car->angle += CAR_TURN_ANGLE;
	}

	if (pressed_keys[CAR_BREAK]) {
		car->accel *= 0.4;
	} else {
		car->accel *= 0.8;
	}
	struct FVec2 prevPos = car->carPos;

	struct FVec2 p = rotate(CAR_LENGTH, 0, car->angle);

	car->carPos.x += p.x * car->velocity * dt;
	car->carPos.y += p.y * car->velocity * dt;
	car->velocity += car->accel * 0.9;
	car->velocity *= 0.9;

	if (car->carPos.x < 0) {
		car->carPos.x = 0;
		car->velocity = 0;
		car->accel = 0;
	} else if (car->carPos.x > CAR_TRACK_SIZE) {
		car->carPos.x = CAR_TRACK_SIZE;
		car->velocity = 0;
		car->accel = 0;
	}
	if (car->carPos.y < 0) {
		car->carPos.y = 0;
		car->velocity = 0;
		car->accel = 0;
	} else if (car->carPos.y > CAR_TRACK_SIZE) {
		car->carPos.y = CAR_TRACK_SIZE;
		car->velocity = 0;
		car->accel = 0;
	}

	if (car->carPos.y < CAR_TRACK_SIZE && car->carPos.x < CAR_TRACK_SIZE &&
			car->track[(int)car->carPos.y][(int)car->carPos.x]) {
		// Not perfect, but good enough for now.
		car->carPos = prevPos;
	}

	struct FLine carLine = {
		.points = {
			[0] = car->carPos,
			[1] = {
				car->carPos.x + (double)CAR_LENGTH * cos(car->angle),
				car->carPos.y + (double)CAR_LENGTH * sin(car->angle),
			},
		},
	};

	static bool passed_starting_line = false;

	if (car->lap < car->max_laps && hasIntersectionFLine(car->startingLine, carLine)) {
		if (passed_starting_line) {
			car->lap++;
		}
		passed_starting_line = false;
	} else {
		passed_starting_line = true;
	}

	if (prevPos.x == car->carPos.x && prevPos.y == car->carPos.y &&
			!pressed_keys[CAR_LEFT] && !pressed_keys[CAR_RIGHT]) {
		return false;
	}
	return true;
}

static void
car_race_UpdateDraw(struct State *state, struct Input input, double dt)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	struct Color bg = state->bg;
	struct Color fg = state->fg;
	struct CarRace *car = &state->car;

	bool redraw = car_race_Update(state, input, dt);
	if (!state->redraw && !redraw)
		return;
	state->redraw = true;

	cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
	cairo_paint(cr);

	int xoff = 0, yoff = 0;
	float scale = 1;
	scaleAndCenterRect(buf->width, buf->height, CAR_TRACK_SIZE, CAR_TRACK_SIZE,
			&xoff, &yoff, &scale);

	if (car->track_surface != NULL) {
		// XXX: We could still optimize this up by saving the scaled
		// version, but I'm not sure if it's worth it.
		cairo_save(cr);
		cairo_scale(cr, scale, scale);
		cairo_set_source_surface(cr, car->track_surface, xoff/scale, yoff/scale);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
		cairo_paint(cr);
		cairo_restore(cr);
	} else {
		float ceilScale = ceilf(scale);
		for (int y = 0; y < CAR_TRACK_SIZE; y++) {
			for (int x = 0; x < CAR_TRACK_SIZE; x++) {
				int color = car->track[y][x] % COLORS_COUNT;
				cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[color]));
				cairo_rectangle(cr, xoff + x*scale, yoff + y*scale,
						ceilScale, ceilScale);
				cairo_fill(cr);
			}
		}
	}

	{
		cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
		cairo_set_line_width(cr, scale);
		float x1 = car->startingLine.points[0].x;
		float y1 = car->startingLine.points[0].y;
		float x2 = car->startingLine.points[1].x;
		float y2 = car->startingLine.points[1].y;

		cairo_move_to(cr,
				xoff + x1 * scale,
				yoff + y1 * scale);
		cairo_line_to(cr,
				xoff + x2 * scale,
				yoff + y2 * scale);
		cairo_stroke(cr);
	}

	struct FVec2 p = rotate(CAR_LENGTH, 0, car->angle);
	p.x += car->carPos.x;
	p.y += car->carPos.y;
	double x1 = xoff + car->carPos.x*scale;
	double y1 = yoff + car->carPos.y*scale;
	double x2 = xoff + p.x*scale;
	double y2 = yoff + p.y*scale;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
	cairo_set_line_width(cr, scale * (float)CAR_WIDTH);
	cairo_move_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	cairo_stroke(cr);

	cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
	cairo_arc(cr, lerp(x1, x2, 0.8), lerp(y1, y2, 0.8), scale * 0.4, 0, PI * 2);
	cairo_fill(cr);

	{
		cairo_text_extents_t ext;
		double fontSize = scale * 3.2;
		char buf[128];
		char *text = buf;
		snprintf(buf, sizeof(buf), "%d/%d", car->lap, car->max_laps);

		cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
		cairo_set_font_size(cr, fontSize);
		cairo_text_extents(cr, text, &ext);
		int ty = yoff + fontSize;
		int tx = xoff + ext.width/2;
		cairo_move_to(cr, tx, ty);
		cairo_show_text(cr, text);
	}
}

static void
car_race_Init(struct State *state)
{
	struct CarRace *car = &state->car;
	memset(car, 0, sizeof(*car));

	int road_radius = (CAR_TRACK_SIZE-1)/2;
	int grass_radius = road_radius*0.6;

	car->max_laps = 3;
	car->lap = 0;

	int center_y = CAR_TRACK_SIZE/2;
	int center_x = CAR_TRACK_SIZE/2;
	for (int y = 0; y < CAR_TRACK_SIZE; y++) {
		for (int x = 0; x < CAR_TRACK_SIZE; x++) {
			int cx = x - center_x;
			int cy = y - center_y;

			if (cx*cx + cy * cy <= grass_radius * grass_radius) {
				car->track[y][x] = COLOR_GREEN;
			} else if (cx*cx + cy * cy <= road_radius * road_radius) {
				car->track[y][x] = COLOR_BLACK;
			} else {
				car->track[y][x] = COLOR_BLUE;
			}
		}
	}
	{
		int x = 0;
		while (x < center_x && car->track[center_y][x])
			x++;
		int start = x;
		while (x < center_x && !car->track[center_y][x])
			x++;

		car->startingLine = (struct FLine){
			.points = {
				[0] = {
					(float)start,
					(float)center_y + 0.5,
				},
				[1] = {
					x,
					(float)center_y + 0.5,
				},
			},
		};
	}
	car->carPos.x = lerpf(
				car->startingLine.points[0].x,
				car->startingLine.points[1].x,
				0.5);
	car->carPos.y = car->startingLine.points[0].y + CAR_LENGTH + 1;
	car->angle = 3 * PI / 2;

	// XXX: this is kind of stupid, but it works
	static cairo_surface_t *surf = NULL;
	if (surf == NULL)
		surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
				CAR_TRACK_SIZE, CAR_TRACK_SIZE);
	cairo_status_t status = cairo_surface_status(surf);
	if (status != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "car_race: cairo: %s\n", cairo_status_to_string(status));
		car->track_surface = NULL;
		return;
	}
	car->track_surface = surf;
	cairo_t *cr = cairo_create(surf);
	for (int y = 0; y < CAR_TRACK_SIZE; y++) {
		for (int x = 0; x < CAR_TRACK_SIZE; x++) {
			int color = car->track[y][x] % COLORS_COUNT;
			cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[color]));
			cairo_rectangle(cr, x, y, 1, 1);
			cairo_fill(cr);
		}
	}
	cairo_destroy(cr);
}

static void
car_race_Fini(struct State *state)
{
	// noop
}

static void
car_race_Preview(struct State *state, int x, int y, int size)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	//struct Color bg = state->bg;
	struct Color fg = state->fg;
	double blockSize = size * 0.1;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
	cairo_rectangle(cr,
			x + size/2 - blockSize,
			y + size/2 - blockSize/2,
			blockSize*2,
			blockSize);
	cairo_fill(cr);

	double tireSize = blockSize*0.4;
	cairo_rectangle(cr,
			x + size/2 - blockSize + tireSize/2,
			y + size/2 - blockSize/2 - tireSize/2,
			tireSize,
			tireSize);
	cairo_fill(cr);
	cairo_rectangle(cr,
			x + size/2 - blockSize + tireSize/2,
			y + size/2 + blockSize/2 - tireSize/2,
			tireSize,
			tireSize);
	cairo_fill(cr);

	cairo_rectangle(cr,
			x + size/2 + blockSize/2 - tireSize/2,
			y + size/2 - blockSize/2 - tireSize/2,
			tireSize,
			tireSize);
	cairo_fill(cr);
	cairo_rectangle(cr,
			x + size/2 + blockSize/2 - tireSize/2,
			y + size/2 + blockSize/2 - tireSize/2,
			tireSize,
			tireSize);
	cairo_fill(cr);

	cairo_rectangle(cr, x, y + size/2 - blockSize, size, tireSize/2);
	cairo_fill(cr);
	cairo_rectangle(cr, x, y + size/2 + blockSize, size, tireSize/2);
	cairo_fill(cr);
}

static void
breakout_UpdateDraw(struct State *state, struct Input input, double dt)
{
	struct Breakout *br = &state->breakout;
	struct Buffer *buf = &state->buffer;
	struct Color fg = state->fg;
	struct Color bg = state->bg;
	cairo_t *cr = buf->cr;

	int xoff = 0, yoff = 0;
	float scale = 0;
	scaleAndCenterRect(buf->width, buf->height,
			BREAKOUT_WIDTH, BREAKOUT_HEIGHT,
			&xoff, &yoff, &scale);

	static bool left = false;
	static bool right = false;
	for (size_t i = 0; i < input.keys_len; i++) {
		switch (input.keys[i].keysym) {
		case XKB_KEY_h:
			left = input.keys[i].state != KEY_RELEASED;
			break;
		case XKB_KEY_l:
			right = input.keys[i].state != KEY_RELEASED;
			break;
		case XKB_KEY_space:
			if (!br->move_ball && input.keys[i].state == KEY_PRESSED) {
				br->ball_speed = BREAKOUT_BALL_SPEED;
				br->ball_velocity.x = BREAKOUT_BALL_SPEED/2;
				br->ball_velocity.y = -BREAKOUT_BALL_SPEED;
				br->move_ball = true;
			}
			break;
		}
	}

	if (left)
		br->x_pos -= BREAKOUT_PLAYER_SPEED;

	if (right)
		br->x_pos += BREAKOUT_PLAYER_SPEED;

	if (br->x_pos < 0)
		br->x_pos = 0;
	if (br->x_pos + BREAKOUT_PLAYER_WIDTH >= BREAKOUT_WIDTH)
		br->x_pos = BREAKOUT_WIDTH-BREAKOUT_PLAYER_WIDTH;

	if (br->move_ball) {
		br->ball_pos.x += br->ball_velocity.x;
		br->ball_pos.y += br->ball_velocity.y;
		if (br->ball_pos.y >= BREAKOUT_HEIGHT) {
			br->ball_velocity.y *= -1;
			br->ball_pos.y = BREAKOUT_HEIGHT-1;
		}

		if (br->ball_pos.y < 0) {
			br->ball_velocity.y *= -1;
			br->ball_pos.y = 0;
		}

		if (br->ball_pos.x >= BREAKOUT_WIDTH) {
			br->ball_velocity.x *= -1;
			br->ball_pos.x = BREAKOUT_WIDTH-1;
		}
		if (br->ball_pos.x < 0) {
			br->ball_velocity.x *= -1;
			br->ball_pos.x = 0;
		}

		struct FRect ball = {
			.x = br->ball_pos.x - BREAKOUT_BALL_RADIUS/2,
			.y = br->ball_pos.y - BREAKOUT_BALL_RADIUS/2,
			.w = BREAKOUT_BALL_RADIUS,
			.h = BREAKOUT_BALL_RADIUS,
		};
		struct FRect player = {
			.x = br->x_pos,
			.y = BREAKOUT_PLAYER_Y,
			.w = BREAKOUT_PLAYER_WIDTH,
			.h = BREAKOUT_PLAYER_HEIGHT,
		};
		if (hasIntersectionF(ball, player)) {
			float x = ball.x - player.x;
			br->ball_velocity.x = br->ball_speed *
				// in range of -1 to 1 depending on ball
				// position relative to center of paddle.
				2.0 * ((x / BREAKOUT_PLAYER_WIDTH) - 0.5);

			float y = ball.y - player.y;
			if (fabsf(y) < BREAKOUT_PLAYER_HEIGHT) {
				br->ball_velocity.y = -br->ball_speed;
			}

			br->ball_speed += 0.05;
			if (br->ball_speed > BREAKOUT_BALL_SPEED_MAX) {
				br->ball_speed = BREAKOUT_BALL_SPEED_MAX;
			}
		} else {
			float w = BREAKOUT_BARS_WIDTH + BREAKOUT_BARS_PADDING;
			float h = BREAKOUT_BARS_HEIGHT + BREAKOUT_BARS_PADDING;
			float bars_xoff = BREAKOUT_WIDTH / 2.0 -
				BREAKOUT_BARS_TOTAL_WIDTH / 2.0;

			// guess the bar that might be affected.
			int x = (int)((br->ball_pos.x - bars_xoff) / w);
			int y = (int)(br->ball_pos.y / h);
			// TODO: it's possible that surrounding bars could
			// still be hit by the ball.
			if (0 <= y && y < BREAKOUT_BARS_ROWS &&
					0 <= x && x < BREAKOUT_BARS_COLS &&
					!br->bars_destroyed[y][x]) {
				struct FRect bar = {
					.x = bars_xoff + x * w,
					.y = y * h,
					.w = BREAKOUT_BARS_WIDTH,
					.h = BREAKOUT_BARS_HEIGHT,
				};

				if (hasIntersectionF(bar, ball)) {
					br->bars_destroyed[y][x] = true;
					if (ball.x <= bar.x &&
							bar.x <= ball.x + ball.w) {
						br->ball_velocity.x *= -1;
					} else if (ball.x <= bar.x + bar.w &&
							bar.x + bar.w <= ball.x + ball.w) {
						br->ball_velocity.x *= -1;
					}
					if (ball.y <= bar.y &&
							bar.y <= ball.y + ball.h) {
						br->ball_velocity.y *= -1;
					} else if (ball.y <= bar.y + bar.h &&
							bar.y + bar.h <= ball.y + ball.h) {
						br->ball_velocity.y *= -1;
					}
				}
			}
		}
	} else {
		br->ball_pos.x = br->x_pos + BREAKOUT_PLAYER_WIDTH/2;
		br->ball_pos.y = BREAKOUT_PLAYER_Y - BREAKOUT_BALL_RADIUS;
	}

	state->redraw = true;
	cairo_set_source_rgba(cr, lerpf(bg.r, fg.r, 0.1),
			lerpf(bg.g, fg.g, 0.1), lerpf(bg.b, fg.b, 0.1),
			bg.a);
	cairo_paint(cr);

	cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
	cairo_rectangle(cr, xoff, yoff, BREAKOUT_WIDTH*scale, BREAKOUT_HEIGHT*scale);
	cairo_fill(cr);

	float bars_xoff = BREAKOUT_WIDTH / 2.0 - BREAKOUT_BARS_TOTAL_WIDTH / 2.0;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
	for (int y = 0; y < BREAKOUT_BARS_ROWS; y++) {
		for (int x = 0; x < BREAKOUT_BARS_COLS; x++) {
			if (br->bars_destroyed[y][x]) {
				continue;
			}
			//int color = (y * BREAKOUT_BARS_COLS + x) % COLORS_COUNT;
			//cairo_set_source_rgba(cr, COLOR_CAIRO(state->colors[color]));
			float w = BREAKOUT_BARS_WIDTH + BREAKOUT_BARS_PADDING;
			float h = BREAKOUT_BARS_HEIGHT + BREAKOUT_BARS_PADDING;
			cairo_rectangle(cr,
					xoff + scale * (bars_xoff + x * w),
					yoff + scale * y * h,
					BREAKOUT_BARS_WIDTH * scale,
					BREAKOUT_BARS_HEIGHT * scale);
			cairo_fill(cr);
		}
	}
	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));

	cairo_arc(cr,
			xoff + scale * br->ball_pos.x,
			yoff + scale * br->ball_pos.y,
			scale * BREAKOUT_BALL_RADIUS, 0, PI * 2);

	cairo_rectangle(cr,
			xoff + scale * br->x_pos,
			yoff + scale * BREAKOUT_PLAYER_Y,
			scale * BREAKOUT_PLAYER_WIDTH,
			scale * BREAKOUT_PLAYER_HEIGHT);
	cairo_fill(cr);
}

static void
breakout_Init(struct State *state)
{
	struct Breakout *br = &state->breakout;
	memset(br, 0, sizeof(*br));

	br->x_pos = BREAKOUT_WIDTH/2 - BREAKOUT_PLAYER_WIDTH/2;
}

static void
breakout_Fini(struct State *state)
{
	// noop
}

static void
breakout_Preview(struct State *state, int x, int y, int size)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;
	struct Color fg = state->fg;
	double paddleSize = size * 0.05;

	cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
	cairo_rectangle(cr,
			x + size/2 - paddleSize*3,
			y + (double)size - paddleSize*2,
			paddleSize*6,
			paddleSize);
	cairo_fill(cr);

	int sx = x;
	int sy = y;
	double bar_w = size / 10.0;
	double bar_h = size / 25.0;
	for (int y = 1; y < 5; y++) {
		for (int x = 1; x < 9; x++) {
			cairo_rectangle(cr,
					sx + x * bar_w,
					sy + y * bar_h,
					bar_w*0.8,
					bar_h*0.8);
			cairo_fill(cr);
		}
	}

	cairo_arc(cr, x + size * 0.7, y + size * 0.6, (double)size*0.03, 0, PI * 2);
	cairo_fill(cr);
}

static void
selectDraw(struct State *state)
{
	struct Buffer *buf = &state->buffer;
	cairo_t *cr = buf->cr;

	struct Color bg = state->bg;
	struct Color fg = state->fg;
	double fontSize = 50;
	int icon_size = 200;
	int padding = 16;
	int line_width = 4;
	int cellSize = icon_size + padding;

	cairo_set_source_rgba(cr, lerpf(bg.r, fg.r, 0.1),
			lerpf(bg.g, fg.g, 0.1), lerpf(bg.b, fg.b, 0.1),
			bg.a);
	cairo_paint(cr);

	int title_height = fontSize;
	if (title_height > buf->height) {
		title_height = buf->height;
	}
	int height = buf->height - title_height;

	int cols = buf->width / cellSize;
	int margin_w = buf->width % cellSize;
	int rows = height / cellSize;
	int margin_h = height % cellSize;
	int selected = state->sel_scr.selected;

	if (cols >= GAMES_COUNT) {
		rows = 1;
		margin_h = height - cellSize;
		cols = GAMES_COUNT;
		margin_w = buf->width - cellSize * cols;
	}
	if (cols <= 0) {
		margin_w = 0;
		cols = 1;
	}
	if (rows <= 0) {
		margin_h = 0;
		rows = 1;
	}

	state->sel_scr.rows = rows;
	state->sel_scr.cols = cols;

	int start_row = selected / cols;

	int start_x = margin_w / 2;
	int start_y = margin_h / 2 + title_height;

	{
		char *text = games[selected].name;
		cairo_text_extents_t ext;

		cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
		cairo_set_font_size(cr, fontSize);
		cairo_text_extents(cr, text, &ext);
		int ty = title_height;
		int tx = buf->width/2 - ext.width/2;
		cairo_move_to(cr, tx, ty);
		cairo_show_text(cr, text);
	}

	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			int n = (y + start_row) * cols + x;
			if (n >= GAMES_COUNT) {
				goto end_preview_drawing;
			}

			int xoff = start_x + x * cellSize;
			int yoff = start_y + y * cellSize;

			if (n == selected) {
				cairo_set_source_rgba(cr,
						COLOR_CAIRO(fg));

				cairo_set_line_width(cr, line_width);
				cairo_move_to(cr, xoff, yoff);
				cairo_line_to(cr, xoff + cellSize, yoff);
				cairo_line_to(cr, xoff + cellSize, yoff + cellSize);
				cairo_line_to(cr, xoff, yoff + cellSize);
				cairo_line_to(cr, xoff, yoff-line_width/2);
				cairo_stroke(cr);
			}
			xoff += padding/2;
			yoff += padding/2;
			cairo_set_source_rgba(cr, COLOR_CAIRO(bg));
			cairo_rectangle(cr, xoff, yoff, icon_size, icon_size);
			cairo_fill(cr);

			if (games[n].preview == NULL) {
				char *text = games[n].name;
				cairo_text_extents_t ext;

				cairo_set_source_rgba(cr, COLOR_CAIRO(fg));
				cairo_set_font_size(cr, fontSize);
				cairo_text_extents(cr, text, &ext);
				int ty = yoff + cellSize/2;
				int tx = xoff + cellSize/2 - ext.width/2;
				cairo_move_to(cr, tx, ty);
				cairo_show_text(cr, text);
			} else {
				games[n].preview(state, xoff, yoff, icon_size);
			}
		}
	}
end_preview_drawing:
	return;
}

static void
selectHandleKey(struct State *state, xkb_keysym_t key)
{
	bool redraw = true;
	int *sel = &state->sel_scr.selected;
	switch (key) {
	case XKB_KEY_Return:
	case XKB_KEY_space:
		state->sel_scr.enter = true;
		break;
	case XKB_KEY_Left: // fallthrough
	case XKB_KEY_h:
		*sel -= 1;
		if (*sel < 0) {
			*sel = 0;
		}
		break;
	case XKB_KEY_Right: // fallthrough
	case XKB_KEY_l:
		*sel += 1;
		if (*sel >= GAMES_COUNT) {
			*sel = GAMES_COUNT-1;
		}
		break;
	case XKB_KEY_Up: // fallthrough
	case XKB_KEY_k:
		*sel -= state->sel_scr.cols;
		if (*sel < 0) {
			*sel = 0;
		}
		break;
	case XKB_KEY_Down: // fallthrough
	case XKB_KEY_j:
		*sel += state->sel_scr.cols;
		if (*sel >= GAMES_COUNT) {
			*sel = GAMES_COUNT-1;
		}
		break;
	default:
		redraw = state->redraw;
		break;
	}
	state->redraw = redraw;
}

void
selectUpdateDraw(struct State *state, struct Input input, double dt)
{
	for (size_t i = 0; i < input.keys_len; i++) {
		if (input.keys[i].state == KEY_PRESSED) {
			selectHandleKey(state, input.keys[i].keysym);
		}
	}

	if (state->sel_scr.enter) {
		int s = state->sel_scr.selected;
		if (s >= GAMES_COUNT) {
			s = GAMES_COUNT-1;
		}
		if (s < 0) {
			s = 0;
		}
		state->cur_game = s;
		state->sel_scr.enter = false;
		games[state->cur_game].init(state);
		state->redraw = true;
	}
	selectDraw(state);
}
