"""音频帧落盘：按会话组织目录，按 seq 幂等去重（设计文档 §6.2）。

目录布局：{AUDIO_DIR}/{session_id}/{seq:06d}.opus
补传冲突（§8）：同 seq 内容不一致时以设备重传为准覆盖，返回 conflict=True 供调用方记警告日志。
"""
import logging
import wave
from pathlib import Path

from ..config import settings
from ..practice.audio import CHANNELS, SAMPLE_RATE, decode_frame, make_decoder

logger = logging.getLogger(__name__)


class AudioStore:
    def __init__(self, base_dir: Path | None = None):
        self.base_dir = Path(base_dir) if base_dir else settings.audio_root()
        self.base_dir.mkdir(parents=True, exist_ok=True)

    def session_dir(self, session_id: str) -> Path:
        d = self.base_dir / session_id
        d.mkdir(parents=True, exist_ok=True)
        return d

    def frame_path(self, session_id: str, seq: int) -> Path:
        return self.session_dir(session_id) / f"{seq:06d}.opus"

    def write_frame(self, session_id: str, seq: int, payload: bytes) -> tuple[bool, bool]:
        """写入一帧。返回 (是否新帧, 是否发生补传冲突)。"""
        path = self.frame_path(session_id, seq)
        if path.exists():
            if path.read_bytes() == payload:
                return False, False  # 重复补传，内容一致，直接去重
            # §8：同 seq 内容不一致 → 以设备重传为准覆盖
            logger.warning("补传冲突 session=%s seq=%d，以重传内容覆盖", session_id, seq)
            path.write_bytes(payload)
            return False, True
        path.write_bytes(payload)
        return True, False

    def list_seqs(self, session_id: str) -> list[int]:
        d = self.base_dir / session_id
        if not d.exists():
            return []
        # 只取纯数字 seq 分片，跳过流水线生成的合并文件（{seq_start}-{seq_end}.opus）
        return sorted(int(p.stem) for p in d.glob("*.opus") if p.stem.isdigit())

    def contiguous_acked(self, session_id: str) -> int:
        """已确认的最大连续 seq（§6.2 补传唯一事实来源），无帧时返回 -1。"""
        acked = -1
        for seq in self.list_seqs(session_id):
            if seq == acked + 1:
                acked = seq
            elif seq > acked + 1:
                break
        return acked

    def write_playable_wav(self, session_id: str, seqs: list[int], out_name: str) -> Path | None:
        """把分段的裸 Opus 帧解码为 WAV（浏览器可直接播放）；无法解码返回 None。"""
        decoder = make_decoder()
        if decoder is None:
            return None
        pcm = bytearray()
        decoded = 0
        for seq in seqs:
            path = self.frame_path(session_id, seq)
            if not path.exists():
                continue
            out = decode_frame(decoder, path.read_bytes())
            if out:
                pcm += out
                decoded += 1
        if not decoded:
            return None
        out_path = self.session_dir(session_id) / out_name
        with wave.open(str(out_path), "wb") as w:
            w.setnchannels(CHANNELS)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(bytes(pcm))
        return out_path

    def total_bytes(self, session_id: str) -> int:
        d = self.base_dir / session_id
        return sum(p.stat().st_size for p in d.glob("*.opus")) if d.exists() else 0
