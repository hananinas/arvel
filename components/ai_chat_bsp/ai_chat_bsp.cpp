#include "ai_chat_bsp.h"
#include "esp_wifi_bsp.h"
#include "user_audio_bsp.h"
#include "sdcard_bsp.h"
#include <stdio.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include <cJSON.h>
#include <errno.h>

#define OPENROUTER_URL "https://openrouter.ai/api/v1/chat/completions"
#define OPENROUTER_HTTP_REFERER "https://example.local/esp32"
#define OPENROUTER_TITLE "ESP32 Touch Panel"
#define CHAT_HISTORY_FILE "/sdcard/chat_history.json"
#define MAX_HISTORY_MESSAGES 20  // Keep last 20 messages (10 user + 10 assistant)

#define CHAT_QUEUE_LENGTH 2
#define CHAT_PROMPT_MAX_LEN 512
#define CHAT_TASK_STACK_SIZE 16384
#define CHAT_DEFAULT_PROMPT "Tell me something interesting about the ESP32-S3."

// GTS Root R4 trusted root needed for the openrouter.ai TLS chain.
static const char OPENROUTER_ROOT_CA_PEM[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIDejCCAmKgAwIBAgIQf+UwvzMTQ77dghYQST2KGzANBgkqhkiG9w0BAQsFADBX\n"
	"MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE\n"
	"CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIzMTEx\n"
	"NTAzNDMyMVoXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT\n"
	"GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFI0\n"
	"MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE83Rzp2iLYK5DuDXFgTB7S0md+8Fhzube\n"
	"Rr1r1WEYNa5A3XP3iZEwWus87oV8okB2O6nGuEfYKueSkWpz6bFyOZ8pn6KY019e\n"
	"WIZlD6GEZQbR3IvJx3PIjGov5cSr0R2Ko4H/MIH8MA4GA1UdDwEB/wQEAwIBhjAd\n"
	"BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwDwYDVR0TAQH/BAUwAwEB/zAd\n"
	"BgNVHQ4EFgQUgEzW63T/STaj1dj8tT7FavCUHYwwHwYDVR0jBBgwFoAUYHtmGkUN\n"
	"l8qJUC99BM00qP/8/UswNgYIKwYBBQUHAQEEKjAoMCYGCCsGAQUFBzAChhpodHRw\n"
	"Oi8vaS5wa2kuZ29vZy9nc3IxLmNydDAtBgNVHR8EJjAkMCKgIKAehhxodHRwOi8v\n"
	"Yy5wa2kuZ29vZy9yL2dzcjEuY3JsMBMGA1UdIAQMMAowCAYGZ4EMAQIBMA0GCSqG\n"
	"SIb3DQEBCwUAA4IBAQAYQrsPBtYDh5bjP2OBDwmkoWhIDDkic574y04tfzHpn+cJ\n"
	"odI2D4SseesQ6bDrarZ7C30ddLibZatoKiws3UL9xnELz4ct92vID24FfVbiI1hY\n"
	"+SW6FoVHkNeWIP0GCbaM4C6uVdF5dTUsMVs/ZbzNnIdCp5Gxmx5ejvEau8otR/Cs\n"
	"kGN+hr/W5GvT1tMBjgWKZ1i4//emhA1JG1BbPzoLJQvyEotc03lXjTaCzv8mEbep\n"
	"8RqZ7a2CPsgRbuvTPBwcOMBBmuFeU88+FSBX6+7iP0il8b4Z0QFqIwwMHfs/L6K1\n"
	"vepuoxtGzi4CZ68zJpiq1UvSqTbFJjtbD4seiMHl\n"
	"-----END CERTIFICATE-----\n";

typedef struct {
	char *prompt;
	uint8_t *audio_data;
	size_t audio_size;
	bool is_audio;
} chat_request_t;

typedef struct {
	std::string role;      // "user" or "assistant"
	std::string content;   // Message text
} chat_message_t;

// Module state
static std::string s_api_key = "<REPLACE_WITH_YOUR_OPENROUTER_API_KEY>";
static std::string s_model = "openai/gpt-4o";
static QueueHandle_t chat_request_queue = NULL;
static TaskHandle_t chat_task_handle = NULL;
static EventGroupHandle_t wifi_event_group = NULL;
static bool chat_in_progress = false;
static lv_obj_t *chat_panel = NULL;
static lv_obj_t *chat_status_label = NULL;
static lv_obj_t *chat_output = NULL;
static lv_obj_t *chat_voice_btn = NULL;
static lv_obj_t *chat_toggle_btn = NULL;
static lv_obj_t *chat_toggle_label = NULL;
static bool voice_recording = false;
static bool voice_recorded = false;  // New state: audio ready to send
static uint8_t *voice_buffer = NULL;
static size_t voice_buffer_size = 0;
static std::vector<chat_message_t> chat_history;  // Conversation history

// Forward declarations
static void chat_worker_task(void *arg);
static void chat_voice_event_cb(lv_event_t *e);
static void chat_recording_task(void *arg);
static void create_chat_ui(lv_ui *ui);
static void chat_toggle_event_cb(lv_event_t *e);
static void chat_send_event_cb(lv_event_t *e);
static void chat_update_status(const char *text);
static void chat_update_output(const char *text);
static void chat_textarea_set_read_only(lv_obj_t *obj, bool enable);
static std::string build_chat_payload(const char *prompt);
static std::string build_chat_payload_with_audio(const char *prompt, const uint8_t *audio_data, size_t audio_size);
static std::string base64_encode(const uint8_t *data, size_t len);
static uint8_t* add_wav_header(const uint8_t *pcm_data, size_t pcm_size, size_t *out_wav_size);
static std::string extract_assistant_reply(const std::string &response_body);
static void initialize_sntp(void);
static bool wait_for_time_sync(int timeout_sec);
static void log_http_error_details(esp_http_client_handle_t client, esp_err_t err);
// Chat history functions
static void save_chat_history(void);
static void load_chat_history(void);
static void add_to_history(const std::string &role, const std::string &content);
static void clear_chat_history(void);

static void chat_update_status(const char *text)
{
	if (!chat_status_label) {
		return;
	}
	lv_label_set_text(chat_status_label, (text != nullptr) ? text : "");
}

static void chat_update_output(const char *text)
{
	if (!chat_output) {
		return;
	}
	lv_textarea_set_text(chat_output, (text != nullptr) ? text : "");
	lv_obj_scroll_to_y(chat_output, LV_COORD_MAX, LV_ANIM_OFF);
}

static void chat_textarea_set_read_only(lv_obj_t *obj, bool enable)
{
	if (!obj) {
		return;
	}
#if LVGL_VERSION_MAJOR >= 9
	lv_textarea_set_read_only(obj, enable);
#else
	if (enable) {
		lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
	} else {
		lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
	}
#endif
}

static void chat_toggle_event_cb(lv_event_t *e)
{
	if (!chat_panel) {
		return;
	}
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
		return;
	}
	bool hidden = lv_obj_has_flag(chat_panel, LV_OBJ_FLAG_HIDDEN);
	if (hidden) {
		lv_obj_clear_flag(chat_panel, LV_OBJ_FLAG_HIDDEN);
		if (chat_toggle_label) {
			lv_label_set_text(chat_toggle_label, "Hide Chat");
		}
		if (wifi_event_group) {
			EventBits_t wifi_bits = xEventGroupGetBits(wifi_event_group);
			if ((wifi_bits & WIFI_CONNECTED_BIT) != 0) {
				chat_update_status("Wi-Fi connected");
			} else {
				chat_update_status("Wi-Fi not connected");
			}
		}
	} else {
		lv_obj_add_flag(chat_panel, LV_OBJ_FLAG_HIDDEN);
		if (chat_toggle_label) {
			lv_label_set_text(chat_toggle_label, "AI Chat");
		}
	}
}

