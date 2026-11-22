"""I1 录音流协议（设计文档 §5.2）：WS /record/v1/

控制消息为 JSON，音频为二进制帧（8 字节帧头：seq u32 + ts u32，小端，+ Opus 载荷）。

消息序列：
  设备→服务  hello       {device_id, token, resume_from_seq}   建连与续传声明（0=全新会话）
  服务→设备  hello_ack   {session_id, acked_seq, server_time}  分配会话，返回已确认位置
  设备→服务  [二进制音频帧]                                     持续发送，按 seq 幂等去重
  服务→设备  record.ack  {acked_seq}                           每 50 帧或 1 秒确认一次
  设备→服务  badge.get   {device_id}                           拉取工牌配置
  服务→设备  badge.data  {name, title, employee_no, qr_text, avatar_id}  应答（头像取预置动态 emoji 素材集，缺省按 name 首字静态占位）
  设备→服务  record.stop {session_id}                          会话结束，触发转写流水线

语义约定：acked_seq 为"已确认的最大连续 seq"（-1 表示未确认），设备断网后从 acked_seq+1 补传（§8）。
认证：hello.token 为烧录固件的预共享令牌（DEVICE_TOKEN），错误直接断连。
错误：JSON {error: {code, message}}。
"""
import asyncio
import json
import logging
import struct
import time
import uuid

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..config import settings
from ..db import get_db, one
from ..storage.audio import AudioStore
from ..transcribe import pipeline

logger = logging.getLogger(__name__)
router = APIRouter()

FRAME_HEADER_SIZE = 8  # seq u32 + ts u32
ACK_EVERY_FRAMES = 50  # §5.2：每 50 帧或 1 秒确认一次
ACK_EVERY_SECONDS = 1.0


def _err(code: str, message: str) -> str:
    return json.dumps({"error": {"code": code, "message": message}}, ensure_ascii=False)


def _touch_device(device_id: str) -> None:
    with get_db() as conn:
        conn.execute(
            "INSERT OR IGNORE INTO device (device_id, name, last_seen) VALUES (?, ?, ?)",
            (device_id, device_id, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())),
        )
        conn.execute(
            "UPDATE device SET last_seen = ? WHERE device_id = ?",
            (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), device_id),
        )


def _find_resumable_session(device_id: str) -> dict | None:
    with get_db() as conn:
        return one(
            conn.execute(
                "SELECT * FROM recording_session WHERE device_id = ? AND status = 'uploading' "
                "ORDER BY started_at DESC LIMIT 1",
                (device_id,),
            )
        )


def _create_session(device_id: str) -> str:
    session_id = uuid.uuid4().hex
    with get_db() as conn:
        conn.execute(
            "INSERT INTO recording_session (id, device_id, started_at, status, acked_seq) "
            "VALUES (?, ?, ?, 'uploading', -1)",
            (
                session_id,
                device_id,
                time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            ),
        )
    return session_id


