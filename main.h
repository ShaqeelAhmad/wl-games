#define APPEND(s, v) \
do { \
	if ((s).len + 1 >= (s).cap) { \
		if ((s).cap == 0) { \
			(s).cap = 16; \
		} else if ((s).cap < 8192) { \
			(s).cap *= 2; \
		} else { \
			(s).cap += 128; \
		} \
		(s).data = erealloc((s).data, sizeof(*(s).data) * (s).cap); \
	} \
	(s).data[(s).len] = v; \
	(s).len++; \
} while(0)

#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))
#define PI 3.14159265358979323846264338327950288419716939937510582097494459

enum Game {
	GAME_SNAKE,
	GAME_SUDOKU,
	GAME_PONG,
	GAME_TETRIS,
	GAME_CAR_RACE,
	GAME_BREAKOUT,
	// GAME_CHESS,
	// GAME_MINESWEEPER,
	// GAME_PACMAN,
	GAMES_COUNT,
};

enum Direction {
	DIR_UP,
	DIR_DOWN,
	DIR_LEFT,
	DIR_RIGHT,
};

struct Vec2 {
	int x;
	int y;
};

struct FVec2 {
	float x;
	float y;
};

struct FRect {
	float x;
	float y;
	float w;
	float h;
};

struct Color {
	double r,g,b,a;
};
#define COLOR_CAIRO(c) (c).r, (c).g, (c).b, (c).a

enum {
	COLOR_BLACK,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_YELLOW,
	COLOR_BLUE,
	COLOR_MAGENTA,
	COLOR_CYAN,
	COLOR_WHITE,
	COLORS_COUNT,
};

struct Snake {
	int x;
	int y;
	enum Direction dir;
	enum Direction next_dir;

	int rows;
	int cols;

	struct {
		struct Vec2 *data;
		int len;
		int cap;
	} tails;

	struct Vec2 apple;

	bool pause;
	bool lost;
};

struct SudokuCell {
	bool user_fill;
	bool values[9];
	char value;
};

struct Sudoku {
	int focus_y;
	int focus_x;

	struct SudokuCell board[9][9];
};

#define PONG_WIDTH 600
#define PONG_HEIGHT 400
#define PONG_BALL_DX 80
#define PONG_BALL_MAX_DX 600
#define PONG_BALL_RADIUS 8
#define PONG_PLAYER_X (PONG_WIDTH * 0.1)
#define PONG_PLAYER_WIDTH (PONG_BALL_RADIUS)
#define PONG_PLAYER_HEIGHT (PONG_HEIGHT * 0.25)
#define PONG_PLAYER_DY 200

struct Pong {
	float player1_y;
	float player1_dy;

	bool ai;
	float player2_y;
	float player2_dy;

	int score_left;
	int score_right;

	struct FVec2 ball;
	struct FVec2 ball_velocity;
};

enum TetrisPiece {
	// https://en.wikipedia.org/wiki/Tetromino#Free_tetrominoes
	TPIECE_STRAIGHT,
	TPIECE_SQUARE,
	TPIECE_T,
	TPIECE_L,
	TPIECE_SKEW,
	TPIECES_COUNT,
};
enum Rotation {
	ROT_0,
	ROT_90,
	ROT_180,
	ROT_270,
	ROTS_COUNT,
};

#define TETRIS_HEIGHT 20
#define TETRIS_WIDTH 10
_Static_assert(TETRIS_WIDTH > 4, "TETRIS_WIDTH must be at least 4");

struct Tetris {
	//int score;
	int board[TETRIS_HEIGHT][TETRIS_WIDTH];
	bool lost;

	struct Vec2 curPos;
	enum TetrisPiece curPiece;
	enum Rotation rotation;

	enum TetrisPiece nextPiece;
	enum Rotation nextRotation;
};

#define CAR_TRACK_SIZE 64
#define CAR_LENGTH 2
#define CAR_WIDTH 1
#define CAR_CHECKPOINTS_MAX 16

struct FLine {
	struct FVec2 points[2];
};

struct CarRace {
	struct FVec2 carPos;
	float velocity;
	float accel;
	float angle;

	int lap;
	int max_laps;

	struct FLine startingLine;
	// TODO:
	//struct FLine checkpoints[CAR_CHECKPOINTS_MAX];
	//size_t checkpoints_len;
	//size_t passed_checkpoints;

	cairo_surface_t *track_surface;
	int track[CAR_TRACK_SIZE][CAR_TRACK_SIZE];
};

