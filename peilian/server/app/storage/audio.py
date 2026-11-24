"""音频帧落盘：按会话组织目录，按 seq 幂等去重（设计文档 §6.2）。

目录布局：{AUDIO_DIR}/{session_id}/{seq:06d}.opus
补传冲突（§8）：同 seq 内容不一致时以设备重传为准覆盖，返回 conflict=True 供调用方记警告日志。
WAV 封装（§4.1）：裸 Opus 帧 → WAV 的解码链为 opuslib（首选）→ ffmpeg（兜底），
均不可用时打 ERROR 日志并返回 None（该分段不可播放），杜绝静默回落。
"""
import logging
import shutil
import struct
import subprocess
import wave
from pathlib import Path

from ..config import settings
from ..practice.audio import CHANNELS, SAMPLE_RATE, decode_frame, make_decoder

logger = logging.getLogger(__name__)

_OGG_CRC_POLY = 0x04C11DB7
_OGG_SAMPLES_PER_FRAME_48K = 48 * 60  # 60ms 帧在 Opus 粒度时钟（48kHz）下的采样数


def _ogg_crc(data: bytes) -> int:
    """Ogg 页 CRC32（多项式 0x04C11DB7，不反转、不异或输出）。"""
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ _OGG_CRC_POLY) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def _ogg_page(serial: int, seqno: int, granule: int, flags: int, packets: list[bytes]) -> bytes:
    lacing: list[int] = []
    for p in packets:
        n = len(p)
        lacing.extend([255] * (n // 255))
        lacing.append(n % 255)
    page = (
        b"OggS" + bytes([0, flags]) + struct.pack("<qii", granule, serial, seqno) + b"\x00\x00\x00\x00"
        + bytes([len(lacing)]) + bytes(lacing) + b"".join(packets)
    )
    return page[:22] + struct.pack("<I", _ogg_crc(page)) + page[26:]


def _ogg_opus_stream(frames: list[bytes]) -> bytes:
    """把裸 Opus 帧序列封装为 Ogg Opus 流（ffmpeg 兜底转码的输入）。"""
    serial = 1
    vendor = b"peilian-server"
    head = b"OpusHead" + struct.pack("<BBHIh", 1, CHANNELS, 0, SAMPLE_RATE, 0) + bytes([0])
    tags = b"OpusTags" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 0)
    out = [_ogg_page(serial, 0, 0, 0x02, [head]), _ogg_page(serial, 1, 0, 0, [tags])]

    packets_done = 0
    seqno = 2
    batch: list[bytes] = []
    for frame in frames:
        batch.append(frame)
        packets_done += 1
        if len(batch) >= 50:
            granule = packets_done * _OGG_SAMPLES_PER_FRAME_48K
            out.append(_ogg_page(serial, seqno, granule, 0, batch))
            seqno += 1
            batch = []
    if batch:
        granule = packets_done * _OGG_SAMPLES_PER_FRAME_48K
        out.append(_ogg_page(serial, seqno, granule, 0x04, batch))  # EOS
    elif out:
        # 尾页打 EOS 标记
        last = bytearray(out[-1])
        last[5] |= 0x04
        page = bytes(last)
        out[-1] = page[:22] + struct.pack("<I", _ogg_crc(page)) + page[26:]
    return b"".join(out)


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

    def _write_wav_via_ffmpeg(self, session_id: str, seqs: list[int], out_name: str) -> Path | None:
        """兜底转码：把裸 Opus 帧封成 Ogg Opus 后交给 ffmpeg 转 WAV；系统无 ffmpeg 或转码失败返回 None。"""
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            return None
        frames = []
        for seq in seqs:
            path = self.frame_path(session_id, seq)
            if path.exists():
                frames.append(path.read_bytes())
        if not frames:
            return None
        sdir = self.session_dir(session_id)
        tmp_ogg = sdir / f"{out_name}.tmp.ogg"
        out_path = sdir / out_name
        tmp_ogg.write_bytes(_ogg_opus_stream(frames))
        try:
            res = subprocess.run(
                [
                    ffmpeg, "-y", "-loglevel", "error", "-i", str(tmp_ogg),
                    "-ar", str(SAMPLE_RATE), "-ac", str(CHANNELS), "-c:a", "pcm_s16le", str(out_path),
                ],
                capture_output=True,
                timeout=120,
            )
            if res.returncode != 0 or not out_path.exists():
                logger.error(
                    "ffmpeg 兜底转码失败 session=%s: %s",
                    session_id,
                    res.stderr.decode(errors="replace")[:300],
                )
                return None
            return out_path
        except Exception as e:  # noqa: BLE001
            logger.error("ffmpeg 兜底转码异常 session=%s: %s", session_id, e)
            return None
        finally:
            tmp_ogg.unlink(missing_ok=True)

    def write_playable_wav(self, session_id: str, seqs: list[int], out_name: str) -> Path | None:
        """把分段的裸 Opus 帧解码为 WAV（浏览器可直接播放）；无法解码返回 None（§4.1 解码链）。

        首选 opuslib 解码；opuslib 缺失或全部解码失败时尝试 ffmpeg 兜底；
        两者均不可用打 ERROR 日志（提示安装依赖），该分段音频不可在线播放。
        """
        pcm = bytearray()
        decoded = 0
        decoder = make_decoder()
        if decoder is not None:
            for seq in seqs:
                path = self.frame_path(session_id, seq)
                if not path.exists():
                    continue
                out = decode_frame(decoder, path.read_bytes())
                if out:
                    pcm += out
                    decoded += 1
        if decoded:
            out_path = self.session_dir(session_id) / out_name
            with wave.open(str(out_path), "wb") as w:
                w.setnchannels(CHANNELS)
                w.setsampwidth(2)
                w.setframerate(SAMPLE_RATE)
                w.writeframes(bytes(pcm))
            return out_path

        out_path = self._write_wav_via_ffmpeg(session_id, seqs, out_name)
        if out_path:
            logger.info("WAV 已经 ffmpeg 兜底转码 session=%s", session_id)
            return out_path
        logger.error(
            "WAV 转码失败 session=%s，音频将无法在线播放：opuslib %s。"
            "请安装 opuslib（pip install opuslib，需系统 libopus）或安装 ffmpeg 后重启服务",
            session_id,
            "不可用" if decoder is None else "解码失败",
        )
        return None

    def total_bytes(self, session_id: str) -> int:
        d = self.base_dir / session_id
        return sum(p.stat().st_size for p in d.glob("*.opus")) if d.exists() else 0
