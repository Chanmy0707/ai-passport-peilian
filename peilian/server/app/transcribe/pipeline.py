"""转写流水线（设计文档 §4.1）：VAD 切段 → MiMo ASR → transcript 入库。

- 触发：record.stop（ws/record.py 异步调 process_session）；"每 60 秒增量触发"留 P1（TODO）
- VAD 本期 stub：固定 50 帧/段，真实 VAD 后续替换（vad_segments 接口已留）
- ASR（I7）：POST {MIMO_BASE_URL}/chat/completions，鉴权头 api-key；失败重试 1 次后分段标 failed（§8 降级）
- 空 MIMO_API_KEY 立即降级，不发起联网调用
- run_quality_check()：质检 skill 扩展点占位，本期不实现

合并文件：每段裸拼接 Opus 载荷存 audio/{session_id}/{seq_start:06d}-{seq_end:06d}.opus（供 ASR）；
同段解码生成 {seq_start:06d}-{seq_end:06d}.wav，file_url 指向 WAV 供浏览器播放，解码失败回落 .opus。
"""
import base64
import logging

import httpx

from ..config import settings
from ..db import get_db, one
from ..storage.audio import AudioStore

logger = logging.getLogger(__name__)

VAD_FRAMES_PER_SEGMENT = 50  # stub：固定 50 帧/段
FRAME_DURATION_MS = 60  # xiaozhi Opus 帧 60ms（16kHz / 1200 采样）
ASR_TIMEOUT_SECONDS = 30


def vad_segments(seqs: list[int]) -> list[list[int]]:
    """VAD 切段 stub：按固定帧数把有序 seq 列表分块。返回 [[seq, ...], ...]。"""
    return [seqs[i : i + VAD_FRAMES_PER_SEGMENT] for i in range(0, len(seqs), VAD_FRAMES_PER_SEGMENT)]


async def _call_mimo_asr(audio: bytes) -> str:
    """调 MiMo ASR：OpenAI 兼容 chat/completions + api-key 头，失败重试 1 次。"""
    payload = {
        "model": settings.mimo_asr_model,
        "messages": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "input_audio",
                        "input_audio": {
                            "data": base64.b64encode(audio).decode(),
                            "format": "opus",
                        },
                    }
                ],
            }
        ],
    }
    headers = {"api-key": settings.mimo_api_key}
    last_err: Exception | None = None
    async with httpx.AsyncClient(timeout=ASR_TIMEOUT_SECONDS) as client:
        for attempt in (1, 2):  # 首次 + 重试 1 次
            try:
                resp = await client.post(
                    f"{settings.mimo_base_url.rstrip('/')}/chat/completions",
                    json=payload,
                    headers=headers,
                )
                resp.raise_for_status()
                return resp.json()["choices"][0]["message"]["content"]
            except Exception as e:  # noqa: BLE001 §8 降级不挑异常类型
                last_err = e
                logger.warning("MiMo ASR 调用失败（第 %d 次）：%s", attempt, e)
    raise RuntimeError(f"MiMo ASR 重试后仍失败: {last_err}")


async def process_session(session_id: str) -> None:
    """会话停止后异步执行整条流水线；任何失败只降级（分段标 failed），不抛出。"""
    try:
        await _run(session_id)
    except Exception:  # noqa: BLE001
        logger.exception("转写流水线异常 session=%s", session_id)
    finally:
        run_quality_check(session_id)


async def _run(session_id: str) -> None:
    store = AudioStore()
    with get_db() as conn:
        session = one(conn.execute("SELECT * FROM recording_session WHERE id = ?", (session_id,)))
    if not session:
        logger.warning("转写跳过：会话不存在 session=%s", session_id)
        return
    seqs = store.list_seqs(session_id)
    if not seqs:
        logger.info("转写跳过：会话无音频帧 session=%s", session_id)
        return

    for chunk in vad_segments(seqs):
        seq_start, seq_end = chunk[0], chunk[-1]
        merged_name = f"{seq_start:06d}-{seq_end:06d}.opus"
        merged_path = store.session_dir(session_id) / merged_name
        merged_path.write_bytes(
            b"".join(store.frame_path(session_id, s).read_bytes() for s in chunk)
        )
        wav_path = store.write_playable_wav(session_id, chunk, f"{seq_start:06d}-{seq_end:06d}.wav")
        if wav_path is not None:
            file_url = f"/audio/{session_id}/{wav_path.name}"
        else:
            file_url = f"/audio/{session_id}/{merged_name}"
        duration_ms = len(chunk) * FRAME_DURATION_MS

        with get_db() as conn:
            cur = conn.execute(
                "INSERT INTO audio_segment (session_id, seq_start, seq_end, file_url, duration_ms, status) "
                "VALUES (?, ?, ?, ?, ?, 'pending')",
                (session_id, seq_start, seq_end, file_url, duration_ms),
            )
            segment_id = cur.lastrowid

        if not settings.mimo_api_key:
            # §8：无 key 立即降级，不发起联网调用
            logger.warning("MIMO_API_KEY 为空，分段转写降级 session=%s seg=%d", session_id, segment_id)
            _mark_failed(segment_id)
            continue

        try:
            text = await _call_mimo_asr(merged_path.read_bytes())
        except Exception:  # noqa: BLE001 降级不抛
            _mark_failed(segment_id)
            continue

        with get_db() as conn:
            conn.execute(
                "INSERT INTO transcript (segment_id, content, asr_model) VALUES (?, ?, ?)",
                (segment_id, text, settings.mimo_asr_model),
            )
            conn.execute("UPDATE audio_segment SET status = 'transcribed' WHERE id = ?", (segment_id,))
        logger.info("分段转写完成 session=%s seg=%d", session_id, segment_id)


def _mark_failed(segment_id: int) -> None:
    with get_db() as conn:
        conn.execute("UPDATE audio_segment SET status = 'failed' WHERE id = ?", (segment_id,))


def run_quality_check(session_id: str) -> None:
    """质检 skill 扩展点（§4.1 后续接入）：本期占位，不做任何事。"""
    # TODO(P2): 质检 skill 接入后在此触发规则命中/评分/摘要报告
    return
