# Web Server Setup Guide - Unravel Agent

## Overview

Your Unravel Agent now includes a fully functional web interface served via HTTP. The web server automatically starts when the device boots and is accessible from any browser on your network.

## What's Been Added

### Components Created

1. **web_server component** ([components/web_server/](components/web_server/))
   - `web_server.h` - Public API
   - `web_server.cpp` - HTTP server implementation
   - Serves the embedded Svelte UI
   - Provides REST API endpoints

2. **Integration** ([components/user_app/user_app.cpp:64-71](components/user_app/user_app.cpp#L64-L71))
   - Web server starts automatically after WiFi initialization
   - Integrated into your existing application flow

## How It Works

### Startup Sequence

```
1. Device boots
2. WiFi initializes (espwifi_init)
3. Web server starts automatically
4. Web interface becomes accessible at http://<device-ip>/
```

### Architecture

```
Browser → HTTP Request → ESP32 Web Server
                              ↓
                    ┌─────────┴─────────┐
                    ↓                   ↓
              Static Files          REST APIs
            (from WWWData.h)    (/rest/system/*, etc)
```

## Accessing the Web Interface

### Step 1: Find Your Device's IP Address

The IP address will be printed to the serial console at startup:

```
I (12345) user_app: Web server started successfully
I (12346) user_app: Access web interface at: http://<device-ip>/
```

### Step 2: Connect to WiFi

Your device should already be configured to:
- **Connect to existing WiFi** (Station mode), or
- **Create its own WiFi hotspot** (Access Point mode)

If in AP mode, the default IP is typically: `http://192.168.4.1/`

### Step 3: Open Browser

Navigate to: `http://<your-device-ip>/`

Example: `http://192.168.1.100/`

## Available Features

### Web Interface Pages

The Svelte UI includes:

- **🏠 Dashboard** - System overview
- **📡 WiFi Settings** - Configure WiFi connection
  - Station mode (connect to router)
  - Access Point mode (create hotspot)
- **⚙️ System** - Device management
  - Status monitoring
  - Performance metrics
  - OTA firmware updates
- **🔗 Connections** - Configure services
  - MQTT client
  - NTP time sync
- **👤 User Management** - Authentication settings

### REST API Endpoints

Currently implemented:

#### System Status
```
GET /rest/system/status
```
Returns:
```json
{
  "app_name": "Unravel-Agent",
  "app_version": "1.0.0",
  "chip_model": "ESP32-S3",
  "cpu_cores": 2,
  "free_heap": 234560,
  "min_free_heap": 189432,
  "uptime": 3600
}
```

#### WiFi Status
```
GET /rest/wifi/status
```
Returns:
```json
{
  "mode": 1,
  "ssid": "MyNetwork",
  "rssi": -45,
  "connected": true,
  "ip": "192.168.1.100"
}
```

## Code Reference

### Starting the Web Server

The web server starts automatically in [user_app.cpp](components/user_app/user_app.cpp). If you need to control it manually:

```cpp
#include "web_server.h"

// Start server
httpd_handle_t server = web_server_start();

// Stop server (if needed)
web_server_stop(server);
```

### Adding Custom API Endpoints

To add your own REST API endpoints, edit [components/web_server/web_server.cpp](components/web_server/web_server.cpp):

```cpp
// 1. Create handler function
static esp_err_t api_my_endpoint_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message", "Hello from custom API!");

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);

    return ESP_OK;
}

// 2. Register in web_server_start() function
httpd_uri_t my_endpoint = {
    .uri = "/rest/my/endpoint",
    .method = HTTP_GET,
    .handler = api_my_endpoint_handler,
    .user_ctx = NULL
};
httpd_register_uri_handler(server, &my_endpoint);
```

### Accessing from Your Svelte UI

To call your API from the web interface, edit files in `interface/src/`:

```typescript
// Example: Fetch system status
async function getSystemStatus() {
    const response = await fetch('/rest/system/status');
    const data = await response.json();
    console.log('System status:', data);
    return data;
}
```

## Troubleshooting

### Web interface not accessible

1. **Check WiFi connection**
   ```
   Serial Monitor → Look for WiFi IP address
   ```

2. **Verify web server started**
   ```
   Serial Monitor → Look for "Web server started successfully"
   ```

3. **Check firewall settings**
   - Ensure port 80 is not blocked
   - Try from the same WiFi network

### Page not loading / 404 errors

1. **Rebuild web interface**
   ```bash
   rm main/WWWData.h
   # Then rebuild project
   ```

2. **Check WWWData.h exists**
   ```bash
   ls -lh main/WWWData.h
   # Should show ~1.4MB file
   ```

### API endpoints not working

1. **Check logs**
   ```
   Serial Monitor → Enable verbose logging
   Set log level: CONFIG_LOG_DEFAULT_LEVEL_DEBUG
   ```

2. **Verify WiFi is connected**
   - APIs won't work without network

### Memory issues

If you see heap allocation errors:

1. **Reduce other features** - The web interface uses ~1.4MB of flash
2. **Adjust partition table** - Ensure factory partition is 3MB+
3. **Monitor heap usage**
   ```cpp
   ESP_LOGI(TAG, "Free heap: %d", esp_get_free_heap_size());
   ```

## Performance Considerations

### Memory Usage

- **Flash**: ~1.4MB (compressed web assets in WWWData.h)
- **RAM**: ~8KB (web server stack) + request buffers
- **Heap**: Varies with concurrent connections

### Optimizations Applied

1. **Gzip compression** - All assets pre-compressed
2. **Cache headers** - Immutable assets cached forever
3. **Efficient routing** - Direct array lookup for static files
4. **Stack tuning** - 8KB stack for JSON operations

### Concurrent Connections

Default: 4 concurrent connections (ESP-IDF default)

To increase, edit [web_server.cpp:201](components/web_server/web_server.cpp#L201):
```cpp
config.max_open_sockets = 8; // Increase concurrent connections
```

## Customization

### Change Web Server Port

Edit [web_server.cpp](components/web_server/web_server.cpp):
```cpp
config.server_port = 8080; // Change from default 80
```

### Enable HTTPS

To add SSL/TLS support:
1. Generate certificates
2. Modify `httpd_config_t` to use `httpd_ssl_config`
3. See ESP-IDF HTTPS server examples

### Customize the UI

The Svelte interface source is in `interface/src/`. After making changes:

```bash
cd interface
npm run build
# Rebuild your ESP-IDF project to regenerate WWWData.h
```

## Security Considerations

⚠️ **Important Security Notes**

### Current State
- **No authentication** on web interface
- **No encryption** (HTTP, not HTTPS)
- **Open access** to anyone on network

### Recommendations for Production

1. **Add authentication**
   - Implement login system
   - Use session tokens
   - See ESP32-Sveltekit's auth system for reference

2. **Enable HTTPS**
   - Generate SSL certificates
   - Configure HTTPS server
   - Redirect HTTP → HTTPS

3. **Network isolation**
   - Use AP mode for private access
   - Implement firewall rules
   - Consider VPN for remote access

4. **API security**
   - Add rate limiting
   - Validate all inputs
   - Implement CORS properly

## Next Steps

### Implement More API Endpoints

The web UI expects these endpoints (not yet implemented):

- `/rest/wifi/scan` - Scan for WiFi networks
- `/rest/wifi/connect` - Connect to WiFi
- `/rest/ntp/status` - NTP sync status
- `/rest/mqtt/status` - MQTT connection status
- `/rest/ota/update` - OTA firmware update
- `/rest/system/restart` - Reboot device

### Add Real-Time Updates

Consider adding WebSocket support for live updates:
- System metrics
- Audio chat status
- Real-time logs

### Integrate with AI Chat

Expose AI chat functionality via API:
- `/rest/chat/status` - Current chat state
- `/rest/chat/send` - Send message
- `/rest/chat/history` - Get conversation history

## Additional Resources

- [ESP-IDF HTTP Server Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
- [INTEGRATION_NOTES.md](INTEGRATION_NOTES.md) - Build system details
- [ESP32-Sveltekit GitHub](https://github.com/theelims/ESP32-sveltekit) - Original framework

## Support

For issues or questions:
1. Check the serial monitor logs
2. Review [INTEGRATION_NOTES.md](INTEGRATION_NOTES.md)
3. Check ESP-IDF HTTP server examples
4. Review ESP32-Sveltekit documentation

---

**Your web interface is now live!** 🎉

Just power on your device, connect to the same network, and navigate to the IP address shown in the serial monitor.
