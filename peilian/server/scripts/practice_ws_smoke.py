"""陪练 WS 协议冒烟（mock 引擎，PRACTICE_MOCK_MODELS=1）。

覆盖《智能陪练详细设计.md》协议链路：hello 握手 -> set_scenario（成功 + SCENARIO_NOT_FOUND）
-> 一轮对话（listen + 假音频帧 + listen stop -> stt/tts 消息序列）-> practice.end -> practice.report
-> practice_session / practice_turn 落库校验。用法同 ws_smoke.py（环境变量 BASE/WS_URL2/DEVICE_TOKEN/DB_PATH）。
"""
import asyncio
import json
import os
import sqlite3
import sys
import urllib.request

import websockets

BASE = os.environ.get("BASE", "http://127.0.0.1:8000")
WS_URL2 = os.environ.get("WS_URL2", "ws://127.0.0.1:8000/xiaozhi/v1/")
DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", "device-dev-token")
ADMIN_TOKEN = os.environ.get("ADMIN_TOKEN", "admin-dev-token")
DB_PATH = os.environ.get("DB_PATH", "smoke.db")
RECV_TIMEOUT = 10


def fail(msg: str):
    print(f"[FAIL] {msg}")
    sys.exit(1)


def admin_post(path: str, body: dict) -> dict:
    req = urllib.request.Request(
        f"{BASE}{path}",
        data=json.dumps(body).encode(),
        headers={"X-Admin-Token": ADMIN_TOKEN, "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req) as resp:
        return json.load(resp)["data"]


async def recv_json(ws) -> dict:
    # 跳过 TTS 二进制 Opus 帧（opuslib 可用时服务端会下发音频帧）
    while True:
        msg = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
        if not isinstance(msg, bytes):
            return json.loads(msg)


def pack_frame(payload: bytes) -> bytes:
    return b"\x00\x00" + len(payload).to_bytes(2, "big") + payload


async def connect():
    headers = {
        "Authorization": f"Bearer {DEVICE_TOKEN}",
        "Device-Id": "smoke-practice-mac",
        "Client-Id": "smoke-practice-client",
        "Protocol-Version": "3",
    }
    try:
        return await websockets.connect(WS_URL2, additional_headers=headers)
    except TypeError:  # websockets<14 用 extra_headers
        return await websockets.connect(WS_URL2, extra_headers=headers)


async def main():
    role = admin_post("/api/v1/roles", {"name": "陪练官", "personality": "耐心", "system_prompt": "你是店长"})
    scenario = admin_post(
        "/api/v1/scenarios",
        {
            "name": "门店接待",
            "description": "迎宾与需求确认",
            "role_id": role["id"],
            "duration_minutes": 10,
            "difficulty": "入门",
        },
    )
    sid = scenario["id"]

    async with await connect() as ws:
        # 1. hello 握手
        await ws.send(json.dumps({
            "type": "hello", "version": 3, "transport": "websocket",
            "audio_params": {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60},
        }))
        hello = await recv_json(ws)
        assert hello.get("type") == "hello", hello
        assert hello.get("audio_params", {}).get("sample_rate") == 16000, hello
        print("[PASS] practice hello 握手")

        # 2. set_scenario 成功
        await ws.send(json.dumps({
            "type": "mcp",
            "payload": {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                        "params": {"name": "set_scenario", "arguments": {"scenario_id": sid}}},
        }))
        resp = await recv_json(ws)
        payload = resp.get("payload", {})
        assert payload.get("id") == 1 and "result" in payload, resp
        text = payload["result"]["content"][0]["text"]
        assert json.loads(text)["role_name"] == "陪练官", text
        print("[PASS] set_scenario 成功（返回角色名）")

        # 3. set_scenario 场景不存在
        await ws.send(json.dumps({
            "type": "mcp",
            "payload": {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                        "params": {"name": "set_scenario", "arguments": {"scenario_id": "no-such"}}},
        }))
        resp = await recv_json(ws)
        err = resp.get("payload", {}).get("error", {})
        assert err.get("code") == "SCENARIO_NOT_FOUND", resp
        print("[PASS] set_scenario 场景不存在 -> SCENARIO_NOT_FOUND")

        # 4. 一轮对话：listen start + 假音频帧 + listen stop -> stt/tts 序列
        await ws.send(json.dumps({"type": "listen", "state": "start", "mode": "manual"}))
        for _ in range(3):
            await ws.send(pack_frame(b"\x01\x02\x03"))
        await ws.send(json.dumps({"type": "listen", "state": "stop"}))

        saw_stt = saw_sentence = saw_start = saw_stop = False
        for _ in range(20):
            msg = await recv_json(ws)
            if msg.get("type") == "stt":
                saw_stt = bool(msg.get("text"))
            elif msg.get("type") == "tts":
                state = msg.get("state")
                if state == "sentence_start":
                    saw_sentence = "模拟角色" in msg.get("text", "")
                elif state == "start":
                    saw_start = True
                elif state == "stop":
                    saw_stop = True
            if saw_stt and saw_sentence and saw_start and saw_stop:
                break
        assert saw_stt and saw_sentence and saw_start and saw_stop, (
            f"stt/tts 序列不完整 stt={saw_stt} sentence={saw_sentence} start={saw_start} stop={saw_stop}"
        )
        print("[PASS] 对话轮 stt/tts 消息序列")

        # 5. practice.end -> practice.report（mock：首 user 轮 score=65）
        await ws.send(json.dumps({"type": "practice", "event": "end", "data": {}}))
        report = None
        for _ in range(10):
            msg = await recv_json(ws)
            if msg.get("type") == "practice" and msg.get("event") == "report":
                report = msg["data"]
                break
        assert report is not None, "未收到 practice.report"
        assert report["turns"] == 1, report
        assert report["overall_score"] == 65, report
        assert report["feedback"], report
        print(f"[PASS] practice.report 下发 {report}")

    # 6. 落库校验
    with sqlite3.connect(DB_PATH) as conn:
        row = conn.execute(
            "SELECT status, overall_score FROM practice_session WHERE scenario_id = ?", (sid,)
        ).fetchone()
        assert row is not None, "practice_session 未落库"
        assert row[0] == "done" and row[1] == 65, row
        turns = conn.execute(
            "SELECT role, score FROM practice_turn ORDER BY id"
        ).fetchall()
        assert len(turns) == 2, turns
        assert turns[0][0] == "user" and turns[0][1] == 65, turns
        assert turns[1][0] == "ai" and turns[1][1] is None, turns
    print("[PASS] practice_session / practice_turn 落库校验")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except SystemExit:
        raise
    except Exception as e:  # noqa: BLE001
        fail(f"practice ws 冒烟异常: {e}")