static std::string build_chat_payload(const char *prompt)
{
	const char *sanitized_prompt = (prompt && prompt[0] != '\0') ? prompt : CHAT_DEFAULT_PROMPT;
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return std::string();
	}
	if (!cJSON_AddStringToObject(root, "model", s_model.c_str())) {
		cJSON_Delete(root);
		return std::string();
	}
	cJSON *messages = cJSON_AddArrayToObject(root, "messages");
	if (!messages) {
		cJSON_Delete(root);
		return std::string();
	}
	const char *system_text = "You are an assistant running on an ESP32 device display.";
	cJSON *system_msg = cJSON_CreateObject();
	if (!system_msg) {
		cJSON_Delete(root);
		return std::string();
	}
	cJSON_AddStringToObject(system_msg, "role", "system");
	cJSON_AddStringToObject(system_msg, "content", system_text);
	cJSON_AddItemToArray(messages, system_msg);

	cJSON *user_msg = cJSON_CreateObject();
	if (!user_msg) {
		cJSON_Delete(root);
		return std::string();
	}
	cJSON_AddStringToObject(user_msg, "role", "user");
	cJSON_AddStringToObject(user_msg, "content", sanitized_prompt);
	cJSON_AddItemToArray(messages, user_msg);

	char *raw = cJSON_PrintUnformatted(root);
	std::string payload;
	if (raw) {
		payload.assign(raw);
		cJSON_free(raw);
	}
	cJSON_Delete(root);
	return payload;
}

static std::string base64_encode(const uint8_t *data, size_t len)
{
	static const char base64_chars[] = 
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	
	std::string result;
	result.reserve(((len + 2) / 3) * 4);
	
	for (size_t i = 0; i < len; i += 3) {
		uint32_t octet_a = i < len ? data[i] : 0;
		uint32_t octet_b = i + 1 < len ? data[i + 1] : 0;
		uint32_t octet_c = i + 2 < len ? data[i + 2] : 0;
		
		uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
		
		result.push_back(base64_chars[(triple >> 18) & 0x3F]);
		result.push_back(base64_chars[(triple >> 12) & 0x3F]);
		result.push_back(i + 1 < len ? base64_chars[(triple >> 6) & 0x3F] : '=');
		result.push_back(i + 2 < len ? base64_chars[triple & 0x3F] : '=');
	}
	
	return result;
}

