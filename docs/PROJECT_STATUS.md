# Project status

> Status date: 2026-07-17

| Layer | Current evidence | Not established |
|:--|:--|:--|
| Source | Authoritative source, archives and exclusions documented | Original dirty Git staging is not treated as release history |
| Repository | Secret scan, structure, file-size, BOM/SVG and claim contracts pass | No claim that the old source directory was release-ready |
| Offline tooling | Python compile, unit/source contracts and scenario JSON-only validation pass | No live DeepSeek quality, cost or safety evaluation |
| Firmware | Clean ESP-IDF 5.5.2 `esp32s3` build passes with empty credentials | Current board not flashed; Flash/PSRAM assumptions not physically confirmed |
| Wake/audio/display | Source and dependency configuration present | No current WakeNet, INMP441, MAX98357A, speaker or ST7789 re-test |
| Cloud chain | IAT `0→1*→2`, DeepSeek SSE and TTS queue source contracts checked | No live XFYun/DeepSeek call, first-packet timing, recognition or playback proof |
| Health content | Fixed educational prompt and warning boundaries present | No medical accuracy, safety, clinical evaluation or regulatory status |
| Media/EDA | None claimed | No current photos, video, schematic, PCB or manufacturing files |

Canonical summary:

```text
Source-confirmed
Hardware-free source contracts passed
ESP-IDF 5.5.2 clean build verified
Offline scenario-file validation passed
Current board, audio, display and live cloud end-to-end re-test not run
```

Unsupported labels include medical device, diagnosis verified, treatment advice verified, safe health advice, R1 online, ASR/TTS verified, low latency verified, hardware verified, production ready, always available and emergency support.
