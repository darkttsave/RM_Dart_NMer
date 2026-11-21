#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M3 参数管理自检（本机联调，需 main + Flask 运行中）

覆盖：
  A. 参数读取：网页读取值 = C++ 实际运行值（20 项，含范围/类型/热更新标记）
  B. 运行时修改：应用后 C++ 运行值变化，并通过 /api/status 验证视觉实际使用新参数
  C. 异常输入：超范围 / 未知参数 / 非整数 / 交叉约束 / 错误类型
  D. 保存配置：apply -> save -> 校验 YAML 中目标值已写入且无关配置保留
  E. 事件一致性：录制会话 events.jsonl 的 elapsed_us / frame_id 与遥测 CSV 对齐

用法：
    python3 tests/test_m3_params.py --config /tmp/m3test/config/debug.yaml \
        --session /tmp/m3test/recordings/<会话> [--base http://127.0.0.1:5000]

说明：
  - 保存测试会写入 --config 指定的配置文件，请务必使用隔离副本；
  - 会临时修改 bright_thresh 并在结束时恢复原值。
"""

import argparse
import csv
import json
import os
import sys
import time

import requests

CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok), detail))
    print(("[PASS] " if ok else "[FAIL] ") + name + (" :: " + detail if detail else ""))


def get(base, path):
    resp = requests.get(base + path, timeout=5)
    return resp.status_code, resp.json()


def post(base, path, payload):
    resp = requests.post(base + path, json=payload, timeout=8)
    return resp.status_code, resp.json()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:5000")
    parser.add_argument("--config", default="", help="隔离的配置文件副本（保存测试用）")
    parser.add_argument("--session", default="", help="录制会话目录（事件一致性测试用）")
    args = parser.parse_args()
    base = args.base.rstrip("/")

    # ---------------- A. 参数读取 ----------------
    status, data = get(base, "/api/params")
    check("参数接口可访问", status == 200 and data.get("ok") is True, str(status))
    running = data.get("running", {})
    saved = data.get("saved", {})
    params = data.get("params", [])
    check("参数数量完整", len(running) == 20 and len(params) == 20,
          f"running={len(running)} params={len(params)}")
    required_fields = {"name", "label", "unit", "group", "type", "min", "max", "hot",
                       "web_editable", "hint"}
    meta_ok = all(required_fields.issubset(set(p.keys())) for p in params)
    check("参数元数据字段完整", meta_ok)
    check("配置路径可见", bool(data.get("config_path")), data.get("config_path", ""))
    brightness = running.get("bright_thresh")
    check("运行值来自 C++", brightness is not None, f"bright_thresh={brightness}")

    # ---------------- B. 运行时修改 + 视觉生效 ----------------
    original = brightness
    temp_value = 255 if original != 255 else 254
    status, applied = post(base, "/api/params", {"cmd": "apply", "values": {"bright_thresh": temp_value}})
    check("应用参数成功", status == 200 and applied.get("ok") is True,
          str(applied.get("applied") or applied.get("error")))
    status, data2 = get(base, "/api/params")
    check("C++ 运行值已更新", data2.get("running", {}).get("bright_thresh") == temp_value,
          f"running={data2.get('running', {}).get('bright_thresh')}")
    check("配置值未被应用修改", data2.get("saved", {}).get("bright_thresh") == original,
          f"saved={data2.get('saved', {}).get('bright_thresh')}")
    check("未保存差异可见", "bright_thresh" in data2.get("dirty", []), str(data2.get("dirty")))

    # 视觉实际使用新参数：bright_thresh=255 时几乎无有效亮区 -> green_pixels 归零
    time.sleep(1.5)
    _, status_data = get(base, "/api/status")
    frame = status_data.get("frame") or {}
    check("视觉流程使用新参数", frame.get("green_pixels") == 0,
          f"green_pixels={frame.get('green_pixels')}")

    # 恢复
    post(base, "/api/params", {"cmd": "apply", "values": {"bright_thresh": original}})
    time.sleep(1.5)
    _, status_data = get(base, "/api/status")
    frame = status_data.get("frame") or {}
    check("恢复原参数后识别恢复", frame.get("green_pixels", 0) > 0,
          f"green_pixels={frame.get('green_pixels')}")

    # ---------------- C. 异常输入 ----------------
    cases = [
        ("超范围", {"bright_thresh": 999}),
        ("未知参数", {"no_such_param": 1}),
        ("非整数", {"required_frames": 3.7}),
        ("交叉约束", {"min_radius": 100, "max_radius": 30}),
        ("空对象", {}),
    ]
    for label, values in cases:
        status, resp = post(base, "/api/params", {"cmd": "apply", "values": values})
        check(f"异常输入拒绝：{label}", resp.get("ok") is False and bool(resp.get("error")),
              f"{status} {resp.get('error')}")
    status, resp = post(base, "/api/params", {"cmd": "apply", "values": "not-an-object"})
    check("错误类型拒绝", status == 400 and resp.get("ok") is False, str(status))

    # ---------------- D. 保存配置 ----------------
    if args.config:
        with open(args.config, "r", encoding="utf-8") as handle:
            before_text = handle.read()
        mark = original if original != 123 else 124
        post(base, "/api/params", {"cmd": "apply", "values": {"bright_thresh": mark}})
        status, resp = post(base, "/api/params", {"cmd": "save"})
        check("保存配置成功", resp.get("ok") is True, str(resp.get("saved_to") or resp.get("error")))
        with open(args.config, "r", encoding="utf-8") as handle:
            after_text = handle.read()
        check("配置值写入 YAML", f"bright_thresh: {mark}" in after_text,
              "bright_thresh 行")
        unrelated = ["communication:", "recording:", "web_debug:", "# 飞镖制导系统配置文件"]
        check("无关配置保留", all(item in after_text for item in unrelated),
              "通信/录制/预览段与注释")
        # 还原配置
        post(base, "/api/params", {"cmd": "apply", "values": {"bright_thresh": original}})
        post(base, "/api/params", {"cmd": "save"})
        with open(args.config, "r", encoding="utf-8") as handle:
            restored = handle.read()
        check("配置可还原", f"bright_thresh: {original}" in restored)
        _ = before_text

    # ---------------- E. 事件与遥测时间一致性 ----------------
    if args.session:
        events_path = os.path.join(args.session, "events.jsonl")
        telemetry_path = os.path.join(args.session, "telemetry_001.csv")
        events = []
        if os.path.isfile(events_path):
            with open(events_path, "r", encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if line:
                        events.append(json.loads(line))
        check("events.jsonl 存在且有事件", len(events) > 0, f"{len(events)} 条")
        telemetry = []
        if os.path.isfile(telemetry_path):
            with open(telemetry_path, "r", encoding="utf-8", newline="") as handle:
                telemetry = list(csv.DictReader(handle))
        frame_ids = {int(row["frame_id"]) for row in telemetry}
        elapsed = [int(row["elapsed_us"]) for row in telemetry]
        t_min, t_max = (min(elapsed), max(elapsed)) if elapsed else (0, 0)
        if events:
            # M4 起 events.jsonl 同时包含参数事件（parameter_changed）与人工记录（manual_note），
            # 本脚本按类型分别校验（人工记录由机械调试台产生，见 M4 报告）。
            param_events = [e for e in events if e.get("type", "parameter_changed") == "parameter_changed"]
            manual_events = [e for e in events if e.get("type") == "manual_note"]
            ev = param_events[0] if param_events else events[0]
            check("事件时间在会话范围内", t_min <= ev["elapsed_us"] <= t_max,
                  f"{ev['elapsed_us']} in [{t_min}, {t_max}]")
            check("事件帧号存在于遥测", ev["frame_id"] in frame_ids,
                  f"frame_id={ev['frame_id']}")
            check("参数事件结构与来源完整",
                  all(k in e for e in param_events
                      for k in ("elapsed_us", "frame_id", "source",
                                "parameter", "old_value", "new_value", "result")),
                  f"{len(param_events)} 条参数事件")
            check("只记录实际生效的修改",
                  all(e.get("result") == "applied" for e in param_events),
                  f"{len(param_events)} 条")
            if manual_events:
                check("人工记录事件结构完整",
                      all(e.get("source") == "manual" and e.get("result") == "recorded"
                          and bool(e.get("note")) for e in manual_events),
                      f"{len(manual_events)} 条人工记录")

    passed = sum(1 for _, ok, _ in CHECKS if ok)
    print(f"\n== 结果: {passed}/{len(CHECKS)} 通过 ==")
    return 0 if passed == len(CHECKS) else 1


if __name__ == "__main__":
    sys.exit(main())