static uint8_t* add_wav_header(const uint8_t *pcm_data, size_t pcm_size, size_t *out_wav_size)
{
	// WAV file header structure for PCM data
	// Audio format: 16kHz, 2 channels, 16-bit PCM
	const uint32_t sample_rate = 16000;
	const uint16_t num_channels = 2;
	const uint16_t bits_per_sample = 16;
	const uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
	const uint16_t block_align = num_channels * (bits_per_sample / 8);
	const uint32_t data_size = pcm_size;
	const uint32_t file_size = 36 + data_size;
	
	// Allocate memory for WAV file (44 byte header + PCM data)
	*out_wav_size = 44 + pcm_size;
	uint8_t *wav_data = (uint8_t *)malloc(*out_wav_size);
	if (!wav_data) {
		ESP_LOGE("chat", "Failed to allocate memory for WAV file");
		return NULL;
	}
	
	uint8_t *p = wav_data;
	
	// RIFF header
	memcpy(p, "RIFF", 4); p += 4;
	*(uint32_t*)p = file_size; p += 4;
	memcpy(p, "WAVE", 4); p += 4;
	
	// fmt chunk
	memcpy(p, "fmt ", 4); p += 4;
	*(uint32_t*)p = 16; p += 4;  // fmt chunk size
	*(uint16_t*)p = 1; p += 2;   // audio format (1 = PCM)
	*(uint16_t*)p = num_channels; p += 2;
	*(uint32_t*)p = sample_rate; p += 4;
	*(uint32_t*)p = byte_rate; p += 4;
	*(uint16_t*)p = block_align; p += 2;
	*(uint16_t*)p = bits_per_sample; p += 2;
	
	// data chunk
	memcpy(p, "data", 4); p += 4;
	*(uint32_t*)p = data_size; p += 4;
	
	// Copy PCM data
	memcpy(p, pcm_data, pcm_size);
	
	ESP_LOGI("chat", "Created WAV file: %d bytes (header: 44, data: %d)", *out_wav_size, pcm_size);
	return wav_data;
}

static std::string build_chat_payload_with_audio(const char *prompt, const uint8_t *audio_data, size_t audio_size)
{
	const char *sanitized_prompt = (prompt && prompt[0] != '\0') ? prompt : "Please transcribe this audio.";
	
	// First, convert raw PCM to WAV format with proper headers
	size_t wav_size = 0;
	uint8_t *wav_data = add_wav_header(audio_data, audio_size, &wav_size);
	if (!wav_data) {
		ESP_LOGE("chat", "Failed to create WAV file");
		return std::string();
	}
	
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		ESP_LOGE("chat", "Failed to create JSON root");
		free(wav_data);
		return std::string();
	}
	
	// Set model - use a model that supports audio input
	if (!cJSON_AddStringToObject(root, "model", s_model.c_str())) {
		cJSON_Delete(root);
		free(wav_data);
		ESP_LOGE("chat", "Failed to add model");
		return std::string();
	}
	
	// Create messages array
	cJSON *messages = cJSON_AddArrayToObject(root, "messages");
	if (!messages) {
		cJSON_Delete(root);
		free(wav_data);
		ESP_LOGE("chat", "Failed to create messages array");
		return std::string();
	}
	
	// Add system message
	cJSON *system_msg = cJSON_CreateObject();
	if (!system_msg) {
		cJSON_Delete(root);
		free(wav_data);
		ESP_LOGE("chat", "Failed to create system message");
		return std::string();
	}
	cJSON_AddStringToObject(system_msg, "role", "system");
	cJSON_AddStringToObject(system_msg, "content", "You are an assistant running on an ESP32 device display. Process audio input and respond appropriately.");
	cJSON_AddItemToArray(messages, system_msg);
	
	// Add user message with audio content
	cJSON *user_msg = cJSON_CreateObject();
	if (!user_msg) {
		cJSON_Delete(root);
		free(wav_data);
		ESP_LOGE("chat", "Failed to create user message");
		return std::string();
	}
	cJSON_AddStringToObject(user_msg, "role", "user");
	
	// Create content array for multimodal content
	cJSON *content = cJSON_AddArrayToObject(user_msg, "content");
	if (!content) {
		cJSON_Delete(root);
		free(wav_data);
		ESP_LOGE("chat", "Failed to create content array");
		return std::string();
	}
	
	// Add text content
	cJSON *text_content = cJSON_CreateObject();
	if (!text_content) {
		cJSON_Delete(root);
		free(wav_data);
		ESP_LOGE("chat", "Failed to create text content");
		return std::string();
	}
	cJSON_AddStringToObject(text_content, "type", "text");
	cJSON_AddStringToObject(text_content, "text", sanitized_prompt);
	cJSON_AddItemToArray(content, text_content);
	
	// Encode WAV audio to base64
	ESP_LOGI("chat", "Encoding %d bytes of WAV audio to base64", wav_size);
	std::string base64_audio = base64_encode(wav_data, wav_size);
	ESP_LOGI("chat", "Base64 encoded audio size: %d bytes", base64_audio.length());
	
	// Free WAV data after encoding
	free(wav_data);
	
	// Add audio content
	cJSON *audio_content = cJSON_CreateObject();
	if (!audio_content) {
		cJSON_Delete(root);
		ESP_LOGE("chat", "Failed to create audio content");
		return std::string();
	}
	cJSON_AddStringToObject(audio_content, "type", "input_audio");
	
	cJSON *input_audio = cJSON_CreateObject();
	if (!input_audio) {
		cJSON_Delete(root);
		ESP_LOGE("chat", "Failed to create input_audio object");
		return std::string();
	}
	cJSON_AddStringToObject(input_audio, "data", base64_audio.c_str());
	cJSON_AddStringToObject(input_audio, "format", "wav");
	
	cJSON_AddItemToObject(audio_content, "input_audio", input_audio);
	cJSON_AddItemToArray(content, audio_content);
	
	cJSON_AddItemToArray(messages, user_msg);
	
	char *raw = cJSON_PrintUnformatted(root);
	std::string payload;
	if (raw) {
		payload.assign(raw);
		cJSON_free(raw);
		ESP_LOGI("chat", "Payload built successfully, size: %d bytes", payload.length());
	} else {
		ESP_LOGE("chat", "Failed to serialize JSON");
	}
	cJSON_Delete(root);
	return payload;
}

