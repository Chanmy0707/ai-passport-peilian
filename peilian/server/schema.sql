-- peilian 服务端数据模型：设计文档 §6.1 的本期 7 张表
-- （QUALITY_REPORT / KNOWLEDGE_* 后置，不在本期建表；PRACTICE_* 已建，见下方 practice_* 表）

CREATE TABLE IF NOT EXISTS device (
    device_id   TEXT PRIMARY KEY,          -- MAC/预分配ID
    name        TEXT,
    default_mode TEXT,
    last_seen   TEXT,
    fw_version  TEXT
);

CREATE TABLE IF NOT EXISTS badge_profile (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id    TEXT NOT NULL UNIQUE,
    name         TEXT NOT NULL,
    title        TEXT,
    employee_no  TEXT,
    qr_text      TEXT,
    avatar_id    TEXT,          -- 动态 emoji 素材 id，设备云端按需下载 /emojis/64px/<id>.gif 并缓存；NULL 回落姓名首字占位（§5.2）
    updated_at   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS recording_session (
    id          TEXT PRIMARY KEY,
    device_id   TEXT NOT NULL,
    started_at  TEXT NOT NULL,
    ended_at    TEXT,
    status      TEXT NOT NULL DEFAULT 'uploading',  -- uploading/done/failed
    audio_bytes INTEGER NOT NULL DEFAULT 0,
    acked_seq   INTEGER NOT NULL DEFAULT -1         -- 补传唯一事实来源（§6.2），-1 表示未确认
);

CREATE TABLE IF NOT EXISTS audio_segment (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id  TEXT NOT NULL REFERENCES recording_session(id),
    seq_start   INTEGER NOT NULL,
    seq_end     INTEGER NOT NULL,
    file_url    TEXT NOT NULL,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    -- 以下为 §8 ASR 降级所需（ER 图未列，标记分段转写状态）
    status      TEXT NOT NULL DEFAULT 'pending'     -- pending/transcribed/failed
);

CREATE TABLE IF NOT EXISTS transcript (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    segment_id INTEGER NOT NULL UNIQUE REFERENCES audio_segment(id),
    content    TEXT NOT NULL,
    asr_model  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS role_profile (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    name          TEXT NOT NULL,
    personality   TEXT,
    system_prompt TEXT,
    tts_voice     TEXT
);

-- 陪练练习会话（§6.1 PRACTICE_SESSION，详见《智能陪练详细设计.md》§4）
CREATE TABLE IF NOT EXISTS practice_session (
    id            TEXT PRIMARY KEY,
    device_id     TEXT NOT NULL,
    scenario_id   TEXT NOT NULL,
    started_at    TEXT NOT NULL,
    ended_at      TEXT,
    overall_score REAL,                            -- user 轮 score 均分
    status        TEXT NOT NULL DEFAULT 'running'  -- running/done/failed
);

-- 陪练逐轮记录（§6.1 PRACTICE_TURN；score/feedback 仅 user 轮）
CREATE TABLE IF NOT EXISTS practice_turn (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES practice_session(id),
    role       TEXT NOT NULL,
    content    TEXT NOT NULL,
    score      REAL,
    feedback   TEXT,
    ts         TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS scenario (
    id              TEXT PRIMARY KEY,               -- §6.1 定义为 string PK
    name            TEXT NOT NULL,
    description     TEXT,
    role_id         INTEGER REFERENCES role_profile(id),
    kb_id           INTEGER,                        -- 知识库绑定后置，仅预留
    duration_minutes INTEGER,                       -- 场景对话时长（分钟），NULL 表示不限
    difficulty      TEXT
);
