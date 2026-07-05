idf_component_register(
    SRCS "lexin_voice.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_http_client
        esp_netif
        esp_timer
        esp_wifi
        espressif__esp32_p4_function_ev_board
        freertos
        espressif__esp_codec_dev)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # Audio capture pipeline. FunASR and most embedded ASR examples are
    # happiest with 16 kHz mono PCM, and the P4 Function EV Board codec
    # can be opened at that rate after BSP initialization.
    LEXIN_VOICE_SAMPLE_RATE=16000
    LEXIN_VOICE_FRAME_MS=30
    # Energy gate: 30 ms frame RMS threshold. Your board's idle level is
    # about 120-160 RMS, so keep the gate above that to avoid uploading
    # room noise forever. Speak normally at 30-60 cm should exceed this.
    LEXIN_VOICE_RMS_THRESHOLD=180
    # How long the user must keep quiet before we stop the current stream.
    LEXIN_VOICE_SILENCE_MS=900
    # Maximum length of a single user utterance. Once reached the stream
    # is closed and forwarded to the proxy for ASR. 6 s @ 16000 Hz =
    # ~96 k samples = ~192 KB of int16 PCM, which still fits in PSRAM
    # without starving the camera and the edge advisor.
    LEXIN_VOICE_MAX_UTTER_MS=6000
    # Cooldown after a stream is forwarded. Prevents the next burst from
    # being truncated while the proxy is still uploading.
    LEXIN_VOICE_COOLDOWN_MS=1500)
