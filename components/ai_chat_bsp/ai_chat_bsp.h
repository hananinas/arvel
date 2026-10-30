#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "desk_buddy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_CHAT_ACCEPTED = 0,
    AI_CHAT_BUSY,
    AI_CHAT_OFFLINE,
    AI_CHAT_NOT_CONFIGURED,
    AI_CHAT_INVALID_INPUT,
    AI_CHAT_NO_MEMORY,
    AI_CHAT_STORAGE_ERROR,
    AI_CHAT_FORBIDDEN
} ai_chat_submit_result_t;


/* Assigned by trusted firmware callers, never decoded from client JSON.
 * NATIVE includes the explicit owner-session/same-origin device handlers;
 * unauthenticated/AP legacy HTTP remains isolated and cannot select NATIVE. */
typedef enum {
    AI_CHAT_NATIVE,
    AI_CHAT_LEGACY_HTTP,
    AI_CHAT_RUNTIME_CHANNEL
} ai_chat_source_t;
/* Initializes the actual Rust agent worker and desk-buddy LVGL interface. */
void ai_chat_init(EventGroupHandle_t wifi_event_group);

typedef enum {
    AI_CHAT_USE_DEFAULT_SPEECH,
    AI_CHAT_SILENT,
    AI_CHAT_READ_ALOUD
} ai_chat_speech_t;

/* Text input is bounded to 8192 UTF-8 bytes; one operation is active at a time.
 * DEFAULT uses saved preferences; explicit SILENT/READ_ALOUD override them.
 * Owner HTTP must pass its token; physical/AP callers pass NULL. The private
 * token is copied into the job and rechecked before effects, never archived. */
ai_chat_submit_result_t ai_chat_submit_text(ai_chat_source_t source, const char *text,
    ai_chat_speech_t speech, const char *owner_session_token);
ai_chat_submit_result_t ai_chat_reset(ai_chat_source_t source);
/* Trusted native C caller only; never expose this as generic HTTP execution.
 * Copies arguments before returning. Rejected submissions do not call back.
 * Accepted submissions complete exactly once, including cancellation/staleness.
 * Completion runs on the serial registry worker, never LVGL or a second LLM.
 * Requests/outcomes use the durable SD ledger when ready; local controls can
 * operate explicitly non-durably when SD is missing. */
ai_chat_submit_result_t ai_chat_submit_desk_tool(const char *tool, const cJSON *arguments,
    desk_buddy_reply_callback_t callback, void *context);
/* Live modes: first tap starts native/Codex speech; later taps mute/unmute.
 * Text pipeline: record/stop/send through the explicitly saved STT model.
 * Owner HTTP passes its token; physical controls pass NULL. */
ai_chat_submit_result_t ai_chat_voice_toggle(ai_chat_source_t source, const char *owner_session_token);
/* Ends live voice without clearing history. Safe during connection or playback. */
ai_chat_submit_result_t ai_chat_voice_stop(ai_chat_source_t source);
/* Owner/native worker-only memory transitions. UUIDs are validated; new/close
 * need no UUID. Resume restores a bounded complete session and keeps mic off. */
ai_chat_submit_result_t ai_chat_memory_action(ai_chat_source_t source, const char *action, const char *uuid);
/* Fixed authenticated HTTP routes only. Tokens are privately copied, checked
 * again immediately before mutation, zeroized on job release, never archived. */
ai_chat_submit_result_t ai_chat_owner_memory_action(const char *action, const char *uuid, const char *owner_session_token);
/* Asynchronous authenticated health only; never opens microphone/inference. */
ai_chat_submit_result_t ai_chat_check_native_readiness(ai_chat_source_t source);

/* Physical top buttons and explicitly authorized owner device controls
 * (handled off the LVGL thread; applied by the render loop). Cycle steps
 * brightness 20→40→60→80→100→20 and persists it when the
 * agent is idle; while the screen is off it only wakes. Toggle forces the
 * screen off/on regardless of the idle sleep timeout. */
bool ai_chat_cycle_brightness(void);
bool ai_chat_toggle_screen(void); /* returns true when the screen is now off */

/* Settings writes are idle-only and persist before replacing live settings.
 * Keys are write-only: all returned JSON is redacted. Caller frees JSON.
 * Errors contain only fixed validation/storage descriptions, never values. */
char *ai_chat_settings_json(void);
ai_chat_submit_result_t ai_chat_update_settings(const cJSON *patch, char *error, size_t error_size);

/* Caller frees the returned JSON string with free(). Never includes credentials.
 * Contains model, state, status, busy, wifiConnected, clockReady,
 * chatConfigured, buddyName, scene, readAloud, voiceConfigured, voicePreview,
 * voiceLiveAvailable, voiceActive, voiceMuted, voiceConnecting, voicePhase,
 * voiceTranscript {role,text,final} or null, lastError, messageRevision,
 * ssid, ip, firmware, localTime ("HH:MM" or ""), preferences, screenOff
 * (manual screen toggle), internalFree, internalLargest, psramFree (bytes),
 * storage {state,conversationId,sqliteBytes,sqliteHighwater,databaseBytes,durable},
 * conversationId, nativeReadiness {state,error,exercised:false}, tools, sources,
 * and up to eight messages {id,role,text,time,tools,sources,interrupted}.
 * state is idle, clock_sync, recording, preview, transcribing, thinking,
 * speaking, error, listening, muted or voice_connecting. Returns NULL on allocation failure. */
char *ai_chat_snapshot_json(ai_chat_source_t source);

#ifdef __cplusplus
}
#endif
