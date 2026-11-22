"""小智 WebSocket 协议子集陪练端点（WS /xiaozhi/v1/）。

契约见《智能陪练详细设计.md》§2：
- 握手鉴权：Authorization: Bearer <DEVICE_TOKEN> / X-Device-Token，Device-Id 头（MAC）；
- hello：服务端回 {type: hello, transport, session_id, audio_params 16k}；
- 二进制帧：v3 头（1B type=0 + 1B 保留 + 2B BE 长度 + Opus 载荷），入向兼容裸 Opus；
- JSON：listen / abort / mcp(set_scenario) / practice(end) 入向，stt / tts / mcp 结果 / practice(tick,report) 出向。
"""
import asyncio
import json
import logging
import time
import uuid

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..config import settings
from .pipeline import DISCONNECT_GRACE_SECONDS, PracticeRuntime, ScenarioNotFound

logger = logging.getLogger(__name__)

router = APIRouter()

TICK_INTERVAL_SECONDS = 30
WATCH_INTERVAL_SECONDS = 5

# device_id -> runtime（断连 5 分钟内同设备重连可续，设计 §2.4）
_runtimes: dict[str, PracticeRuntime] = {}


def _auth_ok(ws: WebSocket) -> bool:
    auth = ws.headers.get("authorization", "")
    token = auth[7:].strip() if auth.startswith("Bearer ") else ""
    return token == settings.device_token or ws.headers.get("x-device-token") == settings.device_token


def _parse_frame(data: bytes) -> bytes:
    """剥 v3 帧头；裸 Opus（v0/v1）原样返回。"""
    if len(data) >= 4 and data[0] == 0 and data[1] == 0:
        size = int.from_bytes(data[2:4], "big")
        if size == len(data) - 4:
            return data[4:]
    return data


def _pack_frame(payload: bytes) -> bytes:
    return b"\x00\x00" + len(payload).to_bytes(2, "big") + payload


class _Link:
    """WebSocket 收发封装：JSON 文本与 v3 二进制帧。"""

    def __init__(self, ws: WebSocket):
        self.ws = ws
        self.lock = asyncio.Lock()

    async def send(self, msg: dict | bytes) -> None:
        async with self.lock:
            if isinstance(msg, bytes):
                await self.ws.send_bytes(_pack_frame(msg))
            else:
                await self.ws.send_text(json.dumps(msg, ensure_ascii=False))


@router.websocket("/xiaozhi/v1/")
async def practice_endpoint(ws: WebSocket):
    if not _auth_ok(ws):
        await ws.close(code=1008)
        return
    device_id = ws.headers.get("device-id") or f"dev-{uuid.uuid4().hex[:8]}"
    await ws.accept()

    # 会话恢复：同设备 5 分钟内重连接续原 runtime
    runtime = _runtimes.get(device_id)
    if runtime is not None:
        if runtime.finalized or (
            runtime.disconnected_at
            and time.monotonic() - runtime.disconnected_at > DISCONNECT_GRACE_SECONDS
        ):
            _runtimes.pop(device_id, None)
            runtime = None
    if runtime is None:
        runtime = PracticeRuntime(device_id)
        _runtimes[device_id] = runtime
    runtime.disconnected_at = None
    link = _Link(ws)
    runtime.send = link.send

    watch_task = asyncio.create_task(_watch_loop(runtime))
    try:
        await _handshake(ws, link)
        await _dispatch_loop(ws, runtime)
    except WebSocketDisconnect:
        pass
    except Exception as e:  # noqa: BLE001 连接级异常记日志后关闭
        logger.warning("practice ws device=%s 异常: %s", device_id, e)
    finally:
        watch_task.cancel()
        runtime.disconnected_at = time.monotonic()
        runtime.send = None
        asyncio.get_event_loop().create_task(
            _grace_finalize(device_id, runtime)
        )


