"""Opus/PCM 音频辅助（16kHz/16bit/mono，60ms 帧）。

opuslib 需要系统 libopus；缺失或编解码失败时返回 None，管线自动降级为文本模式
（《智能陪练详细设计.md》§2.4 TTS 失败降级）。
"""
import logging

logger = logging.getLogger(__name__)

SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_MS = 60
SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_MS // 1000  # 960
FRAME_PCM_BYTES = SAMPLES_PER_FRAME * 2  # int16

# 静音判定阈值（int16 PCM 的 RMS 能量）
SILENCE_RMS = 500


def _opuslib():
    try:
        import opuslib  # noqa: PLC0415 延迟导入：缺 libopus 时仅降级不崩服务

        return opuslib
    except Exception as e:  # noqa: BLE001
        logger.warning("opuslib 不可用，音频编解码降级为文本模式: %s", e)
        return None


def make_decoder():
    lib = _opuslib()
    if lib is None:
        return None
    try:
        return lib.Decoder(SAMPLE_RATE, CHANNELS)
    except Exception as e:  # noqa: BLE001
        logger.warning("Opus 解码器创建失败: %s", e)
        return None


def make_encoder():
    lib = _opuslib()
    if lib is None:
        return None
    try:
        return lib.Encoder(SAMPLE_RATE, CHANNELS, lib.APPLICATION_AUDIO)
    except Exception as e:  # noqa: BLE001
        logger.warning("Opus 编码器创建失败: %s", e)
        return None


def decode_frame(decoder, data: bytes) -> bytes | None:
    """Opus 帧 -> PCM16；失败返回 None（该帧按“有效语音”处理，不影响会话）。"""
    if decoder is None:
        return None
    try:
        return decoder.decode(data, SAMPLES_PER_FRAME)
    except Exception:  # noqa: BLE001 烟测假帧等场景常态失败，不刷屏
        return None


def encode_pcm(encoder, pcm: bytes) -> list[bytes]:
    """PCM16 -> Opus 帧列表（60ms/帧）；编码器缺失返回空列表。"""
    if encoder is None:
        return []
    frames = []
    for off in range(0, len(pcm) - FRAME_PCM_BYTES + 1, FRAME_PCM_BYTES):
        try:
            frames.append(encoder.encode(pcm[off : off + FRAME_PCM_BYTES], SAMPLES_PER_FRAME))
        except Exception as e:  # noqa: BLE001
            logger.warning("Opus 编码失败: %s", e)
            return frames
    return frames


def is_silence(pcm: bytes | None) -> bool:
    """PCM 帧能量是否为静音；无法判断（None）视为有效语音。"""
    if not pcm:
        return False
    samples = memoryview(pcm).cast("h")
    if not samples:
        return True
    acc = 0
    for s in samples:
        acc += s * s
    rms = (acc / len(samples)) ** 0.5
    return rms < SILENCE_RMS
