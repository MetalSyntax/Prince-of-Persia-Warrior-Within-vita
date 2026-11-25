#pragma once
// Menú de opciones del port (SELECT+START), copia del menú v2 de HAWX-vita / Ferrari-GT-Evolution-vita adaptada a PoP:
// editor de botones, controles en pantalla (L+R), pantalla completa, filtro, MSAA, contador y límite de FPS.
// Se guarda en DATA_PATH options.txt (los botones en controls.txt). El ocultado del HUD es el de gles.c (propio de PoP).
#include <stdint.h>

enum { FILTER_BILINEAR, FILTER_SHARP };
extern int opt_filter;      // escalado del drawable a la pantalla
extern int opt_msaa;        // 0 = off, 2 = 2x, 4 = 4x (se aplica al reiniciar: vitaGL lo fija en vglInit)
extern int opt_msaa_active; // el valor con que arrancó vitaGL
extern int opt_stretch;     // 1 = pantalla completa (960x544), 0 = aspecto original 816x544 con barras
extern int opt_show_fps;    // contador arriba a la izquierda
extern int opt_fps_cap;     // 0 = sin límite (el ritmo del juego), 30 = máximo 30 fps
extern int opt_hud_hidden;  // controles táctiles del juego ocultos (L+R, gles.c); siguen respondiendo

void options_load(void);     // antes de vglInitExtended
const char *options_machine(void); // darwin.c: hw.machine (fila "iPhone profile")
void options_save(void);
int options_msaa_mode(void); // SceGxmMultisampleMode para vglInitExtended
void options_open(void);     // patch.c: SELECT+START
int options_pending(void);
void options_run(void (*draw_game)(void)); // gles_present: bucle modal con el juego en pausa detrás
void options_frame(void);    // gles_present, justo antes del swap (estado de pantalla 960x544): FPS
uint32_t options_button_mask(const char *name); // "CROSS", "L"... -> SCE_CTRL_*, 0 si no existe
const char *options_button_name(uint32_t mask);  // un solo bit -> nombre

// Lo pone patch.c: acciones del juego (bit INO) y el/los botones de la Vita que las disparan.
int input_bind_count(void);
const char *input_bind_label(int i);
uint32_t input_bind_mask(int i);
void input_bind_set(int i, uint32_t mask);
void input_save_controls(void);