#define BREAKOUT_BARS_PADDING 2
#define BREAKOUT_BARS_ROWS 5
#define BREAKOUT_BARS_HEIGHT 2
#define BREAKOUT_BARS_COLS 10
#define BREAKOUT_BARS_WIDTH 8
#define BREAKOUT_PLAYER_SPEED 2.0
#define BREAKOUT_BALL_SPEED 0.5
#define BREAKOUT_BALL_SPEED_MAX 1.8
#define BREAKOUT_BALL_RADIUS 1.5
#define BREAKOUT_PLAYER_WIDTH (BREAKOUT_BARS_WIDTH*2)
#define BREAKOUT_PLAYER_HEIGHT BREAKOUT_BARS_HEIGHT

#define BREAKOUT_BARS_TOTAL_HEIGHT (BREAKOUT_BARS_HEIGHT * (BREAKOUT_BARS_ROWS+BREAKOUT_BARS_PADDING))
#define BREAKOUT_HEIGHT (BREAKOUT_BARS_TOTAL_HEIGHT + 80)
#define BREAKOUT_BARS_TOTAL_WIDTH (BREAKOUT_BARS_WIDTH * (BREAKOUT_BARS_COLS+BREAKOUT_BARS_PADDING))
#define BREAKOUT_WIDTH (BREAKOUT_BARS_TOTAL_WIDTH * 1.4)
#define BREAKOUT_PLAYER_Y (BREAKOUT_HEIGHT * 0.9)

struct Breakout {
	float x_pos;
	bool bars_destroyed[BREAKOUT_BARS_ROWS][BREAKOUT_BARS_COLS];

	struct FVec2 ball_pos;
	struct FVec2 ball_velocity;
	float ball_speed;
	bool move_ball;
};

struct Buffer {
	struct wl_buffer *wl_buf;
	int width;
	int height;
	int stride;
	int fd;
	uint8_t *data;
	size_t data_sz;
	cairo_t *cr;
	cairo_surface_t *surf;
};

struct Pointer {
	int x;
	int y;
	struct wl_surface *surface;
	struct wl_cursor_theme *theme;
	struct wl_cursor *cursor;
	int curimg;
	struct wl_cursor_image *image;
	struct wl_pointer *pointer;
	uint32_t serial;
};

#define MAX_INPUT_KEYS 256
enum KeyState {
	KEY_PRESSED,
	KEY_REPEAT,
	KEY_RELEASED,
};

struct Input {
	struct {
		xkb_keysym_t keysym;
		enum KeyState state;
	} keys[MAX_INPUT_KEYS];
	size_t keys_len;
};

struct State {
	struct wl_display *display;
	struct wl_shm *shm;
	struct wl_compositor *compositor;
	struct wl_surface *surface;
	struct wl_registry *registry;
	struct xdg_wm_base *xdg_wm_base;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct wl_seat *seat;
	struct wl_output *output;
	struct zxdg_decoration_manager_v1 *decor_manager;
	struct zxdg_toplevel_decoration_v1 *top_decor;

	struct xkb_state *xkb_state;
	struct xkb_keymap  *xkb_keymap;
	struct xkb_context *xkb_context;
	int32_t repeat_rate;
	int32_t repeat_delay;

	struct Pointer pointer;
	struct Input input;

	struct {
		int fd;
		xkb_keysym_t keysym;
	} repeat_key;

	int width;
	int height;

	struct Buffer buffer;

	struct {
		int selected;
		bool enter;

		int rows;
		int cols;
	} sel_scr;

	int cur_game;
	union {
		struct Snake snake;
		struct Sudoku sudoku;
		struct Pong pong;
		struct Tetris tetris;
		struct CarRace car;
		struct Breakout breakout;
	};

	bool configured;
	bool redraw;
	bool quit;

	struct Color fg;
	struct Color bg;
	struct Color colors[COLORS_COUNT];
};

struct GameInterface {
	char *name;
	void (*updateDraw)(struct State *state, struct Input input, double dt);
	void (*init)(struct State *state);
	void (*fini)(struct State *state);
	void (*preview)(struct State *state, int x, int y, int size);
};

enum {
	CURSOR_ARROW,
	CURSOR_WATCH,
	CURSOR_LEFT,
	CURSOR_RIGHT,

	CURSOR_COUNT,

	CURSOR_NONE = CURSOR_ARROW,
};
