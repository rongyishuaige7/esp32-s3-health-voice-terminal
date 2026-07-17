#!/usr/bin/env python3
"""Release checks that need no hardware or live cloud credentials."""
from __future__ import annotations
import argparse,csv,subprocess,sys,xml.etree.ElementTree as ET
from pathlib import Path
REQUIRED=[".github/workflows/validate.yml",".gitignore",".markdownlint-cli2.jsonc","CMakeLists.txt","HARDWARE.md","LICENSE","Makefile","README.md","SECURITY.md","THIRD_PARTY_NOTICES.md","components/audio_i2s/audio_i2s.c","components/cloud_api/cloud_deepseek.c","components/cloud_api/xfyun_iat_ws.c","components/medical_preprocess/medical_preprocess.c","components/ui_display/lv_font_sc_supplement_16.c","dependencies.lock","docs/BUILDING.md","docs/GITHUB_METADATA.md","docs/HARDWARE_LAB_CARD.md","docs/PROJECT_STATUS.md","docs/PROTOCOL.md","docs/SOURCE_PROVENANCE.md","docs/VERIFICATION.md","hardware/BOM.csv","hardware/wiring-diagram.svg","main/Kconfig.projbuild","partitions.csv","scripts/secret_scan.py","scripts/verify.sh","sdkconfig.defaults","tests/test_source_contracts.py","tools/eval_offline.py","tools/eval_scenarios.json"]
FORBIDDEN_NAMES={"sdkconfig","sdkconfig.old","medical_voice_terminal.tar.gz",".codex"}
FORBIDDEN_DIRS={"build","managed_components",".vscode",".idea",".cursor",".codex","__pycache__",".pytest_cache"}
FORBIDDEN_SUFFIXES={".bin",".elf",".map",".uf2",".zip",".7z",".tar",".gz",".pem",".key"}
MAX=7*1024*1024

def files(root):
 try:raw=subprocess.run(["git","-C",str(root),"ls-files","-z"],check=True,capture_output=True).stdout
 except (subprocess.CalledProcessError,FileNotFoundError):raw=b""
 if raw:return [root/x.decode("utf-8","surrogateescape") for x in raw.split(b"\0") if x]
 return sorted(p for p in root.rglob("*") if p.is_file() and not any(x in {".git","build","managed_components","__pycache__"} for x in p.relative_to(root).parts))
def main():
 ap=argparse.ArgumentParser();ap.add_argument("--root",default=".");root=Path(ap.parse_args().root).resolve();err=[]
 for rel in REQUIRED:
  if not (root/rel).is_file():err.append(f"missing required file: {rel}")
 checked=files(root)
 for p in checked:
  rel=p.relative_to(root)
  if p.name in FORBIDDEN_NAMES:err.append(f"forbidden local/generated file: {rel}")
  if any(x in FORBIDDEN_DIRS for x in rel.parts):err.append(f"forbidden local/generated directory: {rel}")
  if any(p.name.lower().endswith(s) for s in FORBIDDEN_SUFFIXES):err.append(f"forbidden binary/archive/key artifact: {rel}")
  if p.stat().st_size>MAX:err.append(f"file exceeds 7 MiB: {rel}")
 contracts={
  "README.md":["不是医疗器械","当前开发板、音频、显示与真实云端端到端链路尚未重新真机复测","真实 key 只存在于你的本地 `sdkconfig`","first audio frame: data.status = 0"],
  "docs/PROJECT_STATUS.md":["ESP-IDF 5.5.2 干净构建已验证","没有真实讯飞/DeepSeek 调用"],
  "docs/SOURCE_PROVENANCE.md":["a5e761b62a8467ee52f01936cfe015528fc973089c296124b8f6f1a587a3405c","dab2d8b4fe96499a2c018c015e3c6974cebd897dd176b1f7ceaae9477f0d7856","b0f3167d9214630781c1989c3b05542fed66430acea960637e8ae718be4adb22"],
  "sdkconfig.defaults":["CONFIG_WIFI_SSID=\"YOUR_WIFI_SSID\"","CONFIG_DEEPSEEK_API_KEY=\"\"","CONFIG_CLOUD_ASR_USE_XFYUN_WS=n","CONFIG_CLOUD_TTS_USE_XFYUN_WS=n","CONFIG_CHAIN_STREAMING=n"],
  "components/net_wifi/net_wifi.c":["connecting to configured Wi-Fi access point"],
  "components/cloud_api/cloud_util.c":["response body suppressed"],
 }
 for rel,values in contracts.items():
  p=root/rel
  if p.is_file():
   text=p.read_text(encoding="utf-8")
   for value in values:
    if value not in text:err.append(f"fact contract missing in {rel}: {value}")
 claims=["medical device verified","diagnosis verified","safe health advice","r1 online","asr verified","tts verified","low latency verified","current hardware verified","production ready","system online"]
 for rel in ["README.md","docs/PROJECT_STATUS.md","docs/HARDWARE_LAB_CARD.md"]:
  text=(root/rel).read_text(encoding="utf-8").lower() if (root/rel).is_file() else ""
  for claim in claims:
   positive=[line for line in text.splitlines() if claim in line and not any(m in line for m in ("unsupported","not ","no claim","不是","不证明","未","不能"))]
   if positive:err.append(f"unsupported claim in {rel}: {claim}")
 try:ET.parse(root/"hardware/wiring-diagram.svg")
 except (ET.ParseError,OSError) as exc:err.append(f"invalid wiring SVG: {exc}")
 try:
  rows=list(csv.DictReader((root/"hardware/BOM.csv").open(newline="",encoding="utf-8")))
  if len(rows)<8:err.append("BOM must contain at least 8 component rows")
 except (OSError,csv.Error) as exc:err.append(f"invalid BOM.csv: {exc}")
 font=root/"components/ui_display/lv_font_sc_supplement_16.c"
 if font.is_file():
  t=font.read_text(encoding="utf-8",errors="ignore")
  if "/home/" in t or "DroidSansFallbackFull.ttf --size" in t:err.append("generated font still exposes host generation path")
 if err:
  print("Repository check: FAIL",file=sys.stderr)
  for e in sorted(set(err)):print(f"- {e}",file=sys.stderr)
  return 1
 print(f"Repository check: PASS ({len(checked)} files checked)");return 0
if __name__=="__main__":raise SystemExit(main())
