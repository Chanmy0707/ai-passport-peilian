"""对话引擎抽象：MiMo 真实实现 + Mock 实现（PRACTICE_MOCK_MODELS=1 切换，供冒烟/无网联调）。

LLM/TTS 复用 MiMo chat/completions 约定（与 app/api/tts.py 一致）：
- LLM：messages 轮询，assistant 输出严格 JSON {reply, score, feedback}；
- TTS：目标文本放 role=assistant，audio.voice 传音色，choices[0].message.audio.data 为 base64 音频。
设计见《智能陪练详细设计.md》§3.3。
"""
import base64
import json
import logging
import re

import httpx

from ..config import settings
from ..transcribe.pipeline import _call_mimo_asr

logger = logging.getLogger(__name__)

LLM_TIMEOUT_SECONDS = 5  # 设计：首 token >5s 回退固定话术（MVP 用整调超时近似）
TTS_TIMEOUT_SECONDS = 30

FALLBACK_REPLY = "我需要想一下，请再说一遍。"
ASR_RETRY_FALLBACK = "没有听清，请再说一遍。"

JSON_INSTRUCTION = (
    '你是陪练教练兼场景角色。每次回复必须只输出一个 JSON 对象，格式为'
    '{"reply": "以场景角色身份回复学员的内容", "score": 0-100 的整数, '
    '"feedback": "对学员这一轮表现的一句话纠错建议"}，不要输出其他文字。'
)


class EngineError(Exception):
    pass


class Engine:
    async def asr(self, opus_audio: bytes) -> str:
        """合并的 Opus 音频 -> 用户文本（失败抛 EngineError）。"""
        raise NotImplementedError

    async def llm(self, system: str, history: list[dict]) -> dict:
        """返回 {"reply": str, "score": float|None, "feedback": str|None}。"""
        raise NotImplementedError

    async def tts_pcm(self, text: str, voice: str) -> bytes:
        """文本 -> PCM16 16k mono（失败抛 EngineError）。"""
        raise NotImplementedError

    async def summarize(self, system: str, history: list[dict]) -> str:
        """整场练习的一句话总评（失败返回空串）。"""
        raise NotImplementedError


class MiMoEngine(Engine):
    def _url(self) -> str:
        return f"{settings.mimo_base_url.rstrip('/')}/chat/completions"

    def _headers(self) -> dict:
        return {"api-key": settings.mimo_api_key}

    async def asr(self, opus_audio: bytes) -> str:
        if not settings.mimo_api_key:
            raise EngineError("MIMO_API_KEY 未配置")
        try:
            return await _call_mimo_asr(opus_audio)
        except Exception as e:  # noqa: BLE001
            raise EngineError(f"ASR 失败: {e}") from e

    async def llm(self, system: str, history: list[dict]) -> dict:
        if not settings.mimo_api_key:
            raise EngineError("MIMO_API_KEY 未配置")
        payload = {
            "model": settings.mimo_llm_model,
            "messages": [{"role": "system", "content": f"{system}\n{JSON_INSTRUCTION}"}, *history],
        }
        try:
            async with httpx.AsyncClient(timeout=LLM_TIMEOUT_SECONDS) as client:
                resp = await client.post(self._url(), json=payload, headers=self._headers())
                resp.raise_for_status()
                content = resp.json()["choices"][0]["message"]["content"]
        except Exception as e:  # noqa: BLE001
            raise EngineError(f"LLM 失败: {e}") from e
        return _parse_llm_json(content)

    async def tts_pcm(self, text: str, voice: str) -> bytes:
        if not settings.mimo_api_key:
            raise EngineError("MIMO_API_KEY 未配置")
        payload = {
            "model": settings.mimo_tts_model,
            "messages": [{"role": "assistant", "content": text}],
            "audio": {"format": "pcm", "voice": voice or settings.mimo_tts_voice or "mimo_default"},
        }
        try:
            async with httpx.AsyncClient(timeout=TTS_TIMEOUT_SECONDS) as client:
                resp = await client.post(self._url(), json=payload, headers=self._headers())
                resp.raise_for_status()
                audio_b64 = resp.json()["choices"][0]["message"]["audio"]["data"]
            return base64.b64decode(audio_b64)
        except Exception as e:  # noqa: BLE001
            raise EngineError(f"TTS 失败: {e}") from e

    async def summarize(self, system: str, history: list[dict]) -> str:
        if not settings.mimo_api_key:
            return ""
        dialogue = "\n".join(f"{t['role']}: {t['content']}" for t in history)
        payload = {
            "model": settings.mimo_llm_model,
            "messages": [
                {
                    "role": "system",
                    "content": f"{system}\n你是陪练教练。根据练习对话记录给学员一句总评，不超过 40 字，只输出这句话。",
                },
                {"role": "user", "content": dialogue},
            ],
        }
        try:
            async with httpx.AsyncClient(timeout=LLM_TIMEOUT_SECONDS) as client:
                resp = await client.post(self._url(), json=payload, headers=self._headers())
                resp.raise_for_status()
                return resp.json()["choices"][0]["message"]["content"].strip()
        except Exception as e:  # noqa: BLE001
            logger.warning("LLM 总评失败: %s", e)
            return ""


class MockEngine(Engine):
    """确定性假实现：冒烟与无网联调用，不发起任何联网调用。"""

    async def asr(self, opus_audio: bytes) -> str:
        return "你好，我想了解一下这个产品。"

    async def llm(self, system: str, history: list[dict]) -> dict:
        user_turns = sum(1 for t in history if t["role"] == "user")
        return {
            "reply": f"（模拟角色）好的，我们继续第 {user_turns} 轮练习。",
            "score": min(60 + 5 * user_turns, 95),
            "feedback": "（模拟）表述清晰，注意语速。",
        }

    async def tts_pcm(self, text: str, voice: str) -> bytes:
        return b"\x00" * 5760  # 3 帧 60ms 静音 PCM

    async def summarize(self, system: str, history: list[dict]) -> str:
        return "（模拟）完成本次练习，继续保持。"


def _parse_llm_json(content: str) -> dict:
    """解析 LLM 的 {reply, score, feedback}；容忍代码块围栏/前后杂文。"""
    text = content.strip()
    if text.startswith("```"):
        text = re.sub(r"^```[a-zA-Z]*\s*|\s*```$", "", text).strip()
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        match = re.search(r"\{.*\}", text, re.DOTALL)
        if not match:
            return {"reply": text, "score": None, "feedback": None}
        try:
            obj = json.loads(match.group(0))
        except json.JSONDecodeError:
            return {"reply": text, "score": None, "feedback": None}
    reply = str(obj.get("reply") or text)
    score = obj.get("score")
    try:
        score = float(score) if score is not None else None
    except (TypeError, ValueError):
        score = None
    feedback = obj.get("feedback")
    return {"reply": reply, "score": score, "feedback": str(feedback) if feedback else None}


_engine: Engine | None = None


def get_engine() -> Engine:
    global _engine
    if _engine is None:
        _engine = MockEngine() if settings.practice_mock_models else MiMoEngine()
    return _engine
