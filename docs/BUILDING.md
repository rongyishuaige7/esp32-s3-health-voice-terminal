# Building and local configuration

## Supported verified baseline

The publication gate uses ESP-IDF 5.5.2 and target `esp32s3`. `dependencies.lock` records the resolved component set. Other ESP-IDF versions may work but are not part of the current evidence.

## Linux / macOS

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

## Windows PowerShell

Open the ESP-IDF PowerShell supplied by Espressif, then:

```powershell
cd C:\path\to\esp32-s3-health-voice-terminal
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

## Windows CMD

Open the ESP-IDF Command Prompt, then:

```bat
cd /d C:\path\to\esp32-s3-health-voice-terminal
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Do not copy a local `sdkconfig` into documentation or issues. To reset public defaults:

```bash
idf.py fullclean
rm -f sdkconfig sdkconfig.old
idf.py set-target esp32s3
idf.py build
```

## Flash boundary

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Replace the port with the real device. Flashing writes the application and generated ESP-SR model partition; an app-only binary is not sufficient when the selected wake model changes. Confirm board Flash/PSRAM before flashing.
