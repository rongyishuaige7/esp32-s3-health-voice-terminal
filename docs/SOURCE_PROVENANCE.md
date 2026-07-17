# Source provenance

## Authoritative-copy decision

| Object | Role | Evidence | Rule |
|:--|:--|:--|:--|
| `/mnt/shared/2026项目/基于 ESP32 与 DeepSeek R1 的智能医疗问答终端.zip` | Read-only historical archive | SHA-256 `a5e761b62a8467ee52f01936cfe015528fc973089c296124b8f6f1a587a3405c`; public-source comparison matched the desktop tree before publication edits | Never edited or repacked |
| `/home/rongyi/桌面/基于 ESP32 与 DeepSeek R1 的智能医疗问答终端` | Read-only authoritative source | 55 non-`.git` files; source manifest SHA-256 `dab2d8b4fe96499a2c018c015e3c6974cebd897dd176b1f7ceaae9477f0d7856`; files dated through 2026-04-28 | Not cleaned or committed in place |
| source `medical_voice_terminal.tar.gz` | Older nested snapshot | SHA-256 `b0f3167d9214630781c1989c3b05542fed66430acea960637e8ae718be4adb22`; lacks later stream-pipe files and differs from current source | Excluded as a duplicate historical package |
| `/home/rongyi/桌面/esp32-s3-health-voice-terminal` | Clean publication candidate | Fresh Git history and explicit exclusions | All public hardening occurs here |

The desktop and ZIP had the same 54 comparable public-source files before publication work. The desktop directory's Git repository had no commits or branch refs and showed staged/unstaged work, so it was not imported as trustworthy public history.

## Included

- current ESP-IDF source and component manifests;
- `dependencies.lock`, partitions and public `sdkconfig.defaults` after secret-safe default changes;
- current LVGL C glyph data with provenance retained in `THIRD_PARTY_NOTICES.md`;
- offline scenario script and JSON after review.

## Excluded before first public commit

- old `.git`, `.codex`, `.cursor`, `.vscode` and other editor/agent state;
- nested `medical_voice_terminal.tar.gz` and the ZIP itself;
- `sdkconfig`, `sdkconfig.old`, build output and managed dependency caches;
- private host paths in generated font comments;
- any binary built with real provider or Wi-Fi credentials.

No real API credential was found in the current source tree. Strings such as `sk-...` in documentation were short illustrative placeholders. Public defaults were nevertheless changed to explicit placeholders/empty credentials and cloud calls are disabled in the clean CI build configuration.

## Public hardening

- Wi-Fi logs no longer print the configured SSID;
- generic HTTP errors no longer print complete response bodies;
- real provider keys remain separate local Kconfig values and are never replaced by relay authorization;
- public default configuration uses no live DeepSeek/XFYun/ASR/TTS credential;
- source contracts enforce IAT frame order and the DeepSeek SSE done marker;
- documentation separates build evidence, offline JSON validation, historical source and current hardware/live-cloud verification;
- medical-device, diagnosis, prescription, emergency and privacy boundaries are made explicit.
