# Verification

## One-command local gate

```bash
bash scripts/verify.sh
```

The gate creates an isolated copy of the current Git tree, then runs:

1. credential, private-key, private-LAN, local-path and signed-URL checks;
2. required-file, generated-output, large-file, SVG/BOM and claims contracts;
3. Python syntax checks and hardware-free unit/source contracts;
4. JSON-only offline scenario validation with provider keys removed;
5. a clean ESP-IDF 5.5.2 build for `esp32s3` with placeholder Wi-Fi and empty cloud credentials.

The build may download declared open-source components. It does not use real provider credentials, flash hardware or make live ASR/LLM/TTS calls.

## Verified publication build

On 2026-07-17, the clean gate passed with ESP-IDF 5.5.2 and target `esp32s3`:

- application binary: `0x238660` bytes (2,328,160 bytes);
- application partition: `0x300000` bytes (3,145,728 bytes);
- application partition free space: `0xC79A0` bytes (26% as reported by ESP-IDF);
- bootloader binary: `0x5240` bytes, with `0x2DC0` bytes (36%) free in its configured region;
- generated ESP-SR model recommendation: 285 KiB;
- configured model partition: 6 MiB;
- generated clean-build artifacts included the application, bootloader, partition table and `srmodels.bin`.

These figures are build evidence only. They do not confirm the exact physical module capacity, runtime RAM high-water marks, successful flashing or current device behavior.

## Hardware/live-cloud re-test checklist

Bind future evidence to an exact commit and date. Record pass/fail/not-run for:

1. exact ESP32-S3 board, module, Flash, PSRAM and ESP-IDF version;
2. INMP441, MAX98357A, speaker, ST7789, supply and all GPIO wiring;
3. full `idf.py flash`, including ESP-SR model partition;
4. boot, display, WakeNet and energy-threshold behavior;
5. microphone RMS, channel selection, clipping and feedback;
6. Wi-Fi/NTP without publishing SSID, signed URLs or keys;
7. XFYun ASR first/middle/end frame behavior and actual transcript;
8. DeepSeek model/endpoint, SSE event order and first text delta;
9. TTS first PCM, first audible output, sentence order and tail playback;
10. error states for missing/invalid provider credentials and network loss;
11. end-to-end latency using named timestamps, not subjective wording alone;
12. 30–60 minute stability, memory high-water marks and reset behavior;
13. explicit review of inaccurate/unsafe responses—without treating a small scenario list as clinical evidence.

Never upgrade a clean build, source contract or keyword match into current hardware, cloud-service, medical-safety or production evidence.
