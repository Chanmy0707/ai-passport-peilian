"""陪练会话运行时与对话管线（状态机见《智能陪练详细设计.md》§3.1/§3.2）。

每设备一个 PracticeRuntime：收音频帧做能量 VAD 判停，一轮 = ASR -> LLM 结构化评分 -> TTS 回传，
逐轮落 practice_turn；收尾（practice.end / 倒计时归零 / 断连超时）算总评落 practice_session。
"""
import asyncio
import logging
import time
import uuid
from datetime import datetime, timezone

from ..db import get_db
from . import audio
from .engine import FALLBACK_REPLY, Engine, EngineError, get_engine

logger = logging.getLogger(__name__)

VAD_SILENCE_MS = 800  # 静音判停（设计 3.2：~0.8s）
VAD_MIN_SPEECH_MS = 300  # 首段有效语音最短时长，滤纯静音
MAX_UTTERANCE_MS = 15000  # 单轮发言上限，兜底判停
DISCONNECT_GRACE_SECONDS = 300  # 断连保留会话 5 分钟可续（设计 2.4）


class ScenarioNotFound(Exception):
    pass


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class PracticeRuntime:
    def __init__(self, device_id: str, engine: Engine | None = None):
        self.device_id = device_id
        self.engine = engine or get_engine()
        self.send = None  # async callable(dict | bytes)
        self.state = "idle"  # idle/ready/listening/thinking/speaking
        self.scenario: dict | None = None
        self.session_id: str | None = None
        self.started_mono: float = 0.0
        self.duration_seconds: int | None = None
        self.deadline_mono: float | None = None
        self.history: list[dict] = []
        self.user_scores: list[float] = []
        self.turns = 0
        self.frames: list[bytes] = []
        self.voiced_ms = 0
        self.silent_ms = 0
        self.tts_cancel = False
        self.finalized = False
        self.disconnected_at: float | None = None
        self.decoder = audio.make_decoder()
        self.encoder = audio.make_encoder()

    # ---------- 会话生命周期 ----------

    async def set_scenario(self, scenario_id: str) -> dict:
        with get_db() as conn:
            item = conn.execute(
                "SELECT s.id AS scenario_id, s.name, s.description, s.duration_minutes, "
                "       s.difficulty, r.name AS role_name, r.personality, r.system_prompt, "
                "       r.tts_voice "
                "FROM scenario s LEFT JOIN role_profile r ON r.id = s.role_id "
                "WHERE s.id = ?",
                (scenario_id,),
            ).fetchone()
        if item is None:
            raise ScenarioNotFound(scenario_id)
        self.scenario = dict(item)
        self.session_id = uuid.uuid4().hex
        self.started_mono = time.monotonic()
        self.duration_seconds = (
            int(self.scenario["duration_minutes"]) * 60
            if self.scenario.get("duration_minutes")
            else None
        )
        self.deadline_mono = (
            self.started_mono + self.duration_seconds if self.duration_seconds else None
        )
        self.history = []
        self.user_scores = []
        self.turns = 0
        self.finalized = False
        self.state = "ready"
        with get_db() as conn:
            conn.execute(
                "INSERT INTO practice_session (id, device_id, scenario_id, started_at, status) "
                "VALUES (?, ?, ?, ?, 'running')",
                (self.session_id, self.device_id, scenario_id, _now_iso()),
            )
        logger.info(
            "practice session %s start device=%s scenario=%s", self.session_id, self.device_id, scenario_id
        )
        return {"role_name": self.scenario.get("role_name") or self.scenario.get("name")}

    def remaining_seconds(self) -> int | None:
        if self.deadline_mono is None:
            return None
        return max(0, int(self.deadline_mono - time.monotonic()))

    def system_prompt(self) -> str:
        s = self.scenario or {}
        parts = ["你正在与学员进行场景对话陪练练习。"]
        if s.get("role_name"):
            parts.append(f"你的角色是「{s['role_name']}」。")
        if s.get("personality"):
            parts.append(f"角色人设：{s['personality']}")
        if s.get("system_prompt"):
            parts.append(s["system_prompt"])
        if s.get("description"):
            parts.append(f"场景说明：{s['description']}")
        parts.append("始终保持角色身份与学员对话，并在每轮评估学员表现（0-100 分）。")
        return "\n".join(parts)

    # ---------- 音频与对话 ----------

    async def on_audio(self, frame: bytes) -> None:
        if self.state != "listening":
            return
        self.frames.append(frame)
        pcm = audio.decode_frame(self.decoder, frame)
        if audio.is_silence(pcm):
            if self.voiced_ms >= VAD_MIN_SPEECH_MS:
                self.silent_ms += audio.FRAME_MS
                if self.silent_ms >= VAD_SILENCE_MS:
                    await self._process_utterance()
        else:
            self.voiced_ms += audio.FRAME_MS
            self.silent_ms = 0
        if self.voiced_ms >= MAX_UTTERANCE_MS:
            await self._process_utterance()

    async def on_listen(self, state: str) -> None:
        if state == "start" and self.state in ("ready", "listening"):
            self.state = "listening"
            self._reset_utterance()
        elif state == "stop" and self.state == "listening":
            if self.frames:
                await self._process_utterance()
            else:
                self.state = "ready"

    async def on_abort(self) -> None:
        """用户打断（tts.stop）：取消当前 TTS 回传，回到 ready。"""
        if self.state == "speaking":
            self.tts_cancel = True

    def _reset_utterance(self) -> None:
        self.frames = []
        self.voiced_ms = 0
        self.silent_ms = 0

    async def _process_utterance(self) -> None:
        frames, self.frames = self.frames, []
        self.voiced_ms = 0
        self.silent_ms = 0
        if not frames:
            self.state = "ready"
            return
        self.state = "thinking"
        audio_bytes = b"".join(frames)
        try:
            text = (await self.engine.asr(audio_bytes)).strip()
        except EngineError as e:
            logger.warning("practice session %s ASR 失败: %s", self.session_id, e)
            await self._speak_text_only("没有听清，请再说一遍。")
            self.state = "ready"
            return
        if not text:
            await self._speak_text_only("没有听清，请再说一遍。")
            self.state = "ready"
            return
        await self._emit({"type": "stt", "text": text})
        self.history.append({"role": "user", "content": text})

        score, feedback = None, None
        try:
            result = await asyncio.wait_for(
                self.engine.llm(self.system_prompt(), self.history),
                timeout=5,  # LLM_TIMEOUT_SECONDS
            )
            reply = result["reply"]
            score, feedback = result.get("score"), result.get("feedback")
        except (EngineError, asyncio.TimeoutError) as e:
            logger.warning("practice session %s LLM 降级: %s", self.session_id, e)
            reply = FALLBACK_REPLY
        self.history.append({"role": "assistant", "content": reply})
        self.turns += 1
        if score is not None:
            self.user_scores.append(float(score))
        self._save_turn("user", text, score, feedback)
        self._save_turn("ai", reply, None, None)
        await self._speak(reply)

    async def _speak(self, text: str) -> None:
        """TTS 回传；失败降级文本上屏（设计 §2.4）。"""
        self.state = "speaking"
        self.tts_cancel = False
        voice = (self.scenario or {}).get("tts_voice") or ""
        try:
            pcm = await self.engine.tts_pcm(text, voice)
            frames = audio.encode_pcm(self.encoder, pcm)
        except EngineError as e:
            logger.warning("practice session %s TTS 降级: %s", self.session_id, e)
            frames = []
        await self._emit({"type": "tts", "state": "sentence_start", "text": text})
        await self._emit({"type": "tts", "state": "start"})
        for frame in frames:
            if self.tts_cancel:
                break
            await self._emit(frame)
        await self._emit({"type": "tts", "state": "stop"})
        self.state = "ready"

    async def _speak_text_only(self, text: str) -> None:
        await self._emit({"type": "tts", "state": "sentence_start", "text": text})
        await self._emit({"type": "tts", "state": "stop"})

    # ---------- 收尾与落库 ----------

    async def on_end(self) -> dict | None:
        """用户主动结束/倒计时归零：生成并下发评分摘要。"""
        if self.session_id is None or self.finalized:
            return None
        report = await self._finalize()
        await self._emit(
            {
                "type": "practice",
                "event": "report",
                "data": report,
            }
        )
        return report

    async def on_disconnect_timeout(self) -> None:
        """断连超过保留期：落库收尾，不下发 report（设计 §2.4）。"""
        if self.session_id is not None and not self.finalized:
            await self._finalize()

    async def _finalize(self) -> dict:
        self.finalized = True
        overall = round(sum(self.user_scores) / len(self.user_scores), 1) if self.user_scores else 0
        duration_s = int(time.monotonic() - self.started_mono) if self.started_mono else 0
        feedback = ""
        if self.history:
            try:
                feedback = await asyncio.wait_for(
                    self.engine.summarize(self.system_prompt(), self.history), timeout=5
                )
            except (EngineError, asyncio.TimeoutError) as e:
                logger.warning("practice session %s 总评降级: %s", self.session_id, e)
        feedback = feedback or f"完成 {self.turns} 轮练习。"
        with get_db() as conn:
            conn.execute(
                "UPDATE practice_session SET ended_at = ?, overall_score = ?, status = 'done' "
                "WHERE id = ?",
                (_now_iso(), overall, self.session_id),
            )
        logger.info(
            "practice session %s done turns=%d score=%s", self.session_id, self.turns, overall
        )
        self.state = "idle"
        return {
            "overall_score": overall,
            "duration_s": duration_s,
            "turns": self.turns,
            "feedback": feedback,
        }

    def _save_turn(self, role: str, content: str, score: float | None, feedback: str | None) -> None:
        with get_db() as conn:
            conn.execute(
                "INSERT INTO practice_turn (session_id, role, content, score, feedback, ts) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (self.session_id, role, content, score, feedback, _now_iso()),
            )

    async def _emit(self, msg: dict | bytes) -> None:
        if self.send is None:
            return
        try:
            await self.send(msg)
        except Exception as e:  # noqa: BLE001 断连期间消息丢弃即可
            logger.debug("practice emit 失败: %s", e)
