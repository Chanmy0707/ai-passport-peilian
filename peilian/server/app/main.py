"""FastAPI 入口：挂载路由与 WS，启动时初始化 DB 与音频目录。

启动：cd peilian/server && uvicorn app.main:app --port 8000
"""
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from .api import admin, auth, devices, recordings, tts
from .config import settings
from .db import init_db
from .practice import ws as practice_ws
from .ws import record as record_ws


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    settings.audio_root().mkdir(parents=True, exist_ok=True)
    yield


app = FastAPI(title="peilian server", lifespan=lifespan)

# 后台 API（I3/I4/I5/I6）
app.include_router(auth.router, prefix="/api/v1")
app.include_router(devices.router, prefix="/api/v1")
app.include_router(recordings.router, prefix="/api/v1")
app.include_router(admin.router, prefix="/api/v1")
app.include_router(tts.router, prefix="/api/v1")
# 录音流协议（I1，WS /record/v1/）
app.include_router(record_ws.router)
# 陪练对话协议（小智协议子集，WS /xiaozhi/v1/，详见《智能陪练详细设计.md》§2）
app.include_router(practice_ws.router)
# 录音分片静态下载（file_url 指向 /audio/...）
settings.audio_root().mkdir(parents=True, exist_ok=True)
app.mount("/audio", StaticFiles(directory=settings.audio_root()), name="audio")


@app.get("/health")
def health():
    return {"status": "ok"}


# 管理端静态页面（§4.3），挂根路径，index.html 即管理端单页（须在所有路由之后）
STATIC_ROOT = Path(__file__).resolve().parent.parent / "static"
app.mount("/", StaticFiles(directory=STATIC_ROOT, html=True), name="admin")