static std::string extract_assistant_reply(const std::string &response_body)
{
	if (response_body.empty()) {
		ESP_LOGW("chat", "Response body is empty");
		return std::string();
	}
	
	ESP_LOGI("chat", "Parsing JSON response (%d bytes)", response_body.length());
	cJSON *root = cJSON_Parse(response_body.c_str());
	if (!root) {
		ESP_LOGW("chat", "Failed to parse OpenRouter response as JSON");
		std::string preview = response_body.substr(0, 100);
		for (char &c : preview) {
			if (c == '\n' || c == '\r') c = ' ';
		}
		ESP_LOGW("chat", "Response start: %s", preview.c_str());
		return std::string();
	}
	std::string result;
	cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
	if (choices && cJSON_IsArray(choices)) {
		ESP_LOGI("chat", "Found choices array");
		cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
		if (first_choice) {
			cJSON *message = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
			if (message) {
				cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
				if (cJSON_IsString(content) && content->valuestring) {
					result = content->valuestring;
					ESP_LOGI("chat", "Extracted content (%d bytes)", result.length());
				} else {
					ESP_LOGW("chat", "Content field not found or not a string");
				}
			} else {
				ESP_LOGW("chat", "Message field not found");
			}
		} else {
			ESP_LOGW("chat", "First choice not found");
		}
	} else {
		ESP_LOGW("chat", "Choices array not found");
	}
	if (result.empty()) {
		cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
		if (error_obj) {
			cJSON *error_msg = cJSON_GetObjectItemCaseSensitive(error_obj, "message");
			if (cJSON_IsString(error_msg) && error_msg->valuestring) {
				result = error_msg->valuestring;
				ESP_LOGI("chat", "Found error message: %s", result.c_str());
			}
		}
	}
	cJSON_Delete(root);
	return result;
}

static void initialize_sntp(void)
{
	ESP_LOGI("sntp", "Initializing SNTP");
	esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
	esp_sntp_setservername(0, "pool.ntp.org");
	esp_sntp_init();
}

static bool wait_for_time_sync(int timeout_sec)
{
	int retry = 0;
	const int retry_count = timeout_sec * 2;
	
	while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < retry_count) {
		ESP_LOGI("sntp", "Waiting for system time to be set... (%d/%d)", retry, retry_count);
		vTaskDelay(pdMS_TO_TICKS(500));
		retry++;
	}
	
	if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
		time_t now;
		struct tm timeinfo;
		time(&now);
		localtime_r(&now, &timeinfo);
		ESP_LOGI("sntp", "Time synchronized: %s", asctime(&timeinfo));
		return true;
	}
	
	ESP_LOGW("sntp", "Failed to synchronize time within %d seconds", timeout_sec);
	return false;
}

static void log_http_error_details(esp_http_client_handle_t client, esp_err_t err)
{
	int client_errno = 0;
	if (client) {
		client_errno = esp_http_client_get_errno(client);
	}
	if (client_errno != 0) {
		ESP_LOGE("chat", "HTTP request failed: %s (%d), errno=%d (%s)",
			 esp_err_to_name(err), err, client_errno, strerror(client_errno));
	} else {
		ESP_LOGE("chat", "HTTP request failed: %s (%d)", esp_err_to_name(err), err);
	}
}

