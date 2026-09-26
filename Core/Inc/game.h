/*
 * game.h
 *
 * Original Breakout/Arkanoid-style game for the STM32H750B-DK.
 *
 * This is a drop-in replacement for pacman.h: it exposes the same
 * GameData shape (screen_x/y, touch_x/y, timer, refresh()) that
 * main.c already reads and calls every loop iteration, so main.c
 * does not need to change beyond swapping the #include line.
 *
 * Rendering: STM32 BSP UTIL_LCD_* utility API (stm32_lcd.h) — the
 * same standard ST drawing layer main.c already sets up via
 * UTIL_LCD_SetFuncDriver()/UTIL_LCD_SetLayer(0).
 *
 * Control: touch-only. Paddle X tracks touch_x whenever
 * touch_detected is set; there is no physical-button dependency
 * for gameplay.
 */
#ifndef INC_GAME_H_
#define INC_GAME_H_

#include <stdint.h>
#include "stm32_lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Layout constants ------------------------------------------------- */
#define BRICK_ROWS          5
#define BRICK_COLS          10
#define BRICK_GAP_PX        2
#define BRICK_HEIGHT_PX     14
#define BRICK_TOP_Y_PX      26      /* leaves room for the score bar */

#define PADDLE_WIDTH_PX     64
#define PADDLE_HEIGHT_PX    8
#define PADDLE_BOTTOM_MARGIN_PX 18

#define BALL_RADIUS_PX      4
#define BALL_BASE_SPEED_PX  3       /* pixels moved per physics step */

#define STARTING_LIVES      3
#define FRAME_INTERVAL_MS   16      /* ~60 Hz physics/redraw step */

/* ---- Game objects ------------------------------------------------------ */
typedef struct {
    int32_t x;          /* center x */
    int32_t y;           /* center y */
    int32_t vx;
    int32_t vy;
} Ball;

typedef struct {
    int32_t x;           /* left edge */
    int32_t y;           /* top edge */
} Paddle;

typedef struct {
    uint8_t alive;
    uint32_t color;
} Brick;

typedef struct GameData GameData;

struct GameData {
    /* -- fields main.c populates every loop iteration, matching the
     *    exact shape it already expects from pacman.h's GameData -- */
    uint32_t screen_x;
    uint32_t screen_y;
    uint32_t touch_x;
    uint32_t touch_y;
    uint32_t timer;                  /* HAL_GetTick() snapshot, set by main.c */
    void (*refresh)(GameData *data); /* current-state handler, called by main.c */

    /* -- our own gameplay state -- */
    uint32_t touch_active;           /* mirrors tsState.TouchDetected; main.c
                                         does not currently forward this, see
                                         INTEGRATION notes for the 1-line add */
    uint32_t prev_touch_active;      /* for edge-detecting a fresh tap */
    uint32_t last_step_tick;

    Ball ball;
    Paddle paddle;
    Brick bricks[BRICK_ROWS][BRICK_COLS];
    uint32_t bricks_remaining;
    uint32_t brick_w;                /* computed from screen_x at init */

    uint16_t score;
    uint8_t lives;
    uint8_t level;
};

/* Matches pacman.h's call signature used in main.c:
 *   gameData = GameData_Init(0);
 */
GameData GameData_Init(uint32_t max_score);

/* Matches pacman.h's call signature used in main.c:
 *   GameReset(&gameData);
 */
void GameReset(GameData *data);

#ifdef __cplusplus
}
#endif

#endif /* INC_GAME_H_ */
