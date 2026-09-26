/*
 * game.c
 *
 * Original Breakout/Arkanoid-style game for the STM32H750B-DK.
 * See game.h for the integration contract with main.c.
 *
 * Design notes (so you can explain what you're submitting):
 *  - State machine via a function pointer (data->refresh), same
 *    dispatch pattern main.c's superloop already drives.
 *  - Ball motion is a plain integer velocity vector (vx, vy) reflected
 *    off walls/paddle/bricks — not a fixed-angle lookup table.
 *  - Paddle "English": where the ball hits the paddle (offset from
 *    paddle center) steers the rebound angle, standard arcade-breakout
 *    feel, implemented from scratch below.
 *  - No double buffering is set up by the board template, so drawing
 *    erases only the ball/paddle's *previous* rect before drawing the
 *    new one, instead of clearing the whole screen every frame.
 *  - This file only calls UTIL_LCD_* and reads fields off GameData —
 *    no direct HAL/BSP calls — so it doesn't care which board it's on
 *    beyond what main.c already resolved.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "game.h"
#include "fonts.h"

#define COLOR_BG        UTIL_LCD_COLOR_BLACK
#define COLOR_HUD_BG    UTIL_LCD_COLOR_DARKBLUE
#define COLOR_TEXT      UTIL_LCD_COLOR_WHITE
#define COLOR_PADDLE    UTIL_LCD_COLOR_WHITE
#define COLOR_BALL      UTIL_LCD_COLOR_YELLOW

#define HUD_HEIGHT_PX   20

static const uint32_t kRowColors[BRICK_ROWS] = {
    UTIL_LCD_COLOR_RED,
    UTIL_LCD_COLOR_ORANGE,
    UTIL_LCD_COLOR_YELLOW,
    UTIL_LCD_COLOR_GREEN,
    UTIL_LCD_COLOR_CYAN
};

/* ---- forward declarations of the state handlers ------------------------ */
static void State_Start(GameData *data);
static void State_Play(GameData *data);
static void State_LevelClear(GameData *data);
static void State_GameOver(GameData *data);

/* ---- small helpers ------------------------------------------------------ */

static uint32_t TapEdge(GameData *data)
{
    /* rising edge: was not touched last step, is touched now */
    uint32_t tapped = (data->touch_active && !data->prev_touch_active);
    data->prev_touch_active = data->touch_active;
    return tapped;
}

static void DrawCenteredText(GameData *data, uint32_t y, const char *text, sFONT *font, uint32_t color)
{
    UTIL_LCD_SetFont(font);
    UTIL_LCD_SetTextColor(color);
    UTIL_LCD_SetBackColor(COLOR_BG);
    UTIL_LCD_DisplayStringAt(0, y, (uint8_t *)text, CENTER_MODE);
}

static void InitBricks(GameData *data)
{
    uint32_t playable_w = data->screen_x - (BRICK_COLS + 1) * BRICK_GAP_PX;
    data->brick_w = playable_w / BRICK_COLS;

    for (uint32_t row = 0; row < BRICK_ROWS; row++) {
        for (uint32_t col = 0; col < BRICK_COLS; col++) {
            data->bricks[row][col].alive = 1;
            data->bricks[row][col].color = kRowColors[row];
        }
    }
    data->bricks_remaining = BRICK_ROWS * BRICK_COLS;
}

static void BrickRect(GameData *data, uint32_t row, uint32_t col,
                       int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    *w = data->brick_w;
    *h = BRICK_HEIGHT_PX;
    *x = BRICK_GAP_PX + col * (data->brick_w + BRICK_GAP_PX);
    *y = BRICK_TOP_Y_PX + row * (BRICK_HEIGHT_PX + BRICK_GAP_PX);
}

static void DrawAllBricks(GameData *data)
{
    int32_t x, y, w, h;
    for (uint32_t row = 0; row < BRICK_ROWS; row++) {
        for (uint32_t col = 0; col < BRICK_COLS; col++) {
            if (!data->bricks[row][col].alive) continue;
            BrickRect(data, row, col, &x, &y, &w, &h);
            UTIL_LCD_FillRect(x, y, w, h, data->bricks[row][col].color);
        }
    }
}

static void DrawHud(GameData *data)
{
    char buf[32];
    UTIL_LCD_FillRect(0, 0, data->screen_x, HUD_HEIGHT_PX, COLOR_HUD_BG);
    UTIL_LCD_SetFont(&Font16);
    UTIL_LCD_SetTextColor(COLOR_TEXT);
    UTIL_LCD_SetBackColor(COLOR_HUD_BG);

    snprintf(buf, sizeof(buf), "SCORE %u", data->score);
    UTIL_LCD_DisplayStringAt(6, 2, (uint8_t *)buf, LEFT_MODE);

    snprintf(buf, sizeof(buf), "LIVES %u   LVL %u", data->lives, data->level);
    UTIL_LCD_DisplayStringAt(0, 2, (uint8_t *)buf, RIGHT_MODE);
}

