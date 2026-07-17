# Third-party notices

Original Rongyi source code is MIT-licensed. That license does not replace the terms of dependencies, generated font data, hosted APIs or speech/model services.

| Component / service | Role | License or terms boundary |
|:--|:--|:--|
| ESP-IDF 5.5.2 | SDK, FreeRTOS integration, drivers, TLS and build system | Apache-2.0 with component-specific exceptions/notices; <https://github.com/espressif/esp-idf> |
| Espressif ESP-SR 2.4.0 | WakeNet and speech-model build integration | Apache-2.0 source; bundled/pretrained model use remains subject to upstream notices; <https://github.com/espressif/esp-sr> |
| Espressif ESP-DSP 1.7.0 / dl_fft 0.3.1 | Signal-processing dependencies of ESP-SR | Apache-2.0 or upstream component terms; Espressif Component Registry |
| Espressif LVGL port 2.7.2 | LVGL/display integration | Apache-2.0; <https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port> |
| LVGL 9.5.0 | Embedded GUI | MIT; <https://github.com/lvgl/lvgl> |
| Espressif WebSocket client 1.6.1 | XFYun WSS transport | Apache-2.0; Espressif Component Registry |
| Droid Sans Fallback | Source font for `lv_font_sc_supplement_16.c` | Apache-2.0, Copyright Google Corp.; Debian `fonts-droid-fallback` / Android source |
| `lv_font_conv` | Generated LVGL C font data | MIT tool; <https://github.com/lvgl/lv_font_conv> |
| DeepSeek hosted API | Chat completion / optional SSE | No client SDK is vendored; use is governed by current DeepSeek service, privacy and model terms; <https://platform.deepseek.com> |
| XFYun hosted APIs | Streaming speech recognition and TTS | No SDK/source is vendored; use is governed by current XFYun service, voice, account and privacy terms; <https://www.xfyun.cn/> |
| User-configured ASR/TTS endpoint | Optional alternative provider or self-hosted service | The user must review and comply with that endpoint's software, model, voice and data terms |

## Generated font notice

`components/ui_display/lv_font_sc_supplement_16.c` is derived from the Apache-2.0 Droid Sans Fallback font with `lv_font_conv`. The repository distributes generated glyph data, not the original TTF. The source font path embedded by the old generator was removed because it was a private host path; attribution remains here.

Cloud model names, available voices, endpoints, pricing, data retention and terms can change independently of this repository. Verify current provider documents before deployment or redistribution.
