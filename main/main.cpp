#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "user_config.h"
#include "lv_demos.h"
#include "esp_lcd_axs15231b.h"
#include "i2c_bsp.h"
#include "driver/gpio.h"

#include "user_app.h"
#include "buddy_display_rotation.h"

static_assert(EXAMPLE_LCD_H_RES == BUDDY_DISPLAY_HEIGHT &&
              EXAMPLE_LCD_V_RES == BUDDY_DISPLAY_WIDTH, "Arvel panel geometry must stay 172x640");

static const char *TAG = "example";
static SemaphoreHandle_t lvgl_mux = NULL;

static uint16_t *lvgl_dma_buf = NULL; 
static SemaphoreHandle_t lvgl_flush_semap;


#define LCD_BIT_PER_PIXEL (16)
   


static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);

static bool example_lvgl_lock(int timeout_ms);
static void example_lvgl_unlock(void);
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
void example_lvgl_port_task(void *arg);

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  BaseType_t TaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(lvgl_flush_semap,&TaskWoken);
  // IDF's SPI panel post-callback ignores our return value. Request the yield
  // here instead of waiting for the next tick after every 16-row DMA strip.
  if(TaskWoken == pdTRUE) portYIELD_FROM_ISR();
  return TaskWoken == pdTRUE;
}
static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}
extern "C" void app_main(void)
{
  lvgl_flush_semap = xSemaphoreCreateBinary();
  i2c_master_Init();
  user_app_power_init();
  static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv;      // contains callback functions
#if !CONFIG_UNRAVEL_BOARD_V2
  ESP_LOGI(TAG, "Initialize LCD RESET GPIO");

  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = ((uint64_t)0X01<<EXAMPLE_PIN_NUM_LCD_RST);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
#endif

  ESP_LOGI(TAG, "Initialize QSPI bus");
  spi_bus_config_t buscfg = {};
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Install panel IO");
	esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    
    esp_lcd_panel_io_spi_config_t io_config = {};
		io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;                 
  io_config.dc_gpio_num = GPIO_NUM_NC;          
        io_config.spi_mode = 3;              
        io_config.pclk_hz = 40 * 1000 * 1000;
        // The flush callback waits before reusing its single DMA strip.
        io_config.trans_queue_depth = 1;
        io_config.on_color_trans_done = example_notify_lvgl_flush_ready; 
        //io_config.user_ctx = &disp_drv,         
        io_config.lcd_cmd_bits = 32;         
        io_config.lcd_param_bits = 8;        
        io_config.flags.quad_mode = true;                         
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));
    
	axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    
    esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;
    
    ESP_LOGI(TAG, "Install panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));
    
	ESP_ERROR_CHECK(user_app_lcd_reset_set_level(1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(user_app_lcd_reset_set_level(0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(user_app_lcd_reset_set_level(1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

  	lv_init();
  	lvgl_dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN , MALLOC_CAP_DMA);
  	assert(lvgl_dma_buf);
  	// Keep the adjacent rotation reads from straddling PSRAM cache lines.
  	lv_color_t *buf1 = (lv_color_t *)heap_caps_aligned_alloc(
  		CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE, LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
  	assert(buf1);
  	lv_disp_draw_buf_init(&disp_buf, buf1, NULL, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);

  	ESP_LOGI(TAG, "Register display driver to LVGL");
  	lv_disp_drv_init(&disp_drv);
  	disp_drv.hor_res = EXAMPLE_LCD_V_RES;
  	disp_drv.ver_res = EXAMPLE_LCD_H_RES;
  	disp_drv.flush_cb = example_lvgl_flush_cb;
  	disp_drv.draw_buf = &disp_buf;
  	// Retain the full frame, but redraw only dirty objects/regions.
  	disp_drv.direct_mode = 1;
  	disp_drv.user_data = panel;
  	lv_disp_drv_register(&disp_drv);
	
  	ESP_LOGI(TAG, "Install LVGL tick timer");
  	esp_timer_create_args_t lvgl_tick_timer_args = {};
  	  lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
  	  lvgl_tick_timer_args.name = "lvgl_tick";
  	esp_timer_handle_t lvgl_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  	static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
  	lv_indev_drv_init(&indev_drv);
  	indev_drv.type = LV_INDEV_TYPE_POINTER;
  	indev_drv.read_cb = example_lvgl_touch_cb;
  	lv_indev_drv_register(&indev_drv);

  	lvgl_mux = xSemaphoreCreateMutex();
  	assert(lvgl_mux);
  	// Keep the existing GUI task off the Wi-Fi driver's core. 6144 (was 4000):
  	// the render path's worst observed headroom on 4000 was ~0.6 KB.
  	xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", 6144, NULL, 4, NULL, 1);
  	if (example_lvgl_lock(-1))
  	{
  	  	user_app_init();
  	  	//lv_demo_widgets();
  	  	//lv_demo_music();
  	  	example_lvgl_unlock();
  	}
  	ESP_LOGI(TAG, "Heap after app init: internal=%u largest=%u PSRAM=%u bytes",
  	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
  	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
  	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
/* QSPI ignores RASET: nonzero y_start only continues the previous RAM write.
 * Each refresh must restart at row zero (RAMWR), then send contiguous RAMWRC
 * strips through the last dirty row. Direct mode still redraws only dirty
 * regions into the single retained PSRAM frame; no rotation frame is added. */
static int flush_last_row;
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  if (area->x2 + 1 > flush_last_row) flush_last_row = area->x2 + 1;
  if(!lv_disp_flush_is_last(drv)) {
    lv_disp_flush_ready(drv);
    return;
  }
  int last_row = flush_last_row;
  flush_last_row = 0;
  if (last_row <= 0 || last_row > EXAMPLE_LCD_V_RES) last_row = EXAMPLE_LCD_V_RES;
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  const int strip_rows = LVGL_DMA_BUFF_LEN / (EXAMPLE_LCD_H_RES * sizeof(uint16_t));
  const uint16_t *map = (const uint16_t *)color_map;
  xSemaphoreGive(lvgl_flush_semap);
  for(int physical_y = 0; physical_y < last_row; physical_y += strip_rows)
  {
    const int rows = physical_y + strip_rows <= last_row ?
                     strip_rows : last_row - physical_y;
    xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
    buddy_display_rotate_strip(map, lvgl_dma_buf, physical_y, rows);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, physical_y, EXAMPLE_LCD_H_RES,
                              physical_y + rows, lvgl_dma_buf);
  }
  xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
  lv_disp_flush_ready(drv);
}
static bool example_lvgl_lock(int timeout_ms)
{
  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

static void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}

void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    if (example_lvgl_lock(-1)) 
    {
      task_delay_ms = lv_timer_handler();
      //Release the mutex
      example_lvgl_unlock();
    }
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  (void)drv;
  uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e,0x0, 0x0, 0x0};
  uint8_t buff[32] = {0};
  memset(buff,0,32);
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_touch_write_read(disp_touch_dev_handle,read_touchpad_cmd,11,buff,32));
  uint16_t pointX;
  uint16_t pointY;
  pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
  pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
  //ESP_LOGI("Touch","%d,%d",buff[0],buff[1]);
  if (buff[1]>0 && buff[1]<5)
  {
    data->state = LV_INDEV_STATE_PR;
    uint16_t logicalX;
    uint16_t logicalY;
    buddy_display_touch_to_logical(pointX, pointY, &logicalX, &logicalY);
    data->point.x = logicalX;
    data->point.y = logicalY;
  }
  else 
  {
    data->state = LV_INDEV_STATE_REL;
  }
}