async def _grace_finalize(device_id: str, runtime: PracticeRuntime) -> None:
    await asyncio.sleep(DISCONNECT_GRACE_SECONDS)
    if _runtimes.get(device_id) is runtime and runtime.disconnected_at is not None:
        await runtime.on_disconnect_timeout()
        _runtimes.pop(device_id, None)


async def _handshake(ws: WebSocket, link: _Link) -> None:
    raw = await ws.receive_text()
    hello = json.loads(raw)
    if hello.get("type") != "hello":
        await ws.close(code=1002)
        return
    await link.send(
        {
            "type": "hello",
            "version": 3,
            "transport": "websocket",
            "session_id": uuid.uuid4().hex,
            "audio_params": {
                "format": "opus",
                "sample_rate": 16000,
                "channels": 1,
                "frame_duration": 60,
            },
        }
    )


async def _dispatch_loop(ws: WebSocket, runtime: PracticeRuntime) -> None:
    while True:
        msg = await ws.receive()
        if msg["type"] == "websocket.disconnect":
            raise WebSocketDisconnect(msg.get("code", 1000))
        if msg.get("bytes") is not None:
            await runtime.on_audio(_parse_frame(msg["bytes"]))
        elif msg.get("text") is not None:
            await _handle_json(ws, runtime, json.loads(msg["text"]))


async def _handle_json(ws: WebSocket, runtime: PracticeRuntime, msg: dict) -> None:
    mtype = msg.get("type")
    if mtype == "listen":
        await runtime.on_listen(msg.get("state", ""))
    elif mtype == "abort":
        await runtime.on_abort()
    elif mtype == "mcp":
        await _handle_mcp(runtime, msg.get("payload") or {})
    elif mtype == "practice":
        if msg.get("event") == "end":
            await runtime.on_end()
        else:
            logger.debug("practice 未知 event: %s", msg.get("event"))
    elif mtype == "hello":
        pass
    else:
        logger.debug("practice 未知消息 type=%s", mtype)


async def _handle_mcp(runtime: PracticeRuntime, payload: dict) -> None:
    """设备->服务 MCP 调用：仅支持 set_scenario（设计 §2.2）。"""
    rid = payload.get("id")
    method = payload.get("method")
    params = payload.get("params") or {}
    if method != "tools/call" or params.get("name") != "set_scenario":
        await runtime._emit(
            {
                "type": "mcp",
                "payload": {
                    "jsonrpc": "2.0",
                    "id": rid,
                    "error": {"code": "METHOD_NOT_FOUND", "message": f"unsupported: {method}"},
                },
            }
        )
        return
    scenario_id = (params.get("arguments") or {}).get("scenario_id", "")
    try:
        result = await runtime.set_scenario(scenario_id)
    except ScenarioNotFound:
        await runtime._emit(
            {
                "type": "mcp",
                "payload": {
                    "jsonrpc": "2.0",
                    "id": rid,
                    "error": {"code": "SCENARIO_NOT_FOUND", "message": f"scenario {scenario_id} not found"},
                },
            }
        )
        return
    await runtime._emit(
        {
            "type": "mcp",
            "payload": {
                "jsonrpc": "2.0",
                "id": rid,
                "result": {
                    "content": [{"type": "text", "text": json.dumps(result, ensure_ascii=False)}],
                    "isError": False,
                },
            },
        }
    )


async def _watch_loop(runtime: PracticeRuntime) -> None:
    """倒计时兜底收尾 + practice.tick 校准（设计 §2.3）。"""
    last_tick = 0.0
    while True:
        await asyncio.sleep(WATCH_INTERVAL_SECONDS)
        if runtime.finalized or runtime.session_id is None:
            continue
        remaining = runtime.remaining_seconds()
        if remaining is not None and remaining <= 0:
            await runtime.on_end()
            continue
        if remaining is not None and time.monotonic() - last_tick >= TICK_INTERVAL_SECONDS:
            last_tick = time.monotonic()
            await runtime._emit({"type": "practice", "event": "tick", "data": {"remaining_s": remaining}})
