/* Virtual PS2 controller drawn over the game on touch screens (iOS). Main thread only. */
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

void touchpad_layout(SDL_Window *win, int w, int h);   /* w, h: render output in pixels */
void touchpad_event(const SDL_Event *e);                /* finger events; others are ignored */
void touchpad_set_gamepad(int connected);               /* a physical controller hides the overlay until touched */
uint16_t touchpad_buttons(void);                        /* pressed buttons, display.c bit order, 1 = pressed */
int touchpad_stick(uint8_t *x, uint8_t *y);             /* left stick, 0..255; returns 0 when untouched */
void touchpad_draw(SDL_Renderer *ren);
