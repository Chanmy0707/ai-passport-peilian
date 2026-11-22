"""请求/响应模型，字段严格对齐设计文档 §5 接口契约。

- I3 工牌：PUT {name 必填, title, employee_no, qr_text, avatar_id 可选}，成功返回完整 badge 对象（含 updated_at）
- I5-1 场景：{name, description, role_id, kb_id, difficulty} + duration_minutes（对话时长，R3/§4.3）
- I5-2 角色：{name, personality, system_prompt, tts_voice}
- I5/I6 响应统一 {data, error} 包裹
"""
from pydantic import BaseModel, Field


# ---------- I3 工牌 ----------

class BadgeUpdate(BaseModel):
    name: str
    title: str | None = None
    employee_no: str | None = None
    qr_text: str | None = None
    avatar_id: str | None = None


class BadgeData(BaseModel):
    name: str
    title: str | None = None
    employee_no: str | None = None
    qr_text: str | None = None
    avatar_id: str | None = None
    updated_at: str | None = None


# ---------- I5-1 场景 ----------

class ScenarioCreate(BaseModel):
    name: str
    description: str | None = None
    role_id: int | None = None
    kb_id: int | None = None  # 知识库后置，仅预留字段
    duration_minutes: int | None = Field(default=None, ge=0)  # NULL 表示不限（日常对话）
    difficulty: str | None = None
    scenario_id: str | None = None  # 不传则自动生成 uuid


class ScenarioUpdate(BaseModel):
    name: str | None = None
    description: str | None = None
    role_id: int | None = None
    kb_id: int | None = None
    duration_minutes: int | None = Field(default=None, ge=0)
    difficulty: str | None = None


# ---------- I5-2 角色 ----------

class RoleCreate(BaseModel):
    name: str
    personality: str | None = None
    system_prompt: str | None = None
    tts_voice: str | None = None


class RoleUpdate(BaseModel):
    name: str | None = None
    personality: str | None = None
    system_prompt: str | None = None
    tts_voice: str | None = None


# ---------- 管理端登录 ----------

class AdminLogin(BaseModel):
    username: str = Field(min_length=1)
    password: str = Field(min_length=1)


# ---------- 统一响应包裹（I5/I6）----------

def ok(data):
    return {"data": data, "error": None}


def fail(code: str, message: str):
    return {"data": None, "error": {"code": code, "message": message}}
