"""服务端配置：从 .env 读取（设计文档 §7.3 密钥只存服务端配置）。"""
from pathlib import Path

from pydantic_settings import BaseSettings, SettingsConfigDict

# server/ 根目录，.env 与 schema.sql 都在这里
SERVER_ROOT = Path(__file__).resolve().parent.parent


class Settings(BaseSettings):
    # MiMo（I7）
    mimo_api_key: str = ""
    mimo_base_url: str = "https://api.xiaomimimo.com/v1"
    mimo_asr_model: str = "mimo-v2.5-asr"
    mimo_llm_model: str = "mimo-v2.6-flash"
    mimo_tts_model: str = "mimo-v2.5-tts"
    mimo_tts_voice: str = ""

    # 服务端
    admin_token: str = "admin-dev-token"
    # 管理端登录账号（MVP 单账号，POST /api/v1/admin/login）
    admin_username: str = "admin123"
    admin_password: str = "qwe@123456"
    device_token: str = "device-dev-token"
    audio_dir: str = "audio"
    db_path: str = "peilian.db"
    # 陪练对话引擎 mock 开关（1=不联网假模型，冒烟/无网联调用）
    practice_mock_models: bool = False

    model_config = SettingsConfigDict(
        env_file=SERVER_ROOT / ".env",
        env_file_encoding="utf-8",
        extra="ignore",
    )

    def audio_root(self) -> Path:
        p = Path(self.audio_dir)
        return p if p.is_absolute() else SERVER_ROOT / p

    def db_file(self) -> Path:
        p = Path(self.db_path)
        return p if p.is_absolute() else SERVER_ROOT / p


settings = Settings()
