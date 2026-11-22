"""MiMo TTS 音色接口（管理端-智能陪练-角色配置）。

- GET  /api/v1/tts/voices   预置音色列表（mimo-v2.5-tts 模型文档音色表，MiMo 无动态列表接口）
- POST /api/v1/tts/preview  试听：调 MiMo TTS 合成返回 mp3 音频流

鉴权 X-Admin-Token；空 MIMO_API_KEY 时 preview 返回 503（不发起联网调用）。
TTS 调用：POST {MIMO_BASE_URL}/chat/completions，api-key 头，
目标文本放 role=assistant 消息，audio.voice 传预置音色 ID，返回 choices[0].message.audio.data（base64）。
"""
import base64
import logging

import httpx
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel

from ..config import settings
from ..models import fail, ok
from .auth import check_admin

router = APIRouter()
logger = logging.getLogger(__name__)

TTS_TIMEOUT_SECONDS = 30
PREVIEW_TEXT = "您好，我是您的智能陪练角色，很高兴见到您。"

# mimo-v2.5-tts 预置音色（官方模型文档音色表）
VOICES = [
    {"id": "mimo_default", "name": "MiMo-默认", "language": "", "gender": ""},
    {"id": "冰糖", "name": "冰糖", "language": "中文", "gender": "女性"},
    {"id": "茉莉", "name": "茉莉", "language": "中文", "gender": "女性"},
    {"id": "苏打", "name": "苏打", "language": "中文", "gender": "男性"},
    {"id": "白桦", "name": "白桦", "language": "中文", "gender": "男性"},
    {"id": "Mia", "name": "Mia", "language": "英文", "gender": "女性"},
    {"id": "Chloe", "name": "Chloe", "language": "英文", "gender": "女性"},
    {"id": "Milo", "name": "Milo", "language": "英文", "gender": "男性"},
    {"id": "Dean", "name": "Dean", "language": "英文", "gender": "男性"},
]


class TtsPreviewRequest(BaseModel):
    voice: str = "mimo_default"
    text: str = ""


def _unauthorized() -> JSONResponse:
    return JSONResponse(status_code=401, content=fail("UNAUTHORIZED", "管理员令牌无效"))


@router.get("/tts/voices")
def list_voices(request: Request):
    if check_admin(request):
        return _unauthorized()
    return ok(VOICES)


@router.post("/tts/preview")
async def preview_voice(body: TtsPreviewRequest, request: Request):
    if check_admin(request):
        return _unauthorized()
    voice = body.voice if any(v["id"] == body.voice for v in VOICES) else "mimo_default"
    text = (body.text or "").strip() or PREVIEW_TEXT
    if not settings.mimo_api_key:
        return JSONResponse(status_code=503, content=fail("TTS_UNAVAILABLE", "MIMO_API_KEY 未配置，无法试听"))
    payload = {
        "model": settings.mimo_tts_model,
        "messages": [{"role": "assistant", "content": text}],
        "audio": {"format": "mp3", "voice": voice},
    }
    headers = {"api-key": settings.mimo_api_key}
    try:
        async with httpx.AsyncClient(timeout=TTS_TIMEOUT_SECONDS) as client:
            resp = await client.post(
                f"{settings.mimo_base_url.rstrip('/')}/chat/completions",
                json=payload,
                headers=headers,
            )
            resp.raise_for_status()
            audio_b64 = resp.json()["choices"][0]["message"]["audio"]["data"]
    except Exception as e:  # noqa: BLE001 试听降级不挑异常类型
        logger.warning("MiMo TTS 试听失败 voice=%s: %s", voice, e)
        return JSONResponse(status_code=502, content=fail("TTS_FAILED", f"MiMo TTS 合成失败: {e}"))
    return Response(content=base64.b64decode(audio_b64), media_type="audio/mpeg")
