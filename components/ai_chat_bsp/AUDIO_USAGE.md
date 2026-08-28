# Audio Input Support for AI Chat BSP

## Overview

The AI Chat BSP component now supports sending audio directly to OpenRouter's compatible models (like Google Gemini 2.5 Flash) without requiring local speech-to-text conversion. The audio is base64-encoded and sent as part of the chat completion request.

## Supported Features

- **Direct Audio Input**: Send recorded audio directly to AI models that support audio processing
- **Automatic Base64 Encoding**: Audio data is automatically encoded to base64 format
- **Multimodal Content**: Supports sending both text prompts and audio in the same request
- **Native OpenRouter Support**: Uses OpenRouter's native `input_audio` content type

## Setup

### 1. Select an Audio-Capable Model

Before using audio features, set your model to one that supports audio input:

```cpp
// In your main application code
ai_chat_set_model("google/gemini-2.5-flash");
// or
ai_chat_set_model("google/gemini-2.0-flash-exp");
```

You can find audio-capable models on the [OpenRouter Models page](https://openrouter.ai/models) by filtering for "audio input" modality.

### 2. Set Your API Key

```cpp
ai_chat_set_api_key("your-openrouter-api-key-here");
```

## Usage

### Voice Button

The UI includes a voice button with a three-step workflow for audio recording and review:

1. **Press the Voice Button**: Starts recording audio (button shows "Stop")
2. **Press Again to Stop**: Stops recording and plays back the audio for review (button shows "Send")
3. **Press to Send**: Confirms and sends the audio to the AI (or wait and press again to re-record)

The workflow ensures you can:
- Review what was recorded by hearing the playback
- Verify audio quality before sending
- Cancel and re-record if needed

**Audio Processing Steps:**
- Raw PCM audio is captured from the microphone
- A proper WAV file header is added (44 bytes)
- The complete WAV file is base64 encoded
- The encoded audio is packaged in OpenRouter's multimodal format
- Sent to the AI model for transcription and response

### Audio Format

The current implementation records audio with these specifications:
- **Sample Rate**: 16kHz
- **Channels**: 2 (Stereo)
- **Bit Depth**: 16-bit (2 bytes per sample)
- **Duration**: 5 seconds maximum
- **Format**: RAW PCM converted to WAV with proper headers
- **WAV Header**: 44 bytes added automatically

The raw PCM data is converted to a proper WAV file format before encoding, ensuring compatibility with AI models.

### Memory Requirements

Audio recording requires significant memory:
- 5 seconds of audio = 320KB (16000 Hz × 2 channels × 2 bytes × 5 seconds)
- Base64 encoding increases size by ~33%
- Total payload can be ~400-500KB

The implementation uses SPIRAM for audio buffering when available.

## Supported Audio Formats

OpenRouter supports the following audio formats:
- WAV
- MP3
- FLAC
- AAC
- OGG

Currently, the implementation sends audio as WAV format.

## API Structure

The audio is sent using OpenRouter's multimodal content structure:

```json
{
  "model": "google/gemini-2.5-flash",
  "messages": [
    {
      "role": "user",
      "content": [
        {
          "type": "text",
          "text": "Please transcribe and respond to this audio."
        },
        {
          "type": "input_audio",
          "input_audio": {
            "data": "<base64-encoded-audio>",
            "format": "wav"
          }
        }
      ]
    }
  ]
}
```

## Implementation Details

### Key Functions

1. **`base64_encode()`**: Encodes binary audio data to base64 string
2. **`build_chat_payload_with_audio()`**: Creates JSON payload with audio content
3. **`chat_voice_event_cb()`**: Handles voice button clicks and audio recording
4. **`chat_worker_task()`**: Processes both text and audio requests

### Request Structure

The `chat_request_t` structure now includes:
```cpp
typedef struct {
    char *prompt;           // Text prompt
    uint8_t *audio_data;    // Audio buffer
    size_t audio_size;      // Audio buffer size
    bool is_audio;          // Flag for audio requests
} chat_request_t;
```

## Troubleshooting

### "Out of memory" Error
- Ensure your ESP32 has SPIRAM enabled
- Reduce audio recording duration
- Check available heap before recording

### "Model does not support audio"
- Verify you're using an audio-capable model (e.g., `google/gemini-2.5-flash`)
- Check the model list at https://openrouter.ai/models

### Poor Audio Quality
- Adjust sample rate in the code (currently 16kHz)
- Ensure microphone hardware is properly configured
- Check audio input levels

### Timeout Errors
- Increase HTTP timeout in the code (currently 45000ms)
- Check network connection stability
- Consider reducing audio duration

## Example Usage in Main Code

```cpp
#include "ai_chat_bsp.h"

void app_main(void)
{
    // ... initialize WiFi, LVGL, etc ...
    
    // Initialize AI chat with audio support
    ai_chat_init(&guider_ui, wifi_event_group);
    
    // Set an audio-capable model
    ai_chat_set_model("google/gemini-2.5-flash");
    
    // Set your API key
    ai_chat_set_api_key(CONFIG_OPENROUTER_API_KEY);
    
    // The voice button in the UI now supports direct audio input!
}
```

## Performance Considerations

- **Encoding Time**: Base64 encoding 320KB takes ~50-100ms on ESP32-S3
- **Network Transfer**: Sending 400KB payload takes 2-5 seconds on typical WiFi
- **Processing Time**: AI model processing time varies by model (2-10 seconds typical)

## Future Enhancements

Potential improvements for the audio feature:
- Support for different audio formats (MP3, FLAC)
- Adjustable recording duration
- Audio compression before encoding
- Streaming audio support
- Multiple audio file support in one request
- Voice activity detection (VAD) for automatic stop

## References

- [OpenRouter Audio Documentation](https://openrouter.ai/docs/audio-inputs)
- [OpenRouter Models with Audio Support](https://openrouter.ai/models?supported_parameters=audio_input)
- [ESP32-S3 Audio Development](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/audio-development.html)
