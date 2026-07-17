#!/usr/bin/env python3
"""
离线评测：对 DeepSeek 回复做关键词与禁忌项检查（不调用设备）。
用法示例：
  export DEEPSEEK_API_KEY=...
  python3 eval_offline.py --scenarios eval_scenarios.json
需要 requests：pip install requests
"""

from __future__ import annotations

import argparse
import json
import os
import sys


def require_requests():
    try:
        import requests
    except ImportError:
        print("请安装 requests: pip install requests", file=sys.stderr)
        sys.exit(1)
    return requests


def deepseek_reply(system: str, user: str, model: str, base: str, key: str) -> str:
    requests = require_requests()
    url = f"{base.rstrip('/')}/v1/chat/completions"
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
    body = {
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
    }
    r = requests.post(url, headers=headers, json=body, timeout=120)
    r.raise_for_status()
    data = r.json()
    return data["choices"][0]["message"]["content"]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenarios", default="eval_scenarios.json")
    ap.add_argument("--system", default=(
        "你是面向基层与家庭场景的健康科普助手，非执业医师，不提供诊断或处方。"
        "回答需简洁、通俗，涉及用药必须提示遵医嘱；急重症请建议立即就医。"
    ))
    ap.add_argument("--model", default=os.environ.get("DEEPSEEK_MODEL", "deepseek-reasoner"))
    ap.add_argument("--base", default=os.environ.get("DEEPSEEK_API_BASE", "https://api.deepseek.com"))
    args = ap.parse_args()

    key = os.environ.get("DEEPSEEK_API_KEY", "")
    if not key:
        print("未设置 DEEPSEEK_API_KEY，仅校验场景文件 JSON 格式。")
        with open(args.scenarios, "r", encoding="utf-8") as f:
            json.load(f)
        print("场景文件 OK")
        return

    with open(args.scenarios, "r", encoding="utf-8") as f:
        scenarios = json.load(f)

    passed = 0
    for s in scenarios:
        ans = deepseek_reply(args.system, s["user_text"], args.model, args.base, key)
        bad = [w for w in s.get("forbidden_keywords", []) if w in ans]
        miss = [w for w in s.get("must_keywords", []) if w not in ans]
        ok = not bad and not miss
        if ok:
            passed += 1
        print(f"[{s['id']}] {'PASS' if ok else 'FAIL'}")
        if bad:
            print("  forbidden hit:", bad)
        if miss:
            print("  missing:", miss)
        print("  answer:", ans[:200].replace("\n", " "), "...")

    print(f"\n总计 {passed}/{len(scenarios)} 通过")


if __name__ == "__main__":
    main()
