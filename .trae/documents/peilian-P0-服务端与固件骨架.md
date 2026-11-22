# peilian P0：服务端骨架 + 固件工程结构 落地计划

## 一、Summary

在 `/home/zcm/AIpassport/peilian/` 下落地 P0 阶段代码骨架，共两块：

1. **服务端骨架**（`peilian/server/`）：`.env.example`（MiMo 配置，用户自行填 key）、录音服务（WebSocket 接收音频帧 + 会话/补传管理 + 音频落盘）、转写流水线骨架（MiMo ASR 调用代码写好，key 填入即可跑）、后台 API 骨架（工牌/场景/角色配置、录音列表与转写详情，对齐设计文档 §5 接口契约）。
2. **固件工程结构**（`peilian/firmware/`）：拉取 xiaozhi-esp32 源码（建 `mvp` 分支）+ FoloToy ai-passport 板级参考，产出"模式状态机改动点映射"说明与代码挂接点 stub。

约束：`MVP设计文档.md` 等文档留在根目录不动，代码一律进 `peilian/`。后台本期只做 API 不做前端页面（前端属 P3）。

## 二、Current State（勘察结论）

- `/home/zcm/AIpassport/` 现状：仅 `MVP设计文档.md`（9 章 34 节）+ 空目录 `peilian/`，无任何代码、无 git 仓库、无 `.trae` 目录。
- 设计文档是本计划的唯一规格来源，关键章节：
  - §3.6 与小智固件的改动清单 → 固件工程映射依据
  - §4.1 录音服务与转写流水线 → 录音服务职责（接音频、去重存盘、管补传）
  - §5.2 设备侧接口契约（帧格式：`hello`/`ack`/`record.stop`）→ WS 协议实现依据
  - §5.3 后台与外部接口（I3/I5/I6/I7）→ 后台 API 实现依据
  - §6.1 实体关系（DEVICE/BADGE_PROFILE/RECORDING_SESSION/AUDIO_SEGMENT/TRANSCRIPT/SCENARIO/ROLE）→ 建表依据
- 用户已定决策：MiMo 密钥放 `.env` 由用户填写；日常对话用小智官方（本期不实现）；知识库与质检后置（流水线写到转写为止，留 hook）。

## 三、Proposed Changes

### 3.1 目标目录结构

```
/home/zcm/AIpassport/
├── MVP设计文档.md                  # 不动
└── peilian/
    ├── README.md                   # 项目结构 + 各模块启动方式（精简）
    ├── server/
    │   ├── .env.example            # MiMo/服务配置模板（用户填 .env）
    │   ├── requirements.txt        # fastapi, uvicorn[standard], pydantic-settings, httpx
    │   ├── schema.sql              # §6.1 的 7 张本期表（QUALITY_REPORT 不建）
    │   ├── scripts/
    │   │   ├── smoke.sh            # HTTP 接口冒烟（curl）
    │   │   └── ws_smoke.py         # WS 录音链路冒烟（模拟设备：hello+3 帧+stop，断言 ack）
    │   └── app/
    │       ├── __init__.py
    │       ├── main.py             # FastAPI 入口，挂载路由与 WS，启动时初始化 DB
    │       ├── config.py           # pydantic-settings 读 .env（MIMO_API_KEY/BASE_URL/三模型名/音色）
    │       ├── db.py               # sqlite3 连接 + schema.sql 初始化
    │       ├── models.py           # pydantic 模型，字段严格对齐设计文档 §5 契约
    │       ├── api/
    │       │   ├── __init__.py
    │       │   ├── auth.py         # 静态 admin token 依赖（MVP 单账号占位）
    │       │   ├── devices.py      # I1 GET badge、I3 PUT badge、I4 scenario.list（设备侧）
    │       │   ├── recordings.py   # I6-1 录音列表、I6-2 对话详情（segments+转写状态）
    │       │   └── admin.py        # I5 场景/角色 CRUD（含场景说明、对话时长字段）
    │       ├── ws/
    │       │   ├── __init__.py
    │       │   └── record.py       # /ws/record 端点：hello→ack、二进制帧去重落盘、record.stop
    │       ├── storage/
    │       │   ├── __init__.py
    │       │   └── audio.py        # audio/{session_id}/{seq:06d}.opus 环形目录 + 会话元数据
    │       └── transcribe/
    │           ├── __init__.py
    │           └── pipeline.py     # VAD 切段 stub + mimo-v2.5-asr 调用骨架（httpx，读 config）
    └── firmware/
        ├── README.md               # 构建步骤（ESP-IDF v6.x）+ 板级配置说明 + §3.6 改动点→源码文件映射
        ├── xiaozhi-esp32/          # git clone https://github.com/78/xiaozhi-esp32，建 mvp 分支
        └── reference/
            └── ai-passport/        # git clone https://github.com/FoloToy/ai-passport（板级参考，只读）
```