static void ResetBallAndPaddle(GameData *data)
{
    data->paddle.x = (data->screen_x - PADDLE_WIDTH_PX) / 2;
    data->paddle.y = data->screen_y - PADDLE_BOTTOM_MARGIN_PX;

    data->ball.x = data->screen_x / 2;
    data->ball.y = data->paddle.y - BALL_RADIUS_PX - 2;
    data->ball.vx = (rand() % 2 == 0) ? BALL_BASE_SPEED_PX : -BALL_BASE_SPEED_PX;
    data->ball.vy = -BALL_BASE_SPEED_PX;
}

static void ClampPaddleToScreen(GameData *data)
{
    if (data->paddle.x < 0) data->paddle.x = 0;
    if (data->paddle.x > (int32_t)(data->screen_x - PADDLE_WIDTH_PX))
        data->paddle.x = data->screen_x - PADDLE_WIDTH_PX;
}

/* ---- collision handling -------------------------------------------------- */

static void ReflectOffWalls(GameData *data)
{
    Ball *b = &data->ball;

    if (b->x - BALL_RADIUS_PX <= 0) {
        b->x = BALL_RADIUS_PX;
        b->vx = -b->vx;
    } else if (b->x + BALL_RADIUS_PX >= (int32_t)data->screen_x) {
        b->x = data->screen_x - BALL_RADIUS_PX;
        b->vx = -b->vx;
    }

    if (b->y - BALL_RADIUS_PX <= (int32_t)HUD_HEIGHT_PX) {
        b->y = HUD_HEIGHT_PX + BALL_RADIUS_PX;
        b->vy = -b->vy;
    }
}

static uint32_t TryPaddleBounce(GameData *data)
{
    Ball *b = &data->ball;
    Paddle *p = &data->paddle;

    if (b->vy <= 0) return 0; /* only catch on the way down */

    int32_t ball_bottom = b->y + BALL_RADIUS_PX;
    int32_t paddle_top = p->y;

    if (ball_bottom < paddle_top || ball_bottom > paddle_top + PADDLE_HEIGHT_PX + 4)
        return 0;
    if (b->x + BALL_RADIUS_PX < p->x || b->x - BALL_RADIUS_PX > p->x + PADDLE_WIDTH_PX)
        return 0;

    /* Offset from paddle center steers the rebound ("English") */
    int32_t paddle_center = p->x + PADDLE_WIDTH_PX / 2;
    int32_t offset = b->x - paddle_center;              /* -32..+32 roughly */
    b->vx = offset / 6;
    if (b->vx == 0) b->vx = (rand() % 2 == 0) ? 1 : -1;
    b->vy = -BALL_BASE_SPEED_PX;
    b->y = paddle_top - BALL_RADIUS_PX;
    return 1;
}

/* Returns 1 if a brick was hit this step (only ever break one per frame) */
static uint32_t TryBrickBounce(GameData *data)
{
    Ball *b = &data->ball;
    int32_t x, y, w, h;

    for (uint32_t row = 0; row < BRICK_ROWS; row++) {
        for (uint32_t col = 0; col < BRICK_COLS; col++) {
            if (!data->bricks[row][col].alive) continue;
            BrickRect(data, row, col, &x, &y, &w, &h);

            int32_t closest_x = b->x;
            if (closest_x < x) closest_x = x;
            if (closest_x > x + w) closest_x = x + w;
            int32_t closest_y = b->y;
            if (closest_y < y) closest_y = y;
            if (closest_y > y + h) closest_y = y + h;

            int32_t dx = b->x - closest_x;
            int32_t dy = b->y - closest_y;
            if ((dx * dx + dy * dy) > (int32_t)(BALL_RADIUS_PX * BALL_RADIUS_PX))
                continue; /* no overlap */

            /* Hit: kill brick, score, erase it, reflect on whichever
             * axis has the smaller penetration. */
            data->bricks[row][col].alive = 0;
            data->bricks_remaining--;
            data->score += (BRICK_ROWS - row) * 10;
            UTIL_LCD_FillRect(x, y, w, h, COLOR_BG);

            int32_t pen_x = (dx == 0) ? (BALL_RADIUS_PX) : abs(dx);
            int32_t pen_y = (dy == 0) ? (BALL_RADIUS_PX) : abs(dy);
            if (pen_x < pen_y) b->vx = -b->vx;
            else b->vy = -b->vy;

            return 1;
        }
    }
    return 0;
}

/* ---- state: START -------------------------------------------------------- */

static void State_Start(GameData *data)
{
    /* one-time draw of the title screen; only redraw on entry, not every call */
    static uint32_t drawn = 0;
    if (!drawn) {
        UTIL_LCD_Clear(COLOR_BG);
        DrawCenteredText(data, data->screen_y / 2 - 30, "BREAKOUT", &Font24, COLOR_TEXT);
        DrawCenteredText(data, data->screen_y / 2 + 10, "TAP TO START", &Font16, UTIL_LCD_COLOR_LIGHTGRAY);
        drawn = 1;
    }

    if (TapEdge(data)) {
        drawn = 0; /* reset for next time we land on this state */
        data->score = 0;
        data->level = 1;
        data->lives = STARTING_LIVES;
        InitBricks(data);
        ResetBallAndPaddle(data);
        UTIL_LCD_Clear(COLOR_BG);
        DrawHud(data);
        DrawAllBricks(data);
        data->refresh = State_Play;
    }
}

