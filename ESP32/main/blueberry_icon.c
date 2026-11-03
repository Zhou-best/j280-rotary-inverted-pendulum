#include "blueberry_icon.h"
#include "tft_spi.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"


extern spi_device_handle_t disp_spi;

#define TFT_CS  14
#define TFT_DC  12

void draw_blueberry(uint16_t x, uint16_t y, uint8_t scale) {
    uint16_t leaf_green  = 0x2584;
    uint16_t berry_red   = 0xF800;
    uint16_t seed_yellow = 0xFFE0;


    static const uint8_t map[24][24] = {
        {0,0,0,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,0,0,0},

        {0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,1,1,1,3,1,1,1,1,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,1,1,1,1,1,3,1,1,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1,1,1,3,1,1,1,1,1,1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,1,1,1,3,1,1,3,1,1,1,1,1,0,0,0,0,0,0},
        {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
        {0,0,0,0,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,0,0,0,0,0},
        {0,0,0,0,1,1,1,1,1,1,1,1,1,3,1,1,1,1,1,0,0,0,0,0},
        {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
        {0,0,0,0,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,0,0,0,0,0},
        {0,0,0,0,0,1,1,1,1,1,1,1,1,3,1,1,1,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0},
        {0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0}
    };


    uint16_t w = 24 * scale;
    uint16_t h = 24 * scale;


    static uint16_t buf[48 * 48];


    const uint16_t bg = 0x02C5;
    for (int i = 0; i < w * h; i++) buf[i] = bg;


    for (int row = 0; row < 24; row++) {
        for (int col = 0; col < 24; col++) {
            uint16_t color;
            switch (map[23 - row][col]) {
                case 1: color = berry_red;   break;
                case 2: color = leaf_green;  break;
                case 3: color = seed_yellow; break;
                default: continue;
            }
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    buf[(row * scale + dy) * w + col * scale + dx] = color;
                }
            }
        }
    }


    {
        uint16_t x1 = x, y1 = y, x2 = x + w - 1, y2 = y + h - 1;
        spi_transaction_t t;
        uint8_t buf8[4];


        gpio_set_level(TFT_DC, 0); gpio_set_level(TFT_CS, 0);
        t = (spi_transaction_t){ .length = 8, .tx_data = {0x2A}, .flags = SPI_TRANS_USE_TXDATA };
        spi_device_transmit(disp_spi, &t);
        gpio_set_level(TFT_DC, 1);
        buf8[0] = (uint8_t)(x1 >> 8); buf8[1] = (uint8_t)(x1 & 0xFF);
        buf8[2] = (uint8_t)(x2 >> 8); buf8[3] = (uint8_t)(x2 & 0xFF);
        t = (spi_transaction_t){ .length = 32, .tx_buffer = buf8, .flags = 0 };
        spi_device_transmit(disp_spi, &t);


        gpio_set_level(TFT_DC, 0);
        t = (spi_transaction_t){ .length = 8, .tx_data = {0x2B}, .flags = SPI_TRANS_USE_TXDATA };
        spi_device_transmit(disp_spi, &t);
        gpio_set_level(TFT_DC, 1);
        buf8[0] = (uint8_t)(y1 >> 8); buf8[1] = (uint8_t)(y1 & 0xFF);
        buf8[2] = (uint8_t)(y2 >> 8); buf8[3] = (uint8_t)(y2 & 0xFF);
        t = (spi_transaction_t){ .length = 32, .tx_buffer = buf8, .flags = 0 };
        spi_device_transmit(disp_spi, &t);


        gpio_set_level(TFT_DC, 0);
        t = (spi_transaction_t){ .length = 8, .tx_data = {0x2C}, .flags = SPI_TRANS_USE_TXDATA };
        spi_device_transmit(disp_spi, &t);
        gpio_set_level(TFT_DC, 1);
        t = (spi_transaction_t){ .length = w * h * 2 * 8, .tx_buffer = buf, .flags = 0 };
        spi_device_transmit(disp_spi, &t);
        gpio_set_level(TFT_CS, 1);
    }
}
