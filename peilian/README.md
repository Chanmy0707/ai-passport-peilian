# peilian/ — AI Passport 双模式语音设备 MVP 代码

智能工牌 + 智能陪练 + 日常对话三模式语音设备（FoloToy AI Passport / ESP32-C3）。设计规格见仓库根目录 [MVP设计文档.md](../MVP设计文档.md)。

## 结构

| 目录 | 说明 |
|---|---|
| `server/` | 自建服务端：录音接收与转写流水线、配置后台 API（I3–I6）、录音流协议（I1）、智能陪练 WS 服务（场景/对话/评分，见 [智能陪练详细设计.md](../智能陪练详细设计.md)） |
| `firmware/` | 固件工程：xiaozhi-esp32 基座（mvp 分支）+ 硬件参考仓库，见 [firmware/README.md](firmware/README.md) |

## 服务端启动

```sh
cd server
cp .env.example .env        # 填 MIMO_API_KEY 等参数（用户自填）
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn app.main:app --port 8000
curl http://127.0.0.1:8000/health
```

主要端点：`GET/PUT /api/v1/devices/{id}/badge`（I3，工牌配置）、`GET /api/v1/devices/{id}/scenarios`（I4，场景列表）、`/api/v1/scenarios` + `/api/v1/roles`（I5，后台 CRUD，`X-Admin-Token`）、`/api/v1/recordings`（I6，录音查询）、`WS /record/v1/`（I1，设备录音流）、`/audio/`（录音分片静态下载）。

## 冒烟验证

```sh
cd server
bash scripts/smoke.sh       # 起服务端 → HTTP 接口冒烟 → WS 录音链路冒烟 → 停服务
```

覆盖：badge 校验 422 与回读、场景/角色 CRUD、录音列表空态、WS hello/补传去重/record.ack/record.stop、转写降级（假 key → 重试 1 次 → 分段标 failed）。使用独立 `smoke.db` / `smoke-audio` 数据目录，可重复执行。