/* ---- state: PLAY ----------------------------------------------------------- */

static void State_Play(GameData *data)
{
    /* frame pacing: only step physics ~60 times/sec */
    if (data->timer - data->last_step_tick < FRAME_INTERVAL_MS) {
        TapEdge(data); /* still track edges so a mid-frame tap isn't lost */
        return;
    }
    data->last_step_tick = data->timer;
    TapEdge(data);

    /* --- paddle follows touch --- */
    int32_t old_paddle_x = data->paddle.x;
    if (data->touch_active) {
        data->paddle.x = (int32_t)data->touch_x - PADDLE_WIDTH_PX / 2;
        ClampPaddleToScreen(data);
    }
    if (data->paddle.x != old_paddle_x) {
        UTIL_LCD_FillRect(old_paddle_x, data->paddle.y, PADDLE_WIDTH_PX, PADDLE_HEIGHT_PX, COLOR_BG);
        UTIL_LCD_FillRect(data->paddle.x, data->paddle.y, PADDLE_WIDTH_PX, PADDLE_HEIGHT_PX, COLOR_PADDLE);
    }

    /* --- ball step --- */
    int32_t old_ball_x = data->ball.x;
    int32_t old_ball_y = data->ball.y;

    data->ball.x += data->ball.vx;
    data->ball.y += data->ball.vy;

    ReflectOffWalls(data);
    TryPaddleBounce(data);
    uint32_t brick_hit = TryBrickBounce(data);

    UTIL_LCD_FillCircle(old_ball_x, old_ball_y, BALL_RADIUS_PX, COLOR_BG);
    UTIL_LCD_FillCircle(data->ball.x, data->ball.y, BALL_RADIUS_PX, COLOR_BALL);

    if (brick_hit) {
        DrawHud(data); /* score changed */
    }

    /* --- ball dropped past the paddle --- */
    if (data->ball.y - BALL_RADIUS_PX > (int32_t)data->screen_y) {
        UTIL_LCD_FillCircle(data->ball.x, data->ball.y, BALL_RADIUS_PX, COLOR_BG);
        if (data->lives > 0) data->lives--;
        DrawHud(data);
        if (data->lives == 0) {
            data->refresh = State_GameOver;
        } else {
            ResetBallAndPaddle(data);
        }
        return;
    }

    if (data->bricks_remaining == 0) {
        data->refresh = State_LevelClear;
    }
}

/* ---- state: LEVEL CLEAR ----------------------------------------------------- */

static void State_LevelClear(GameData *data)
{
    static uint32_t drawn = 0;
    if (!drawn) {
        UTIL_LCD_Clear(COLOR_BG);
        DrawCenteredText(data, data->screen_y / 2 - 15, "LEVEL CLEAR", &Font24, UTIL_LCD_COLOR_GREEN);
        drawn = 1;
        data->last_step_tick = data->timer; /* reuse as a simple delay timestamp */
    }

    if (data->timer - data->last_step_tick > 1200) {
        drawn = 0;
        data->level++;
        if (data->ball.vx > 0) data->ball.vx++; else data->ball.vx--;
        InitBricks(data);
        ResetBallAndPaddle(data);
        UTIL_LCD_Clear(COLOR_BG);
        DrawHud(data);
        DrawAllBricks(data);
        data->refresh = State_Play;
    }
}

/* ---- state: GAME OVER -------------------------------------------------------- */

static void State_GameOver(GameData *data)
{
    static uint32_t drawn = 0;
    if (!drawn) {
        char buf[32];
        UTIL_LCD_Clear(COLOR_BG);
        DrawCenteredText(data, data->screen_y / 2 - 40, "GAME OVER", &Font24, UTIL_LCD_COLOR_RED);
        snprintf(buf, sizeof(buf), "SCORE %u", data->score);
        DrawCenteredText(data, data->screen_y / 2, buf, &Font16, COLOR_TEXT);
        DrawCenteredText(data, data->screen_y / 2 + 30, "TAP TO RESTART", &Font16, UTIL_LCD_COLOR_LIGHTGRAY);
        drawn = 1;
    }

    if (TapEdge(data)) {
        drawn = 0;
        data->refresh = State_Start;
    }
}

/* ---- public API matching pacman.h's contract ---------------------------------- */

GameData GameData_Init(uint32_t max_score)
{
    GameData data;
    memset(&data, 0, sizeof(data));
    (void)max_score; /* not used by this game, kept for call-signature compatibility */
    return data;
}

void GameReset(GameData *data)
{
    data->score = 0;
    data->level = 0;
    data->lives = STARTING_LIVES;
    data->touch_active = 0;
    data->prev_touch_active = 0;
    data->last_step_tick = 0;
    data->refresh = State_Start;
}