static void chat_recording_task(void *arg)
{
	ESP_LOGI("chat", "Recording task started");
	
	// Record audio in chunks (like the working example)
	audio_playback_read_chunked(voice_buffer, voice_buffer_size, 512);
	
	ESP_LOGI("chat", "Audio recording completed");
	
	// Auto-transition to review state
	voice_recording = false;
	voice_recorded = true;
	
	// Update button label
	lv_obj_t *label = lv_obj_get_child(chat_voice_btn, 0);
	if (label) {
		lv_label_set_text(label, LV_SYMBOL_CALL " Send");
	}
	
	chat_update_status("Playing back audio...");
	
	// Play back the recorded audio so user can hear it
	audio_playback_write(voice_buffer, voice_buffer_size);
	
	// Update status after playback starts
	vTaskDelay(pdMS_TO_TICKS(200));
	chat_update_status("Tap Send or Voice again");
	
	// Task deletes itself
	vTaskDelete(NULL);
}

static void chat_voice_event_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
		return;
	}
	
	if (chat_in_progress) {
		chat_update_status("Chat busy");
		return;
	}
	
	// State machine: idle -> recording -> recorded (playing back) -> send
	if (!voice_recording && !voice_recorded) {
		// State 1: Start recording
		voice_recording = true;
		chat_update_status("Recording voice...");
		
		// Allocate buffer for 5 seconds of audio: 16kHz * 2 channels * 2 bytes * 5 seconds
		voice_buffer_size = 16000 * 2 * 2 * 5;
		voice_buffer = (uint8_t *)heap_caps_malloc(voice_buffer_size, MALLOC_CAP_SPIRAM);
		
		if (!voice_buffer) {
			chat_update_status("Out of memory");
			voice_recording = false;
			return;
		}
		
		// Update button label immediately so UI is responsive
		lv_obj_t *label = lv_obj_get_child(chat_voice_btn, 0);
		if (label) {
			lv_label_set_text(label, LV_SYMBOL_STOP " Recording");
		}
		
		// Force UI update
		lv_refr_now(NULL);
		
		ESP_LOGI("chat", "Starting audio recording: %d bytes", voice_buffer_size);
		
		// Start recording in background task so UI stays responsive
		xTaskCreatePinnedToCore(chat_recording_task, "chat_record", 4096, NULL, 5, NULL, 1);
		
	} else if (!voice_recording && voice_recorded) {
		// State 2: User confirmed, send to AI
		voice_recorded = false;
		
		// Update button label back to normal
		lv_obj_t *label = lv_obj_get_child(chat_voice_btn, 0);
		if (label) {
			lv_label_set_text(label, LV_SYMBOL_AUDIO " Voice");
		}
		
		chat_update_status("Encoding audio...");
		
		// Create a copy of the audio buffer for the request
		uint8_t *audio_copy = (uint8_t *)malloc(voice_buffer_size);
		if (!audio_copy) {
			chat_update_status("Out of memory");
			if (voice_buffer) {
				free(voice_buffer);
				voice_buffer = NULL;
			}
			return;
		}
		memcpy(audio_copy, voice_buffer, voice_buffer_size);
		
		ESP_LOGI("chat", "Sending %d bytes of audio to AI", voice_buffer_size);
		
		// Send audio directly to OpenRouter
		if (chat_request_queue) {
			chat_request_t request = {
				.prompt = strdup("Please transcribe and respond to this audio."),
				.audio_data = audio_copy,
				.audio_size = voice_buffer_size,
				.is_audio = true
			};
			
			if (xQueueSend(chat_request_queue, &request, pdMS_TO_TICKS(10)) == pdPASS) {
				chat_in_progress = true;
				if (chat_voice_btn) {
					lv_obj_add_state(chat_voice_btn, LV_STATE_DISABLED);
				}
				chat_update_status("Sending audio to AI...");
				chat_update_output("");
			} else {
				free(request.prompt);
				free(audio_copy);
				chat_update_status("Chat queue full");
			}
		} else {
			free(audio_copy);
			chat_update_status("Chat queue missing");
		}
		
		// Clean up voice buffer
		if (voice_buffer) {
			free(voice_buffer);
			voice_buffer = NULL;
		}
	}
}

