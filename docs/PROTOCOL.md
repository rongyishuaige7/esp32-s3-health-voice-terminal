# Cloud and streaming protocol boundary

## Data ownership and destinations

A complete configured turn can send microphone PCM to ASR, the resulting transcript to the model endpoint, and answer text to TTS. These are three data-processing boundaries. Provider keys are independent local credentials; this repository provides no relay service or relay authorization.

## XFYun streaming IAT

`components/cloud_api/xfyun_iat_ws.c` implements this client sequence:

1. connect to signed `wss://iat-api.xfyun.cn/v2/iat`;
2. first audio frame uses `data.status=0` and includes `common` plus `business`;
3. each middle audio frame uses `data.status=1`;
4. recording completion causes a final `{"data":{"status":2}}` frame;
5. the client waits for the server final status and accumulates text;
6. disconnect/error before completion is treated as failure.

Audio send pacing is approximately 40 ms per 1280-byte frame for 16 kHz, 16-bit mono PCM. Source inspection and tests verify this ordering; no current provider session was run.

## DeepSeek chat

- Endpoint: `<CONFIG_DEEPSEEK_API_BASE>/v1/chat/completions`.
- Authentication: `Authorization: Bearer <CONFIG_DEEPSEEK_API_KEY>`.
- Non-streaming mode reads `choices[0].message.content`.
- Streaming mode sets `stream=true`, accepts only HTTP 2xx, parses `data: {...}` lines, and ends on `data: [DONE]`.
- Incremental content is accumulated for the UI and segmented into sentences for TTS.

Current implementation handles `content` deltas and strips complete `<think>...</think>` blocks after accumulation. It is not a general OpenAI-compatible protocol translator and is not claimed to support every provider/event variant.

## TTS and first audible output

- XFYun TTS submits one request with `data.status=2` and receives base64 raw PCM chunks.
- In the stream pipe, a sentence enters the TTS queue only after LLM segmentation.
- The UI playback callback fires on the first non-empty PCM chunk, not merely when the TTS request is sent.
- Playback completes only after the TTS queue EOF and PCM buffer drain, followed by tail silence and I²S TX stop.

This order is source-confirmed. Network latency, actual first audible packet and audio integrity require timestamped hardware/live-provider testing.
