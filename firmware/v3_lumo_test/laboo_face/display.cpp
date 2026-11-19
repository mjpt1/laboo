#include "display.h"
#include "TCA9554PWR.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_heap_caps.h"
#include <stdarg.h>

#define LCD_HOST        SPI2_HOST
#define PIN_TE          18
#define PIN_SCK         40
#define PIN_D0          46
#define PIN_D1          45
#define PIN_D2          42
#define PIN_D3          41
#define PIN_CS          21
#define LCD_OPCODE_READ_CMD (0x0BULL)

static esp_lcd_panel_handle_t panel = NULL;
static uint16_t* bands[2] = {NULL, NULL};
static int cur = 0;

// Init sequence for the newer panel revision (ID 00 02 7F 7F). Taken from Waveshare's
// ESP-IDF example -- their Arduino example ships this table EMPTY, which leaves the
// screen black on newer boards.
static const st77916_lcd_init_cmd_t vendor_init_new[] = {
  {0xF0, (uint8_t []){0x28}, 1, 0},
  {0xF2, (uint8_t []){0x28}, 1, 0},
  {0x73, (uint8_t []){0xF0}, 1, 0},
  {0x7C, (uint8_t []){0xD1}, 1, 0},
  {0x83, (uint8_t []){0xE0}, 1, 0},
  {0x84, (uint8_t []){0x61}, 1, 0},
  {0xF2, (uint8_t []){0x82}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0xF0, (uint8_t []){0x01}, 1, 0},
  {0xF1, (uint8_t []){0x01}, 1, 0},
  {0xB0, (uint8_t []){0x56}, 1, 0},
  {0xB1, (uint8_t []){0x4D}, 1, 0},
  {0xB2, (uint8_t []){0x24}, 1, 0},
  {0xB4, (uint8_t []){0x87}, 1, 0},
  {0xB5, (uint8_t []){0x44}, 1, 0},
  {0xB6, (uint8_t []){0x8B}, 1, 0},
  {0xB7, (uint8_t []){0x40}, 1, 0},
  {0xB8, (uint8_t []){0x86}, 1, 0},
  {0xBA, (uint8_t []){0x00}, 1, 0},
  {0xBB, (uint8_t []){0x08}, 1, 0},
  {0xBC, (uint8_t []){0x08}, 1, 0},
  {0xBD, (uint8_t []){0x00}, 1, 0},
  {0xC0, (uint8_t []){0x80}, 1, 0},
  {0xC1, (uint8_t []){0x10}, 1, 0},
  {0xC2, (uint8_t []){0x37}, 1, 0},
  {0xC3, (uint8_t []){0x80}, 1, 0},
  {0xC4, (uint8_t []){0x10}, 1, 0},
  {0xC5, (uint8_t []){0x37}, 1, 0},
  {0xC6, (uint8_t []){0xA9}, 1, 0},
  {0xC7, (uint8_t []){0x41}, 1, 0},
  {0xC8, (uint8_t []){0x01}, 1, 0},
  {0xC9, (uint8_t []){0xA9}, 1, 0},
  {0xCA, (uint8_t []){0x41}, 1, 0},
  {0xCB, (uint8_t []){0x01}, 1, 0},
  {0xD0, (uint8_t []){0x91}, 1, 0},
  {0xD1, (uint8_t []){0x68}, 1, 0},
  {0xD2, (uint8_t []){0x68}, 1, 0},
  {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
  {0xDD, (uint8_t []){0x4F}, 1, 0},
  {0xDE, (uint8_t []){0x4F}, 1, 0},
  {0xF1, (uint8_t []){0x10}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0xF0, (uint8_t []){0x02}, 1, 0},
  {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
  {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
  {0xF0, (uint8_t []){0x10}, 1, 0},
  {0xF3, (uint8_t []){0x10}, 1, 0},
  {0xE0, (uint8_t []){0x07}, 1, 0},
  {0xE1, (uint8_t []){0x00}, 1, 0},
  {0xE2, (uint8_t []){0x00}, 1, 0},
  {0xE3, (uint8_t []){0x00}, 1, 0},
  {0xE4, (uint8_t []){0xE0}, 1, 0},
  {0xE5, (uint8_t []){0x06}, 1, 0},
  {0xE6, (uint8_t []){0x21}, 1, 0},
  {0xE7, (uint8_t []){0x01}, 1, 0},
  {0xE8, (uint8_t []){0x05}, 1, 0},
  {0xE9, (uint8_t []){0x02}, 1, 0},
  {0xEA, (uint8_t []){0xDA}, 1, 0},
  {0xEB, (uint8_t []){0x00}, 1, 0},
  {0xEC, (uint8_t []){0x00}, 1, 0},
  {0xED, (uint8_t []){0x0F}, 1, 0},
  {0xEE, (uint8_t []){0x00}, 1, 0},
  {0xEF, (uint8_t []){0x00}, 1, 0},
  {0xF8, (uint8_t []){0x00}, 1, 0},
  {0xF9, (uint8_t []){0x00}, 1, 0},
  {0xFA, (uint8_t []){0x00}, 1, 0},
  {0xFB, (uint8_t []){0x00}, 1, 0},
  {0xFC, (uint8_t []){0x00}, 1, 0},
  {0xFD, (uint8_t []){0x00}, 1, 0},
  {0xFE, (uint8_t []){0x00}, 1, 0},
  {0xFF, (uint8_t []){0x00}, 1, 0},
  {0x60, (uint8_t []){0x40}, 1, 0},
  {0x61, (uint8_t []){0x04}, 1, 0},
  {0x62, (uint8_t []){0x00}, 1, 0},
  {0x63, (uint8_t []){0x42}, 1, 0},
  {0x64, (uint8_t []){0xD9}, 1, 0},
  {0x65, (uint8_t []){0x00}, 1, 0},
  {0x66, (uint8_t []){0x00}, 1, 0},
  {0x67, (uint8_t []){0x00}, 1, 0},
  {0x68, (uint8_t []){0x00}, 1, 0},
  {0x69, (uint8_t []){0x00}, 1, 0},
  {0x6A, (uint8_t []){0x00}, 1, 0},
  {0x6B, (uint8_t []){0x00}, 1, 0},
  {0x70, (uint8_t []){0x40}, 1, 0},
  {0x71, (uint8_t []){0x03}, 1, 0},
  {0x72, (uint8_t []){0x00}, 1, 0},
  {0x73, (uint8_t []){0x42}, 1, 0},
  {0x74, (uint8_t []){0xD8}, 1, 0},
  {0x75, (uint8_t []){0x00}, 1, 0},
  {0x76, (uint8_t []){0x00}, 1, 0},
  {0x77, (uint8_t []){0x00}, 1, 0},
  {0x78, (uint8_t []){0x00}, 1, 0},
  {0x79, (uint8_t []){0x00}, 1, 0},
  {0x7A, (uint8_t []){0x00}, 1, 0},
  {0x7B, (uint8_t []){0x00}, 1, 0},
  {0x80, (uint8_t []){0x48}, 1, 0},
  {0x81, (uint8_t []){0x00}, 1, 0},
  {0x82, (uint8_t []){0x06}, 1, 0},
  {0x83, (uint8_t []){0x02}, 1, 0},
  {0x84, (uint8_t []){0xD6}, 1, 0},
  {0x85, (uint8_t []){0x04}, 1, 0},
  {0x86, (uint8_t []){0x00}, 1, 0},
  {0x87, (uint8_t []){0x00}, 1, 0},
  {0x88, (uint8_t []){0x48}, 1, 0},
  {0x89, (uint8_t []){0x00}, 1, 0},
  {0x8A, (uint8_t []){0x08}, 1, 0},
  {0x8B, (uint8_t []){0x02}, 1, 0},
  {0x8C, (uint8_t []){0xD8}, 1, 0},
  {0x8D, (uint8_t []){0x04}, 1, 0},
  {0x8E, (uint8_t []){0x00}, 1, 0},
  {0x8F, (uint8_t []){0x00}, 1, 0},
  {0x90, (uint8_t []){0x48}, 1, 0},
  {0x91, (uint8_t []){0x00}, 1, 0},
  {0x92, (uint8_t []){0x0A}, 1, 0},
  {0x93, (uint8_t []){0x02}, 1, 0},
  {0x94, (uint8_t []){0xDA}, 1, 0},
  {0x95, (uint8_t []){0x04}, 1, 0},
  {0x96, (uint8_t []){0x00}, 1, 0},
  {0x97, (uint8_t []){0x00}, 1, 0},
  {0x98, (uint8_t []){0x48}, 1, 0},
  {0x99, (uint8_t []){0x00}, 1, 0},
  {0x9A, (uint8_t []){0x0C}, 1, 0},
  {0x9B, (uint8_t []){0x02}, 1, 0},
  {0x9C, (uint8_t []){0xDC}, 1, 0},
  {0x9D, (uint8_t []){0x04}, 1, 0},
  {0x9E, (uint8_t []){0x00}, 1, 0},
  {0x9F, (uint8_t []){0x00}, 1, 0},
  {0xA0, (uint8_t []){0x48}, 1, 0},
  {0xA1, (uint8_t []){0x00}, 1, 0},
  {0xA2, (uint8_t []){0x05}, 1, 0},
  {0xA3, (uint8_t []){0x02}, 1, 0},
  {0xA4, (uint8_t []){0xD5}, 1, 0},
  {0xA5, (uint8_t []){0x04}, 1, 0},
  {0xA6, (uint8_t []){0x00}, 1, 0},
  {0xA7, (uint8_t []){0x00}, 1, 0},
  {0xA8, (uint8_t []){0x48}, 1, 0},
  {0xA9, (uint8_t []){0x00}, 1, 0},
  {0xAA, (uint8_t []){0x07}, 1, 0},
  {0xAB, (uint8_t []){0x02}, 1, 0},
  {0xAC, (uint8_t []){0xD7}, 1, 0},
  {0xAD, (uint8_t []){0x04}, 1, 0},
  {0xAE, (uint8_t []){0x00}, 1, 0},
  {0xAF, (uint8_t []){0x00}, 1, 0},
  {0xB0, (uint8_t []){0x48}, 1, 0},
  {0xB1, (uint8_t []){0x00}, 1, 0},
  {0xB2, (uint8_t []){0x09}, 1, 0},
  {0xB3, (uint8_t []){0x02}, 1, 0},
  {0xB4, (uint8_t []){0xD9}, 1, 0},
  {0xB5, (uint8_t []){0x04}, 1, 0},
  {0xB6, (uint8_t []){0x00}, 1, 0},
  {0xB7, (uint8_t []){0x00}, 1, 0},
  
  {0xB8, (uint8_t []){0x48}, 1, 0},
  {0xB9, (uint8_t []){0x00}, 1, 0},
  {0xBA, (uint8_t []){0x0B}, 1, 0},
  {0xBB, (uint8_t []){0x02}, 1, 0},
  {0xBC, (uint8_t []){0xDB}, 1, 0},
  {0xBD, (uint8_t []){0x04}, 1, 0},
  {0xBE, (uint8_t []){0x00}, 1, 0},
  {0xBF, (uint8_t []){0x00}, 1, 0},
  {0xC0, (uint8_t []){0x10}, 1, 0},
  {0xC1, (uint8_t []){0x47}, 1, 0},
  {0xC2, (uint8_t []){0x56}, 1, 0},
  {0xC3, (uint8_t []){0x65}, 1, 0},
  {0xC4, (uint8_t []){0x74}, 1, 0},
  {0xC5, (uint8_t []){0x88}, 1, 0},
  {0xC6, (uint8_t []){0x99}, 1, 0},
  {0xC7, (uint8_t []){0x01}, 1, 0},
  {0xC8, (uint8_t []){0xBB}, 1, 0},
  {0xC9, (uint8_t []){0xAA}, 1, 0},
  {0xD0, (uint8_t []){0x10}, 1, 0},
  {0xD1, (uint8_t []){0x47}, 1, 0},
  {0xD2, (uint8_t []){0x56}, 1, 0},
  {0xD3, (uint8_t []){0x65}, 1, 0},
  {0xD4, (uint8_t []){0x74}, 1, 0},
  {0xD5, (uint8_t []){0x88}, 1, 0},
  {0xD6, (uint8_t []){0x99}, 1, 0},
  {0xD7, (uint8_t []){0x01}, 1, 0},
  {0xD8, (uint8_t []){0xBB}, 1, 0},
  {0xD9, (uint8_t []){0xAA}, 1, 0},
  {0xF3, (uint8_t []){0x01}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0x21, (uint8_t []){0x00}, 1, 0},
  {0x11, (uint8_t []){0x00}, 1, 120},
  {0x29, (uint8_t []){0x00}, 1, 0},  
};


static void lcd_reset() {
  Set_EXIO(EXIO_PIN2, Low);
  vTaskDelay(pdMS_TO_TICKS(10));
  Set_EXIO(EXIO_PIN2, High);
  vTaskDelay(pdMS_TO_TICKS(50));
}

static char bootlog[1024]; static size_t bootlen = 0;
void blog(const char* fmt, ...) {
  char tmp[160];
  va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
  if (n > 0) {
    if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    if (bootlen + n < sizeof(bootlog) - 1) { memcpy(bootlog + bootlen, tmp, n); bootlen += n; bootlog[bootlen] = 0; }
    Serial.write((const uint8_t*)tmp, n);
  }
}
const char* blog_text() { return bootlog; }
#define DBG(...) blog(__VA_ARGS__)

bool display_init() {
  DBG("[boot] lcd reset\n");
  lcd_reset();
  pinMode(PIN_TE, OUTPUT);

  spi_bus_config_t bus = {};
  bus.data0_io_num = PIN_D0;
  bus.data1_io_num = PIN_D1;
  bus.sclk_io_num  = PIN_SCK;
  bus.data2_io_num = PIN_D2;
  bus.data3_io_num = PIN_D3;
  bus.data4_io_num = -1; bus.data5_io_num = -1; bus.data6_io_num = -1; bus.data7_io_num = -1;
  bus.max_transfer_sz = LCD_W * BAND_H * 2 + 64;
  bus.flags = SPICOMMON_BUSFLAG_MASTER;
  esp_err_t e = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
  DBG("[boot] spi bus: %s\n", esp_err_to_name(e));
  if (e != ESP_OK) return false;

  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = PIN_CS;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  io.pclk_hz = 3 * 1000 * 1000;          // slow first, to read the panel ID
  io.trans_queue_depth = 10;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = 1;
  esp_lcd_panel_io_handle_t io_handle = NULL;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &io_handle) != ESP_OK) return false;

  // Read register 0x04 to find out which panel revision we have
  uint8_t id[4] = {0};
  int cmd = (0x04 << 8) | (LCD_OPCODE_READ_CMD << 24);
  e = esp_lcd_panel_io_rx_param(io_handle, cmd, id, sizeof(id));
  DBG("[boot] panel id read: %s  %02x %02x %02x %02x\n", esp_err_to_name(e), id[0], id[1], id[2], id[3]);

  esp_lcd_panel_io_del(io_handle);          // the slow ID-read IO is only temporary
  io_handle = NULL;
  io.pclk_hz = 80 * 1000 * 1000;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &io_handle) != ESP_OK) return false;

  st77916_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  if (id[0] == 0x00 && id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F) {
    DBG("[boot] panel: old revision, default init\n");
  } else {
    // new revision (00 02 7F 7F) -- also used when the ID is unknown
    vendor.init_cmds = vendor_init_new;
    vendor.init_cmds_size = sizeof(vendor_init_new) / sizeof(st77916_lcd_init_cmd_t);
    DBG("[boot] panel: new revision, %d init commands\n", (int)vendor.init_cmds_size);
  }

  esp_lcd_panel_dev_config_t pc = {};
  pc.reset_gpio_num = -1;
  pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  pc.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
  pc.bits_per_pixel = 16;
  pc.vendor_config = &vendor;
  e = esp_lcd_new_panel_st77916(io_handle, &pc, &panel);
  DBG("[boot] new panel: %s\n", esp_err_to_name(e));
  if (e != ESP_OK) return false;
  esp_lcd_panel_reset(panel);
  e = esp_lcd_panel_init(panel);
  DBG("[boot] panel init: %s\n", esp_err_to_name(e));
  esp_lcd_panel_disp_on_off(panel, true);

  for (int i = 0; i < 2; i++) {
    bands[i] = (uint16_t*)heap_caps_malloc(LCD_W * BAND_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!bands[i]) { DBG("[boot] band malloc FAILED\n"); return false; }
  }
  DBG("[boot] display ok, free internal RAM %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  ledcAttach(LCD_BL_PIN, 20000, 10);
  display_set_backlight(80);
  return true;
}

void display_set_backlight(uint8_t p) {
  if (p > 100) p = 100;
  ledcWrite(LCD_BL_PIN, (uint32_t)p * 1023 / 100);
}

// Double buffering: while one band is being sent by DMA, we render into the other.
// esp_lcd waits for the previous color transfer before starting a new draw call,
// so the buffer we hand out here is always free.
uint16_t* display_band_buffer() { return bands[cur]; }

void display_push_band(int y0, int h) {
  esp_err_t e = esp_lcd_panel_draw_bitmap(panel, 0, y0, LCD_W, y0 + h, bands[cur]);
  static int errs = 0;
  if (e != ESP_OK && errs++ < 5) DBG("[lcd] draw error: %s\n", esp_err_to_name(e));
  cur ^= 1;
}

void display_fill(uint16_t be_color) {
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    uint16_t* b = display_band_buffer();
    for (int i = 0; i < LCD_W * BAND_H; i++) b[i] = be_color;
    display_push_band(y0, BAND_H);
  }
}