@router.websocket("/record/v1/")
async def record_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    store = AudioStore()

    # ---- hello 握手 ----
    try:
        raw = await ws.receive_text()
    except WebSocketDisconnect:
        return
    try:
        hello = json.loads(raw)
    except (ValueError, TypeError):
        await ws.send_text(_err("BAD_REQUEST", "首条消息必须是 JSON hello"))
        await ws.close(code=1008)
        return

    if hello.get("type", "hello") != "hello":
        await ws.send_text(_err("BAD_REQUEST", "首条消息必须是 hello"))
        await ws.close(code=1008)
        return

    device_id = hello.get("device_id") or ""
    token = hello.get("token") or ""
    if not device_id or token != settings.device_token:
        # §5.2：缺失或错误服务端直接断连
        await ws.send_text(_err("UNAUTHORIZED", "设备令牌无效"))
        await ws.close(code=1008)
        return

    resume_from_seq = int(hello.get("resume_from_seq") or 0)
    _touch_device(device_id)

    if resume_from_seq > 0:
        # 断网重连续传：接管该设备最近一个 uploading 会话（acked_seq 以磁盘为准，§6.2）
        session = _find_resumable_session(device_id)
    else:
        session = None

    if session:
        session_id = session["id"]
        acked_seq = store.contiguous_acked(session_id)
        with get_db() as conn:
            conn.execute(
                "UPDATE recording_session SET acked_seq = ? WHERE id = ?",
                (acked_seq, session_id),
            )
    else:
        session_id = _create_session(device_id)
        acked_seq = -1

    await ws.send_text(
        json.dumps(
            {
                "type": "hello_ack",
                "session_id": session_id,
                "acked_seq": acked_seq,
                "server_time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            },
            ensure_ascii=False,
        )
    )

    # ---- 主收帧循环 ----
    frames_since_ack = 0
    last_ack_time = time.monotonic()

    def flush_ack() -> str:
        nonlocal frames_since_ack, last_ack_time
        frames_since_ack = 0
        last_ack_time = time.monotonic()
        return json.dumps(
            {"type": "record.ack", "acked_seq": store.contiguous_acked(session_id)}
        )

    try:
        while True:
            msg = await ws.receive()
            if msg.get("type") == "websocket.disconnect":
                break

            if msg.get("bytes") is not None:
                frame = msg["bytes"]
                if len(frame) <= FRAME_HEADER_SIZE:
                    await ws.send_text(_err("BAD_REQUEST", "音频帧过短"))
                    continue
                seq, _ts = struct.unpack("<II", frame[:FRAME_HEADER_SIZE])
                is_new, conflict = store.write_frame(session_id, seq, frame[FRAME_HEADER_SIZE:])
                if conflict:
                    logger.warning("session=%s seq=%d 补传冲突已覆盖", session_id, seq)
                if is_new:
                    with get_db() as conn:
                        conn.execute(
                            "UPDATE recording_session SET audio_bytes = ? WHERE id = ?",
                            (store.total_bytes(session_id), session_id),
                        )
                frames_since_ack += 1
                if frames_since_ack >= ACK_EVERY_FRAMES or (
                    time.monotonic() - last_ack_time
                ) >= ACK_EVERY_SECONDS:
                    await ws.send_text(flush_ack())
                continue

            if msg.get("text") is not None:
                try:
                    ctl = json.loads(msg["text"])
                except ValueError:
                    await ws.send_text(_err("BAD_REQUEST", "控制消息必须是 JSON"))
                    continue
                ctype = ctl.get("type")

                if ctype == "badge.get":
                    with get_db() as conn:
                        badge = one(
                            conn.execute(
                                "SELECT name, title, employee_no, qr_text, avatar_id, updated_at "
                                "FROM badge_profile WHERE device_id = ?",
                                (ctl.get("device_id") or device_id,),
                            )
                        )
                    await ws.send_text(
                        json.dumps(
                            {
                                "type": "badge.data",
                                # 未配置工牌时返回空占位，头像缺省按 name 首字静态占位（§5.2）
                                **(badge or {"name": "", "title": "", "employee_no": "", "qr_text": "", "avatar_id": None, "updated_at": None}),
                            },
                            ensure_ascii=False,
                        )
                    )

                elif ctype == "record.stop":
                    if ctl.get("session_id") and ctl["session_id"] != session_id:
                        await ws.send_text(_err("BAD_REQUEST", "session_id 不匹配"))
                        continue
                    with get_db() as conn:
                        conn.execute(
                            "UPDATE recording_session SET status = 'done', ended_at = ?, acked_seq = ? WHERE id = ?",
                            (
                                time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                                store.contiguous_acked(session_id),
                                session_id,
                            ),
                        )
                    # 会话结束触发转写流水线（§4.1 异步执行）
                    asyncio.create_task(pipeline.process_session(session_id))
                    await ws.send_text(
                        json.dumps({"type": "record.stopped", "session_id": session_id})
                    )
                    break

                else:
                    await ws.send_text(_err("BAD_REQUEST", f"未知控制消息: {ctype}"))
    except WebSocketDisconnect:
        pass
    finally:
        # 异常断连：会话保持 uploading，供设备按 acked_seq 续传（§8）
        logger.info("record ws 断开 session=%s acked=%d", session_id, store.contiguous_acked(session_id))
