# Svelte UI Integration - Unravel Agent

This document describes the integration of the ESP32-Sveltekit web interface into the Unravel Agent project.

## Overview

The Svelte UI from the ESP32-Sveltekit project has been integrated into this ESP-IDF based project. The web interface is built during compilation and embedded directly into the firmware as compressed assets.

## Project Structure

```
Unravel agent/
├── interface/              # SvelteKit web application
│   ├── src/               # Svelte source files
│   ├── build/             # Built output (generated)
│   ├── CMakeLists.txt     # ESP-IDF component configuration
│   └── dummy.c            # Dummy source for ESP-IDF component
├── scripts/
│   └── build_interface_idf.py  # Build script for ESP-IDF
├── main/
│   └── WWWData.h          # Generated embedded web assets (1.4MB)
├── partitions.csv         # Updated partition table
└── CMakeLists.txt         # Updated with interface component
```

## Integration Details

### 1. Web Interface Component (`interface/`)

The complete SvelteKit application was copied from ESP32-sveltekit, including:
- **Framework**: SvelteKit with Svelte 5 + TypeScript
- **Styling**: TailwindCSS v4 + DaisyUI
- **Features**:
  - WiFi management (AP and STA modes)
  - System status and metrics
  - Connection management (MQTT, NTP)
  - User management
  - OTA updates
  - Chart.js for data visualization
  - MessagePack for efficient data serialization

### 2. Build System Integration

#### CMakeLists.txt Changes

**Main CMakeLists.txt** ([CMakeLists.txt:7-18](CMakeLists.txt#L7-L18)):
- Added Python3 finder for build scripts
- Added `interface` to `EXTRA_COMPONENT_DIRS`
- Added compile definitions:
  - `EMBED_WWW` - Embeds web assets in firmware
  - `APP_NAME="Unravel-Agent"`
  - `APP_VERSION="1.0.0"`

**Interface Component** ([interface/CMakeLists.txt](interface/CMakeLists.txt)):
- Registers as an ESP-IDF component with dummy.c
- Defines custom command to build web interface
- Calls Python build script to generate WWWData.h
- Makes component depend on web interface build

#### Build Script ([scripts/build_interface_idf.py](scripts/build_interface_idf.py))

Adapted from PlatformIO version to work with ESP-IDF:
- Detects package manager (npm/yarn/pnpm)
- Installs Node.js dependencies
- Builds SvelteKit application
- Converts built assets to compressed C arrays
- Generates WWWData.h header with 67 embedded assets

### 3. Partition Table

Updated [partitions.csv](partitions.csv) to support OTA and web interface:

```csv
# Name      Type  SubType   Offset   Size      Flags
nvs,        data, nvs,      0x9000,  0x6000,   # Settings storage
phy_init,   data, phy,      0xf000,  0x1000,   # WiFi calibration
factory,    app,  factory,  0x10000, 3M,       # Main app (includes WWWData.h)
ota_0,      app,  ota_0,    ,        3M,       # OTA partition 0
ota_1,      app,  ota_1,    ,        3M,       # OTA partition 1
nvs_key,    data, nvs_keys, ,        0x1000,   # NVS encryption keys
storage,    data, spiffs,   ,        0x100000, # File storage
```

### 4. Generated Assets

The build process generates [main/WWWData.h](main/WWWData.h):
- Size: ~1.4MB
- Contains 67 web assets (HTML, JS, CSS, images)
- All assets are gzip compressed
- Provides C struct array for easy serving:

```c
typedef struct {
    const char* uri;
    const char* content_type;
    const uint8_t* content;
    size_t len;
} www_data_t;

#define WWW_DATA_COUNT 67
extern const www_data_t www_data[WWW_DATA_COUNT];
```

## Build Process

### First Time Setup

1. **Install Node.js dependencies**:
   ```bash
   cd interface
   npm install
   ```

2. **Build web interface** (optional, happens automatically):
   ```bash
   npm run build
   ```

### Normal Build

When you build the ESP-IDF project:

1. CMake detects changes in `interface/src/`
2. Automatically runs `build_interface_idf.py`
3. Builds SvelteKit application
4. Generates `WWWData.h` with embedded assets
5. Compiles firmware with embedded web interface

### Manual Web Interface Build

To rebuild just the web interface:

```bash
python3 scripts/build_interface_idf.py .
```

## How to Use in Your Code

To serve the embedded web interface, you'll need to:

1. **Include the header**:
   ```c
   #include "WWWData.h"
   ```

2. **Register routes with your HTTP server** (ESP-IDF example):
   ```c
   #include "esp_http_server.h"
   #include "WWWData.h"

   // Handler for serving static files
   esp_err_t www_handler(httpd_req_t *req) {
       for (int i = 0; i < WWW_DATA_COUNT; i++) {
           if (strcmp(req->uri, www_data[i].uri) == 0) {
               httpd_resp_set_type(req, www_data[i].content_type);
               httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
               httpd_resp_send(req, (const char*)www_data[i].content, www_data[i].len);
               return ESP_OK;
           }
       }
       httpd_resp_send_404(req);
       return ESP_FAIL;
   }

   // Register all routes
   void register_www_routes(httpd_handle_t server) {
       httpd_uri_t uri_handler = {
           .method = HTTP_GET,
           .handler = www_handler
       };

       for (int i = 0; i < WWW_DATA_COUNT; i++) {
           uri_handler.uri = www_data[i].uri;
           httpd_register_uri_handler(server, &uri_handler);
       }
   }
   ```

## Configuration Options

In [CMakeLists.txt](CMakeLists.txt), you can configure:

- `EMBED_WWW`: Always enabled for ESP-IDF builds
- `APP_NAME`: Application name (currently "Unravel-Agent")
- `APP_VERSION`: Application version (currently "1.0.0")

## Web Interface Features

The integrated UI provides:

- **Dashboard**: System overview and status
- **WiFi Management**:
  - Station mode (connect to WiFi)
  - Access Point mode (create WiFi hotspot)
- **System**:
  - Status monitoring
  - Performance metrics
  - OTA firmware updates
  - Core dump viewer
- **Connections**:
  - MQTT client configuration
  - NTP time synchronization
- **User Management**: User authentication and settings
- **Demo Pages**: Sample implementations

## Dependencies

### Build Dependencies
- Python 3.x
- Node.js (npm/yarn/pnpm)
- ESP-IDF toolchain

### Node.js Packages
- SvelteKit 2.x
- Svelte 5.x
- TailwindCSS 4.x
- DaisyUI 5.x
- Chart.js
- MessagePack
- And more (see [interface/package.json](interface/package.json))

## Troubleshooting

### Web interface not rebuilding
Delete `main/WWWData.h` to force rebuild:
```bash
rm main/WWWData.h
```

### Node modules missing
```bash
cd interface
rm -rf node_modules package-lock.json
npm install
```

### Build script errors
Run manually to see detailed output:
```bash
python3 scripts/build_interface_idf.py .
```

## Future Enhancements

Potential improvements:
- Add LittleFS support (serve from filesystem instead of embedding)
- Implement caching headers for better performance
- Add API endpoints for backend communication
- Customize UI pages for Unravel Agent specific features
- Add WebSocket support for real-time updates

## Credits

- **ESP32-Sveltekit**: Original web interface framework
  - https://github.com/theelims/ESP32-sveltekit
- **Unravel Agent**: ESP32 AI audio chat agent project