### 3.2 服务端实现要点

| 文件 | 做什么 / 为什么 |
|---|---|
| `.env.example` | `MIMO_API_KEY=`（留空由用户填）、`MIMO_BASE_URL=https://api.xiaomimimo.com/v1`、`MIMO_ASR_MODEL=mimo-v2.5-asr`、`MIMO_LLM_MODEL=mimo-v2.6-flash`、`MIMO_TTS_MODEL=mimo-v2.5-tts`、`MIMO_TTS_VOICE=`、`ADMIN_TOKEN=`、`AUDIO_DIR=audio`、`DB_PATH=peilian.db`。实际 `.env` 加入 `.gitignore` |
| `ws/record.py` | 按设计文档 §5.2 实现：设备先发 `{"type":"hello","device_id","session_id","sample_rate":16000,"codec":"opus","seq"}`，服务端应答 `{"type":"ack","acked_seq"}`；随后二进制 Opus 帧按 `seq` 去重落盘（幂等补传的核心，§6.2）；`{"type":"record.stop","session_id"}` 结束会话并触发转写流水线。本期用 `ws://`（局域网），协议层预留 wss |
| `transcribe/pipeline.py` | 异步触发：会话停止/每 60 秒 → VAD 切段（本期 stub：固定时长切段，接口留好）→ 逐段调 MiMo ASR（httpx + `api-key` 头，错误重试 1 次）→ 写 TRANSCRIPT 表。质检 hook 用一个空函数 `run_quality_check()` 占位 |
| `api/devices.py` | I1 `GET /api/v1/devices/{id}/badge`（设备开机拉工牌，无 photo 字段，头像由设备按姓名首字渲染）、I3 `PUT /api/v1/devices/{id}/badge`（对齐 §5.3 契约与错误码 401/404/422/429）、I4 场景列表（含场景说明与对话时长，供设备菜单） |
| `api/recordings.py` | I6-1 分页列表（筛选 device_id/date/keyword）、I6-2 详情 `{segments:[{seq_start,seq_end,file_url,duration_ms,text}],transcript_status}`，转写未完成返回 409 |
| `api/admin.py` | 角色 CRUD（名称/性格描述/人设 prompt/TTS 音色）、场景 CRUD（名称/场景说明/绑定角色/对话时长/难度），字段与 §4.3 一致 |
| `db.py` + `schema.sql` | 轻量 sqlite3，不用 ORM（MVP 七张表，避免过度设计 `[Expert judgment]`） |

### 3.3 固件工程结构要点