static void create_chat_ui(lv_ui *ui)
{
	if (!ui || chat_panel) {
		return;
	}

	chat_toggle_btn = lv_btn_create(ui->screen_carousel_1_element_2);
	lv_obj_set_size(chat_toggle_btn, 150, 36);
	lv_obj_set_pos(chat_toggle_btn, 11, 380);
	chat_toggle_label = lv_label_create(chat_toggle_btn);
	lv_label_set_text(chat_toggle_label, "AI Chat");
	lv_obj_center(chat_toggle_label);
	lv_obj_add_event_cb(chat_toggle_btn, chat_toggle_event_cb, LV_EVENT_CLICKED, NULL);

	chat_panel = lv_obj_create(ui->screen);
	lv_obj_set_size(chat_panel, 172, 360);
	lv_obj_align(chat_panel, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_opa(chat_panel, LV_OPA_80, LV_PART_MAIN);
	lv_obj_set_style_bg_color(chat_panel, lv_color_hex(0x20232a), LV_PART_MAIN);
	lv_obj_set_style_pad_all(chat_panel, 8, LV_PART_MAIN);
	lv_obj_set_style_border_width(chat_panel, 1, LV_PART_MAIN);
	lv_obj_set_style_border_color(chat_panel, lv_color_hex(0x4c566a), LV_PART_MAIN);
	lv_obj_add_flag(chat_panel, LV_OBJ_FLAG_HIDDEN);

	chat_status_label = lv_label_create(chat_panel);
	lv_label_set_text(chat_status_label, "AI chat idle");
	lv_obj_set_width(chat_status_label, LV_PCT(100));
	lv_obj_set_style_text_color(chat_status_label, lv_color_hex(0xffffff), LV_PART_MAIN);

	// Make output larger since we removed input area
	chat_output = lv_textarea_create(chat_panel);
	lv_obj_set_size(chat_output, 156, 272);  // Increased height from 200 to 272
	lv_obj_set_pos(chat_output, 0, 32);
	lv_textarea_set_text(chat_output, "");
	lv_textarea_set_max_length(chat_output, 1024);
	lv_textarea_set_cursor_click_pos(chat_output, false);
	chat_textarea_set_read_only(chat_output, true);

	// Voice button - centered at bottom
	chat_voice_btn = lv_btn_create(chat_panel);
	lv_obj_set_size(chat_voice_btn, 156, 40);  // Full width
	lv_obj_set_pos(chat_voice_btn, 0, 308);
	lv_obj_add_event_cb(chat_voice_btn, chat_voice_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *voice_label = lv_label_create(chat_voice_btn);
	lv_label_set_text(voice_label, LV_SYMBOL_AUDIO " Voice");
	lv_obj_center(voice_label);

	chat_update_status("Wi-Fi not connected");
}

static void chat_worker_task(void *arg)
{
	(void)arg;
	
	chat_request_t request;
	static bool sntp_initialized = false;
	
	while (true) {
		ESP_LOGI("chat", "=== Worker task waiting for chat request from queue ===");
		if (xQueueReceive(chat_request_queue, &request, portMAX_DELAY) != pdPASS) {
			ESP_LOGW("chat", "Queue receive failed");
			continue;
		}
		ESP_LOGI("chat", "=== Received chat request from queue (audio: %s) ===", request.is_audio ? "yes" : "no");
		if (!request.prompt) {
			ESP_LOGW("chat", "Received null prompt");
			if (request.is_audio && request.audio_data) {
				free(request.audio_data);
			}
			continue;
		}
		ESP_LOGI("chat", "=== Chat request prompt: '%s' ===", request.prompt);

		if (s_api_key == "<REPLACE_WITH_YOUR_OPENROUTER_API_KEY>" || s_api_key.empty()) {
			chat_update_status("Set OpenRouter API key");
			chat_update_output("");
			free(request.prompt);
			if (request.is_audio && request.audio_data) {
				free(request.audio_data);
			}
			chat_in_progress = false;
			if (chat_voice_btn) {
				lv_obj_clear_state(chat_voice_btn, LV_STATE_DISABLED);
			}
			continue;
		}

		EventBits_t wifi_bits = wifi_event_group ? xEventGroupGetBits(wifi_event_group) : 0;
		ESP_LOGI("chat", "=== WiFi check: event_group=%p, bits=0x%lx, WIFI_CONNECTED_BIT=0x%lx ===", 
			wifi_event_group, (unsigned long)wifi_bits, (unsigned long)WIFI_CONNECTED_BIT);
		if ((wifi_bits & WIFI_CONNECTED_BIT) == 0) {
			ESP_LOGW("chat", "=== WiFi NOT connected! bits=0x%lx, expected bit=0x%lx ===", 
				(unsigned long)wifi_bits, (unsigned long)WIFI_CONNECTED_BIT);
			chat_update_status("Wi-Fi not connected");
			vTaskDelay(pdMS_TO_TICKS(100));
			chat_update_output("");
			free(request.prompt);
			if (request.is_audio && request.audio_data) {
				free(request.audio_data);
			}
			chat_in_progress = false;
			if (chat_voice_btn) {
				lv_obj_clear_state(chat_voice_btn, LV_STATE_DISABLED);
			}
			continue;
		}
		ESP_LOGI("chat", "=== WiFi connected! Proceeding with request ===");
		
		// Initialize SNTP on first request after WiFi is connected
		if (!sntp_initialized) {
			ESP_LOGI("chat", "WiFi connected, initializing SNTP for time sync");
			chat_update_status("Syncing time...");
			initialize_sntp();
			if (wait_for_time_sync(10)) {
				ESP_LOGI("chat", "Time synchronized, ready for HTTPS");
				chat_update_status("Time synced");
			} else {
				ESP_LOGW("chat", "Time sync timeout, proceeding anyway");
				chat_update_status("Time sync timeout");
			}
			sntp_initialized = true;
			vTaskDelay(pdMS_TO_TICKS(500));
		}

		ESP_LOGI("chat", "=== Processing prompt: %s ===", request.prompt);
		chat_update_status("Building request...");
		vTaskDelay(pdMS_TO_TICKS(100));  // Give UI time to update
		
		std::string payload;
		if (request.is_audio && request.audio_data && request.audio_size > 0) {
			ESP_LOGI("chat", "=== Building audio payload with %d bytes of audio ===", request.audio_size);
			payload = build_chat_payload_with_audio(request.prompt, request.audio_data, request.audio_size);
			// Free audio data after encoding
			free(request.audio_data);
			request.audio_data = NULL;
		} else {
			payload = build_chat_payload(request.prompt);
		}
		
		std::string response_body;
		esp_err_t err = ESP_FAIL;
		int http_status = -1;
		bool output_set = false;

		if (!payload.empty()) {
			ESP_LOGI("chat", "=== Payload built successfully, size: %d bytes ===", payload.length());
			chat_update_status("Connecting to OpenRouter...");
			vTaskDelay(pdMS_TO_TICKS(200));  // Give UI time to update
			ESP_LOGI("chat", "=== Starting HTTP client connection ===");
			
			// Build Authorization header
			std::string auth_header = "Bearer ";
			auth_header += s_api_key;
			
			esp_http_client_config_t config = {};
			config.url = OPENROUTER_URL;
			config.timeout_ms = 45000;
			config.user_data = &response_body;
			config.event_handler = NULL;  // Disable event handler, we'll read manually
			config.cert_pem = OPENROUTER_ROOT_CA_PEM;
			config.skip_cert_common_name_check = false;
			config.buffer_size = 8192;
			config.buffer_size_tx = 4096;
			config.is_async = false;
			config.keep_alive_enable = true;
			config.keep_alive_idle = 5;
			config.keep_alive_interval = 5;
			config.keep_alive_count = 3;
			config.method = HTTP_METHOD_POST;
			
			ESP_LOGI("chat", "=== Initializing HTTP client ===");
			esp_http_client_handle_t client = esp_http_client_init(&config);
			if (client) {
				ESP_LOGI("chat", "=== HTTP client initialized successfully ===");
				chat_update_status("Setting headers...");
				vTaskDelay(pdMS_TO_TICKS(100));
				
				// Set headers BEFORE opening connection
				ESP_LOGI("chat", "=== Setting HTTP headers ===");
				esp_http_client_set_header(client, "Content-Type", "application/json");
				esp_http_client_set_header(client, "Authorization", auth_header.c_str());
				esp_http_client_set_header(client, "HTTP-Referer", OPENROUTER_HTTP_REFERER);
				esp_http_client_set_header(client, "X-Title", OPENROUTER_TITLE);
				
				ESP_LOGI("chat", "=== Opening HTTP connection to %s ===", OPENROUTER_URL);
				chat_update_status("Opening connection...");
				vTaskDelay(pdMS_TO_TICKS(100));  // Let UI update
				
				err = esp_http_client_open(client, payload.length());
				ESP_LOGI("chat", "=== esp_http_client_open result: %s (0x%x) ===", esp_err_to_name(err), err);
				
				if (err != ESP_OK) {
					ESP_LOGE("chat", "=== FAILED to open HTTP connection: %s (0x%x) ===", esp_err_to_name(err), err);
					log_http_error_details(client, err);
					esp_http_client_cleanup(client);
					chat_update_status("Connection failed");
					free(request.prompt);
					chat_in_progress = false;
					if (chat_voice_btn) {
						lv_obj_clear_state(chat_voice_btn, LV_STATE_DISABLED);
					}
					continue;
				}
				
				ESP_LOGI("chat", "=== Connection opened successfully, writing POST data (%d bytes) ===", payload.length());
				chat_update_status("Sending request...");
				vTaskDelay(pdMS_TO_TICKS(100));
				
				int wlen = esp_http_client_write(client, payload.c_str(), payload.length());
				ESP_LOGI("chat", "=== Write result: %d bytes (expected %d) ===", wlen, payload.length());
				
				if (wlen < 0) {
					ESP_LOGE("chat", "=== FAILED to write POST data: %d ===", wlen);
					esp_http_client_cleanup(client);
					chat_update_status("Write failed");
					free(request.prompt);
					chat_in_progress = false;
					if (chat_voice_btn) {
						lv_obj_clear_state(chat_voice_btn, LV_STATE_DISABLED);
					}
					continue;
				}
				
				ESP_LOGI("chat", "=== Wrote %d bytes successfully, fetching response headers ===", wlen);
				chat_update_status("Waiting for response...");
				vTaskDelay(pdMS_TO_TICKS(100));
				
				int content_length = esp_http_client_fetch_headers(client);
				http_status = esp_http_client_get_status_code(client);
				ESP_LOGI("chat", "=== Fetch headers complete: Status=%d, Content-Length=%d ===", http_status, content_length);
				http_status = esp_http_client_get_status_code(client);
				
				ESP_LOGI("chat", "HTTP Status: %d, Content-Length: %d", http_status, content_length);
				
				if (http_status >= 200 && http_status < 300) {
					chat_update_status("Reading AI response...");
					// Read response data
					char buffer[1024];
					int read_len;
					response_body.clear(); // Clear any event handler data
					int total_read = 0;
					while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1)) > 0) {
						buffer[read_len] = '\0';
						response_body.append(buffer, read_len);
						total_read += read_len;
						if (total_read % 2048 == 0) {
							ESP_LOGI("chat", "Read %d bytes so far...", total_read);
						}
					}
					
					ESP_LOGI("chat", "Finished reading response, total: %d bytes", total_read);
					chat_update_status("Parsing response...");
					if (!response_body.empty()) {
						ESP_LOGI("chat", "Response size: %d", response_body.length());
						// Log first 200 chars for debugging
						std::string preview = response_body.substr(0, 200);
						// Replace newlines with spaces for logging
						for (char &c : preview) {
							if (c == '\n' || c == '\r') c = ' ';
						}
						ESP_LOGI("chat", "Response preview: %s", preview.c_str());
					}
					err = ESP_OK;
				} else {
					// Read error response
					char buffer[512];
					int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
					if (read_len > 0) {
						buffer[read_len] = '\0';
						response_body.assign(buffer, read_len);
						ESP_LOGW("chat", "Error response: %s", response_body.c_str());
					}
					
					char status_msg[64];
					snprintf(status_msg, sizeof(status_msg), "OpenRouter HTTP %d", http_status);
					chat_update_status(status_msg);
					err = ESP_FAIL;
				}
				
				esp_http_client_close(client);
				esp_http_client_cleanup(client);
			} else {
				ESP_LOGE("chat", "Failed to init HTTP client");
				chat_update_status("HTTP client init failed");
			}
		} else {
			ESP_LOGE("chat", "Failed to build OpenRouter payload");
			chat_update_status("Prompt invalid");
			chat_update_output("Unable to prepare OpenRouter request");
			output_set = true;
		}

		if (!response_body.empty()) {
			std::string assistant_text = extract_assistant_reply(response_body);
			if (!assistant_text.empty()) {
				if (http_status >= 200 && http_status < 300) {
					chat_update_output(assistant_text.c_str());
				} else {
					std::string error_detail = "OpenRouter response: " + assistant_text;
					chat_update_output(error_detail.c_str());
				}
				output_set = true;
			} else {
				chat_update_output("Unable to parse OpenRouter response");
				output_set = true;
			}
		} else {
			if (!output_set) {
				if (err == ESP_OK && http_status >= 200 && http_status < 300) {
					chat_update_output("Empty response from OpenRouter");
					output_set = true;
				} else if (err != ESP_OK) {
					chat_update_output(esp_err_to_name(err));
					output_set = true;
				}
			}
		}

		free(request.prompt);

		chat_in_progress = false;
		if (chat_voice_btn) {
			lv_obj_clear_state(chat_voice_btn, LV_STATE_DISABLED);
		}
	}
}

// Public API Implementation
void ai_chat_init(lv_ui *ui, EventGroupHandle_t wifi_evt_group)
{
	if (!ui) {
		ESP_LOGE("ai_chat", "UI pointer is NULL");
		return;
	}
	
	wifi_event_group = wifi_evt_group;
	
	if (!chat_request_queue) {
		chat_request_queue = xQueueCreate(CHAT_QUEUE_LENGTH, sizeof(chat_request_t));
	}
	
	create_chat_ui(ui);
	
	if (chat_request_queue && chat_task_handle == NULL) {
		xTaskCreatePinnedToCore(chat_worker_task, "chat_worker_task", CHAT_TASK_STACK_SIZE, NULL, 4, &chat_task_handle, 1);
	}
	
	ESP_LOGI("ai_chat", "AI Chat component initialized");
}

void ai_chat_set_api_key(const char *api_key)
{
	if (api_key && api_key[0] != '\0') {
		s_api_key = api_key;
		ESP_LOGI("ai_chat", "API key updated");
	}
}

void ai_chat_set_model(const char *model)
{
	if (model && model[0] != '\0') {
		s_model = model;
		ESP_LOGI("ai_chat", "Model set to: %s", model);
	}
}
