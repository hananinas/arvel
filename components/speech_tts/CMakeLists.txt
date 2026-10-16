cmake_minimum_required(VERSION 3.16)
if(NOT ESP_PLATFORM)
    project(unravel_speech_tts_host LANGUAGES C CXX)
endif()

set(espeak_dir "${CMAKE_CURRENT_LIST_DIR}/vendor/espeak-ng")
set(espeak_sources
    common.c dictionary.c encoding.c error.c intonation.c langopts.c mnemonics.c
    numbers.c phoneme.c phonemelist.c readclause.c setlengths.c soundicon.c
    speech.c ssml.c synthdata.c synthesize.c tr_languages.c translate.c translateword.c
    voices.c wavegen.c
    ucd-tools/case.c ucd-tools/categories.c ucd-tools/ctype.c
    ucd-tools/proplist.c ucd-tools/scripts.c ucd-tools/tostring.c)
list(TRANSFORM espeak_sources PREPEND "${espeak_dir}/")
set(speech_sources ${espeak_sources} embedded_workspace.c speech_pcm.cpp speech_tts.cpp)
set(espeak_definitions ESPEAK_EMBEDDED=1 USE_ASYNC=0 USE_MBROLA=0 USE_KLATT=0
    USE_LIBSONIC=0 USE_LIBPCAUDIO=0)

if(ESP_PLATFORM)
    idf_component_get_property(peer_dir espressif__esp_peer COMPONENT_DIR)
    file(STRINGS "${peer_dir}/idf_component.yml"
        peer_version_line REGEX "^version:[ ]+1[.]5[.]7$")
    if(NOT peer_version_line)
        message(FATAL_ERROR "Re-audit native voice DTLS hook before changing esp_peer 1.5.7")
    endif()
    idf_component_register(SRCS ${speech_sources} speech_live.cpp
        speech_native.cpp speech_native_protocol.cpp INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS . "${espeak_dir}"
            "${peer_dir}/src"
        LDFRAGMENTS "linker.lf"
        REQUIRES audio_bsp
        PRIV_REQUIRES heap esp_http_client tcp_transport esp-tls mbedtls esp_timer cjson lwip
            speech_stt codex_auth espressif__esp_peer espressif__esp_audio_codec espressif__esp_libsrtp)
    # esp_peer 1.5.7 does not bind its optional-auth DTLS certificate to SDP.
    # Fail closed in our wrapper before its main loop can expose SRTP/SCTP.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dtls_srtp_handshake")
    # main -> user_app -> chat -> speech -> audio -> i2c -> main needs a third archive pass.
    set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${espeak_definitions})
    target_link_libraries(${COMPONENT_LIB} PRIVATE m)
else()
    add_library(speech_tts STATIC ${speech_sources})
    target_compile_features(speech_tts PUBLIC cxx_std_17)
    target_compile_definitions(speech_tts PRIVATE ${espeak_definitions})
    target_include_directories(speech_tts PUBLIC include PRIVATE . "${espeak_dir}" ../audio_bsp)
    target_link_libraries(speech_tts PUBLIC m)
    add_executable(speech_tts_smoke host/smoke.cpp)
    target_link_libraries(speech_tts_smoke PRIVATE speech_tts)
    add_executable(speech_live_protocol_test host/live_protocol.cpp)
    target_compile_features(speech_live_protocol_test PRIVATE cxx_std_17)
    target_include_directories(speech_live_protocol_test PRIVATE .)
    add_executable(speech_native_protocol_test host/native_protocol.cpp speech_native_protocol.cpp)
    target_compile_features(speech_native_protocol_test PRIVATE cxx_std_17)
    target_include_directories(speech_native_protocol_test PRIVATE .)
endif()
