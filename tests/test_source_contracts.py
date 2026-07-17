from __future__ import annotations
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


class SourceContracts(unittest.TestCase):
    def test_public_defaults_have_no_live_credentials(self) -> None:
        defaults = read("sdkconfig.defaults")
        for name in (
            "CONFIG_DEEPSEEK_API_KEY",
            "CONFIG_CLOUD_ASR_TOKEN",
            "CONFIG_CLOUD_TTS_TOKEN",
            "CONFIG_XFYUN_APP_ID",
            "CONFIG_XFYUN_API_KEY",
            "CONFIG_XFYUN_API_SECRET",
        ):
            self.assertRegex(defaults, rf'(?m)^{name}=""$')
        self.assertIn('CONFIG_WIFI_SSID="YOUR_WIFI_SSID"', defaults)
        self.assertIn('CONFIG_WIFI_PASSWORD="YOUR_WIFI_PASSWORD"', defaults)
        self.assertIn("CONFIG_CLOUD_ASR_USE_XFYUN_WS=n", defaults)
        self.assertIn("CONFIG_CLOUD_TTS_USE_XFYUN_WS=n", defaults)
        self.assertIn("CONFIG_CHAIN_STREAMING=n", defaults)
        self.assertIn("CONFIG_ESP_TASK_WDT_TIMEOUT_S=60", defaults)
        self.assertNotIn("CONFIG_TASK_WDT_TIMEOUT_S=", defaults)
        self.assertIn("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y", defaults)
        self.assertNotIn("CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240", defaults)
        self.assertNotIn("CONFIG_ESP32S3_SPIRAM_SUPPORT", defaults)
        self.assertNotIn("CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY", defaults)

    def test_disabled_xfyun_sources_are_conditionally_compiled(self) -> None:
        cmake = read("components/cloud_api/CMakeLists.txt")
        tts = read("components/cloud_api/cloud_tts.c")
        unconditional = cmake.split("if(CONFIG_CLOUD_ASR_USE_XFYUN_WS OR", 1)[0]
        for source in ("cloud_xfyun_uri.c", "xfyun_iat_ws.c", "xfyun_tts_ws.c", "cloud_stream_pipe.c"):
            self.assertNotIn(source, unconditional)
        self.assertIn("if(CONFIG_CLOUD_TTS_USE_XFYUN_WS)", cmake)
        self.assertRegex(
            tts,
            r'#if CONFIG_CLOUD_TTS_USE_XFYUN_WS\s+#include "xfyun_tts_ws.h"\s+#endif',
        )

    def test_ci_activates_the_esp_idf_container(self) -> None:
        workflow = read(".github/workflows/validate.yml")
        activation = '. "$IDF_PATH/export.sh"'
        target = "idf.py set-target esp32s3"
        build = "idf.py build"
        self.assertIn("image: espressif/idf:v5.5.2", workflow)
        self.assertLess(workflow.index(activation), workflow.index(target))
        self.assertLess(workflow.index(target), workflow.index(build))

    def test_iat_frame_order_contract(self) -> None:
        src = read("components/cloud_api/xfyun_iat_ws.c")
        first = src.index('cJSON_AddNumberToObject(data, "status", 0)')
        middle = src.index('cJSON_AddNumberToObject(data, "status", 1)')
        sender = src.index("static void iat_stream_send_task")
        final = src.index('const char *end = "{\\"data\\":{\\"status\\":2}}"')
        self.assertLess(first, middle)
        self.assertLess(middle, sender)
        self.assertLess(sender, final)
        self.assertIn("first ? build_first_frame", src)
        self.assertIn(": build_mid_frame", src)

    def test_sse_and_first_pcm_contracts(self) -> None:
        ds = read("components/cloud_api/cloud_deepseek.c")
        pipe = read("components/cloud_api/cloud_stream_pipe.c")
        self.assertIn('cJSON_AddBoolToObject(root, "stream", true)', ds)
        self.assertIn('strcmp(payload, "[DONE]") == 0', ds)
        self.assertIn("if (!s_first_pcm)", pipe)
        self.assertIn("s_on_first_pcm(s_on_first_ctx)", pipe)
        self.assertLess(pipe.index("s_first_pcm = true"), pipe.index("s_on_first_pcm(s_on_first_ctx)"))

    def test_tls_verification_and_redacted_logs(self) -> None:
        util = read("components/cloud_api/cloud_util.c")
        ds = read("components/cloud_api/cloud_deepseek.c")
        wifi = read("components/net_wifi/net_wifi.c")
        self.assertGreaterEqual(util.count(".crt_bundle_attach = esp_crt_bundle_attach"), 3)
        self.assertIn(".crt_bundle_attach = esp_crt_bundle_attach", ds)
        self.assertNotIn("skip_cert_common_name_check", util + ds)
        self.assertNotIn("body=%s", util)
        self.assertNotIn('connecting to %s', wifi)

    def test_health_prompt_has_narrow_boundary(self) -> None:
        prompt = read("components/medical_preprocess/medical_preprocess.c")
        for phrase in ("健康科普助手", "非执业医师", "不提供诊断或处方", "遵医嘱", "立即就医"):
            self.assertIn(phrase, prompt)

    def test_scenarios_are_structurally_valid_not_medical_proof(self) -> None:
        data = json.loads(read("tools/eval_scenarios.json"))
        self.assertGreaterEqual(len(data), 3)
        for item in data:
            self.assertIsInstance(item["id"], str)
            self.assertIsInstance(item["user_text"], str)
            self.assertIsInstance(item.get("must_keywords", []), list)
            self.assertIsInstance(item.get("forbidden_keywords", []), list)


if __name__ == "__main__":
    unittest.main()
