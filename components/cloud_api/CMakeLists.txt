set(cloud_api_srcs
    "cloud_util.c" "cloud_deepseek.c" "cloud_asr.c" "cloud_tts.c" "cloud_chain.c")

if(CONFIG_CLOUD_ASR_USE_XFYUN_WS OR CONFIG_CLOUD_TTS_USE_XFYUN_WS)
    list(APPEND cloud_api_srcs "cloud_xfyun_uri.c")
endif()

if(CONFIG_CLOUD_ASR_USE_XFYUN_WS)
    list(APPEND cloud_api_srcs "xfyun_iat_ws.c")
endif()

if(CONFIG_CLOUD_TTS_USE_XFYUN_WS)
    list(APPEND cloud_api_srcs "xfyun_tts_ws.c")
endif()

if(CONFIG_CHAIN_STREAMING AND CONFIG_CLOUD_ASR_USE_XFYUN_WS AND CONFIG_CLOUD_TTS_USE_XFYUN_WS)
    list(APPEND cloud_api_srcs "cloud_stream_pipe.c")
endif()

idf_component_register(
    SRCS ${cloud_api_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_system json mbedtls esp-tls heap medical_preprocess
        espressif__esp_websocket_client esp_event audio_i2s net_wifi
)