1. `git clone --depth 1 https://github.com/78/xiaozhi-esp32` 到 `peilian/firmware/xiaozhi-esp32/`，建 `mvp` 分支；`git clone --depth 1 https://github.com/FoloToy/ai-passport` 到 `peilian/firmware/reference/ai-passport/`（只作板级配置参考，不修改）。
2. clone 后勘察 xiaozhi-esp32 的 `main/` 源码结构（应用状态机、音频管线、网络/OTA 配置位置），把设计文档 §3.6 改动清单逐项映射到具体源码文件，写入 `firmware/README.md` 的"改动点映射"表（如：模式状态机 → main/ 下应用主循环；多服务端连接配置 → OTA/websocket 配置读取处；场景菜单 → LVGL UI 目录）。
3. 代码挂接点 stub：在 mvp 分支上创建 `main/mvp/` 目录，放 `mode_manager.h/.c`（四模式枚举 + 状态切换接口声明 + TODO 注释指向映射表），本期**不接线不编译进固件**，避免在未深入读源码前破坏基线构建。
4. `firmware/README.md` 同时记录构建方式（ESP-IDF v6.x、目标 esp32c3、esptool 备份提醒、配网页面 OTA URL 指向自建服务器的方法）。

### 3.4 根目录约定

- `MVP设计文档.md` 留根目录，不移动、不修改；
- `.gitignore` 与 git 仓库本次不初始化（用户未要求；如需版本管理由用户决定后补）。

## 四、Assumptions & Decisions

- **技术栈**：Python 3.10+ / FastAPI / uvicorn / sqlite3（stdlib）/ httpx；与设计文档 §2.3 选型一致，不引入 ORM、不引入 Celery（转写用 FastAPI BackgroundTasks 起步）。
- **范围裁定**（用户已确认）："服务端 + 固件工程结构"档；后台前端、陪练服务（xiaozhi-esp32-server 部署）、知识库、质检均为后续阶段。
- **协议实现深度**：WS 录音协议完整实现（这是工牌链路核心）；小智对话协议不在本期（P1 由 xiaozhi-esp32-server 承担）。
- **转写流水线**：MiMo 调用代码写全，但无 key 时不实测；VAD 本期 stub（固定切段），真实 VAD 后续替换。
- **固件 stub 不接线**：xiaozhi-esp32 基线构建保持可用是硬约束，mvp 代码本期只建目录与接口骨架。
- **网络风险**：clone GitHub 若沙箱网络受限，改用 `dangerouslyDisableSandbox` 或由用户手动下载 zip 解压到目标路径（计划内两种路径都预留）。
- **文档最小化**：仅 `peilian/README.md` 与 `peilian/firmware/README.md` 两份必要说明（结构/启动/构建），不另建其他文档。

## 五、Verification

1. **服务端启动**：`cd peilian/server && cp .env.example .env && pip install -r requirements.txt && uvicorn app.main:app --port 8000` 正常启动，`GET /health` 返回 200，启动日志无报错（.env 空 key 不阻塞启动）。
2. **HTTP 冒烟**（`scripts/smoke.sh`）：PUT badge（含校验 422 路径）→ GET badge 回读一致；场景/角色 CRUD 各一轮；GET /api/v1/recordings 返回空列表 200。
3. **WS 冒烟**（`scripts/ws_smoke.py`）：模拟设备发 hello + 3 个带 seq 的二进制帧（含 1 个重复 seq 模拟补传）+ record.stop，断言收到 ack 且音频目录落盘 3 个不重复分片、DB 写入会话记录。
4. **转写骨架**：用假 key 触发 pipeline，断言走"重试 1 次后标记分段 failed"的降级路径（§8 行为），不抛未捕获异常。
5. **固件工程**：clone 成功、`mvp` 分支存在、`README.md` 映射表覆盖 §3.6 全部 8 行改动清单；（可选，后台执行）`idf.py set-target esp32c3 && idf.py build` 基线编译通过。

## 六、实施顺序

1. 建 `peilian/server/` 骨架（config/db/models）→ 2. WS 录音服务 + 存储 → 3. 后台 API 三组路由 → 4. 转写 pipeline 骨架 + 两个冒烟脚本 → 5. clone 固件仓库 + 改动点映射 + mvp stub → 6. 跑通全部验证项。
