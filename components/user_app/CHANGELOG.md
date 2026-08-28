# User App Cleanup - AI Audio Chat Only

## Overview
Streamlined `user_app` component to focus exclusively on AI Audio Chat functionality, removing all test and demo code.

## Changes Made

### Removed Components
1. **Test Tasks** - All removed:
   - `example_color_task` - RGB color cycling demo
   - `example_button_boot_task` - Button event handling for demos
   - `example_button_pwr_task` - Power button management
   - `example_user_task` - RTC, IMU, ADC monitoring
   - `example_sdcard_task` - SD card testing
   - `example_scan_wifi_ble_task` - WiFi/BLE scanning demo
   - `example_test_touch_task` - Touch canvas demo
   - `i2s_audio_Test` - Audio recording/playback test
   - `power_Test` - Battery power detection

2. **Callback Functions** - Removed:
   - `audio_Test_Callback` - Audio test buttons
   - `lvgl_obj_event_callback` - Slider/brightness control
   - `set_canvas_color` - Touch canvas drawing
   - `set_canvas_fill` - Canvas clearing

3. **Dependencies Removed**:
   - `sdcard_bsp.h` - SD card operations
   - `button_bsp.h` - Physical button handling
   - `adc_bsp.h` - Analog-to-digital converter
   - `i2c_equipment.h` - RTC, IMU sensors
   - `ble_scan_bsp.h` - Bluetooth scanning
   - `esp_timer.h`, `esp_http_client.h`, `esp_crt_bundle.h`, `esp_sntp.h`
   - `cJSON.h`, `errno.h`
   - Standard library: `<string>`, `<cstring>`, `<cstdlib>`, `<time.h>`, `<sys/time.h>`

4. **Global Variables Removed**:
   - `app_touch_data_queue` - Touch event queue
   - `app_touch_group` - Touch event group
   - `audio_ptr` - Large audio buffer (192KB)
   - `lvgl_button_groups` - Button event synchronization
   - `is_vbatpowerflag` - Battery power state

5. **Header File**:
   - Removed `app_touch_t` structure definition
   - Removed `app_touch_data_queue` extern declaration
   - Removed `freertos/FreeRTOS.h` include

### Kept Components
1. **Essential Initialization**:
   - UI setup (`setup_ui`)
   - WiFi initialization (`espwifi_init`)
   - AI Chat component with OpenRouter API
   - LCD backlight PWM
   - I/O expander for power management
   - Audio codec initialization

2. **Key Settings**:
   - **API Key**: Configured for OpenRouter
   - **Model**: `google/gemini-2.0-flash-exp` (supports native audio input)
   - **Audio**: ES8311 DAC + ES7210 ADC at 16kHz/2ch/16-bit

## File Sizes
- **Before**: 453 lines (user_app.cpp)
- **After**: 60 lines (user_app.cpp)
- **Reduction**: 86% smaller

## Benefits
1. ✅ **Cleaner code** - No test/demo clutter
2. ✅ **Lower memory usage** - No 192KB audio buffer allocation
3. ✅ **Fewer tasks** - Only essential background tasks run
4. ✅ **Faster initialization** - No sensor/peripheral setup delays
5. ✅ **Single purpose** - Dedicated AI audio chat device
6. ✅ **Easier maintenance** - Minimal surface area for bugs

## Testing Notes
- The AI Chat UI is created by `ai_chat_bsp` component
- Voice button and output area are managed automatically
- All audio operations handled by background tasks in `ai_chat_bsp`
- No manual task creation needed in user_app

## Migration Notes
If you need any removed functionality:
- **Audio testing**: Check `components/audio_bsp/` examples
- **Sensor data**: See `components/i2c_equipment/`
- **SD card**: See `components/sdcard_bsp/`
- **Touch drawing**: See original git history for canvas code
- **Button handling**: See `components/button_bsp/`

## Next Steps
Consider further UI optimization by simplifying the LVGL screens in `setup_ui()` to show only:
- AI Chat conversation area
- Voice input button
- Status/connection indicator
- Optional: Battery level, WiFi indicator
