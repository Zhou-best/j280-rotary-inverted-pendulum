#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


#define TFT_MOSI    11
#define TFT_SCK     10
#define TFT_CS      14
#define TFT_DC      12
#define TFT_RST     13
#define TFT_LED     9


#define BTN_A  4
#define BTN_B  5
#define BTN_C  6


#define TFT_WIDTH   480
#define TFT_HEIGHT  320


#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_RED         0xF800
#define COLOR_GREEN       0x07E0
#define COLOR_BLUE        0x001F
#define COLOR_YELLOW      0xFFE0
#define COLOR_CYAN        0x07FF
#define COLOR_MAGENTA     0xF81F
#define COLOR_GRAY        0x8410
#define COLOR_LIGHT_BLUE  0xCE79
#define COLOR_ORANGE      0xFD20
#define COLOR_PINK        0xF81F
#define COLOR_DARK_GRAY   0x4208
#define COLOR_LIGHT_GRAY  0xC618
#define COLOR_NAVY        0x000F
#define COLOR_DARK_GREEN  0x03E0
#define COLOR_BROWN       0xA145


void tft_init(void);
void tft_fill_screen(uint16_t color);
void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void tft_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);


void tft_draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale);
void tft_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale);


void tft_draw_utf8(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale);
int  tft_utf8_width(const char *str, uint8_t scale);
int  tft_draw_utf8_wrapped(uint16_t x, uint16_t y, uint16_t max_w, uint8_t max_lines,
                           const char *str, uint16_t color, uint16_t bg, uint8_t scale);


void button_init(void);
bool button_a_pressed(void);
bool button_b_pressed(void);
bool button_c_pressed(void);


void menu_init(void);
void menu_run(void);


void draw_ai_chat_screen(const char *status_text);


void tft_request_redraw(void);


void tft_k230_ui_request(int cmd, int action);


void tft_web_ui_request(int cmd, int action);

#ifdef __cplusplus
}
#endif
