/**
 * @file gles.h
 * @brief OpenGL ES 1.1 / 2.0 graphics pipeline over vitaGL with SoftFP calling convention.
 */
#pragma once
#include <stdint.h>

/**
 * @brief Initializes the OpenGL ES pipeline and off-screen virtual FBO.
 */
void gles_init(void);

/**
 * @brief Presents current frame: blits and scales iOS FBO to PS Vita 960x544 screen.
 */
void gles_present(void);

/**
 * @brief Returns the total number of draw calls dispatched this session.
 */
uint32_t gles_get_draw_calls(void);

/**
 * @brief Destination rectangle of the game image on the 960x544 screen (x offset and width; full height).
 */
void gles_dest_rect(int *x, int *w);

/**
 * @brief 1 = following draws write no color (0 % opacity), 0 = back to the previous color mask.
 */
void gles_color_mask_off(int on);
