# Hardware Lab 索引卡片

```yaml
name: ESP32-S3 健康科普语音终端
platform: ESP32-S3 · ESP-IDF · ESP-SR · DeepSeek · XFYun · LVGL
summary: 健康科普语音交互教学原型，覆盖本地唤醒、双 I²S 音频、流式听写、DeepSeek SSE、按句 TTS 与 ST7789 状态显示。
status: 源码与硬件无关契约已确认 · ESP-IDF 5.5.2 干净构建已验证 · 当前硬件与真实云端端到端链路尚未重新真机复测
media_scope: 当前没有实物照片、演示视频、EDA、PCB 或制造文件；公开源码推导的 BOM 与接线边界图。
known_boundaries:
  - 不是医疗器械，不提供诊断、处方、急救或经过验证的健康建议。
  - CI 使用空凭据，不调用讯飞或 DeepSeek，不证明识别、回复、TTS、首包延迟或内容安全。
  - 真实凭据会编译进固件；用户必须使用独立、可轮换、低额度的服务商凭据。
  - 当前 ESP32-S3、INMP441、MAX98357A、ST7789 与端到端云链路未重新真机复测。
```
