"""WS 录音链路冒烟：模拟设备走 hello → 4 帧（3 个唯一 seq + 1 个重复 seq）→ badge.get → record.stop。

断言（计划验证项 3/4）：
  1. 收到 hello_ack（含 session_id、acked_seq=-1）与至少一条 record.ack
  2. 收到 badge.data 与 record.stopped
  3. 音频目录落盘 3 个不重复 seq 分片（000000/000001/000002.opus），重复帧被去重
  4. DB：会话 status=done；1 个分段（seq 0-2）转写走 §8 降级标 failed；无 transcript
     （依赖服务端以假 key 或空 MIMO_API_KEY 运行，smoke.sh 即如此启动）

用法：python3 scripts/ws_smoke.py（服务端需已启动；WS_URL/DEVICE_TOKEN/DB_PATH/AUDIO_DIR 可用环境变量覆盖）
"""
import asyncio
import json
import os
import sqlite3
import struct
import sys
import time
from pathlib import Path

import websockets

SERVER_ROOT = Path(__file__).resolve().parent.parent


def _path(env_name: str, default: str) -> Path:
    p = Path(os.environ.get(env_name) or default)
    return p if p.is_absolute() else SERVER_ROOT / p


WS_URL = os.environ.get("WS_URL", "ws://127.0.0.1:8000/record/v1/")
DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", "device-dev-token")
DEVICE_ID = "smoke-device"
DB_PATH = _path("DB_PATH", "peilian.db")
AUDIO_DIR = _path("AUDIO_DIR", "audio")


def frame(seq: int, ts: int = 0) -> bytes:
    """二进制音频帧：8 字节头（seq u32 + ts u32 小端）+ Opus 载荷（冒烟用假载荷）。"""
    return struct.pack("<II", seq, ts) + bytes([seq + 1]) * 32


async def run() -> str:
    async with websockets.connect(WS_URL) as ws:
        # ---- hello 握手 ----
        await ws.send(
            json.dumps(
                {"type": "hello", "device_id": DEVICE_ID, "token": DEVICE_TOKEN, "resume_from_seq": 0}
            )
        )
        hello_ack = json.loads(await asyncio.wait_for(ws.recv(), 10))
        assert hello_ack.get("type") == "hello_ack", f"hello_ack 异常: {hello_ack}"
        session_id = hello_ack["session_id"]
        assert hello_ack["acked_seq"] == -1, f"全新会话 acked_seq 应为 -1: {hello_ack}"
        print(f"[PASS] hello_ack session_id={session_id}")

        # ---- 音频帧：seq 0 / 1 / 2 + 重复 seq 1（模拟补传，应被去重）----
        await ws.send(frame(0))
        await asyncio.sleep(1.2)  # 让服务端按“每 50 帧或 1 秒”节流回 record.ack
        await ws.send(frame(1))
        await ws.send(frame(2))
        await ws.send(frame(1))

        # ---- 控制消息 ----
        await ws.send(json.dumps({"type": "badge.get", "device_id": DEVICE_ID}))
        await ws.send(json.dumps({"type": "record.stop", "session_id": session_id}))

        msgs = []
        try:
            while True:
                msgs.append(json.loads(await asyncio.wait_for(ws.recv(), 10)))
        except (asyncio.TimeoutError, websockets.ConnectionClosed):
            pass

    # ---- 断言服务端消息 ----
    types = [m.get("type") for m in msgs]
    acks = [m for m in msgs if m.get("type") == "record.ack"]
    assert acks, f"未收到 record.ack: {msgs}"
    assert acks[-1]["acked_seq"] >= 0, f"acked_seq 异常: {acks[-1]}"
    print(f"[PASS] record.ack x{len(acks)} acked_seq={acks[-1]['acked_seq']}")
    assert "badge.data" in types, f"未收到 badge.data: {msgs}"
    print("[PASS] badge.data")
    assert "record.stopped" in types, f"未收到 record.stopped: {msgs}"
    assert msgs[types.index("record.stopped")]["session_id"] == session_id
    print("[PASS] record.stopped")

    # ---- 断言落盘：3 个不重复分片 ----
    sess_dir = AUDIO_DIR / session_id
    frames = sorted(p.name for p in sess_dir.glob("*.opus") if p.stem.isdigit())
    assert frames == ["000000.opus", "000001.opus", "000002.opus"], f"落盘分片异常: {frames}"
    print("[PASS] 落盘 3 个不重复分片（重复 seq 已去重）")

    # ---- 断言 DB：会话 done、分段降级 failed、无 transcript ----
    deadline = time.monotonic() + 15
    session_row, segs, n_transcript = None, [], -1
    while time.monotonic() < deadline:
        with sqlite3.connect(DB_PATH) as conn:
            conn.row_factory = sqlite3.Row
            session_row = conn.execute(
                "SELECT * FROM recording_session WHERE id = ?", (session_id,)
            ).fetchone()
            segs = [
                dict(r)
                for r in conn.execute(
                    "SELECT * FROM audio_segment WHERE session_id = ? ORDER BY seq_start",
                    (session_id,),
                ).fetchall()
            ]
            n_transcript = conn.execute(
                "SELECT COUNT(*) AS n FROM transcript t JOIN audio_segment a ON t.segment_id = a.id "
                "WHERE a.session_id = ?",
                (session_id,),
            ).fetchone()["n"]
        if session_row and session_row["status"] == "done" and segs and segs[0]["status"] in ("failed", "transcribed"):
            break
        await asyncio.sleep(0.5)

    assert session_row is not None, "DB 无会话记录"
    assert session_row["status"] == "done", f"会话状态异常: {session_row['status']}"
    assert len(segs) == 1, f"分段数异常（3 帧应切 1 段）: {segs}"
    seg = segs[0]
    assert (seg["seq_start"], seg["seq_end"]) == (0, 2), f"分段 seq 区间异常: {seg}"
    assert seg["duration_ms"] == 3 * 60, f"分段时长异常: {seg}"
    assert seg["file_url"] == f"/audio/{session_id}/000000-000002.opus", f"file_url 异常: {seg}"
    assert seg["status"] == "failed", f"分段应走 §8 降级标 failed: {seg}"
    assert (sess_dir / "000000-000002.opus").exists(), "合并分段文件缺失"
    assert n_transcript == 0, f"降级后不应有 transcript: {n_transcript}"
    print("[PASS] DB 会话 done、分段降级 failed、无 transcript")
    return session_id


if __name__ == "__main__":
    try:
        sid = asyncio.run(run())
    except Exception as e:  # noqa: BLE001
        print(f"[FAIL] WS 冒烟: {e}", file=sys.stderr)
        raise SystemExit(1)
    print(f"WS SMOKE PASS session={sid}")
