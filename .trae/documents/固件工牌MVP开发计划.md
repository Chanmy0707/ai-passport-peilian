# 固件工牌模式 MVP（P1）开发计划

## Summary

在 xiaozhi-esp32 二开基座（`mvp` 分支）上实现工牌模式全量功能：record 协议客户端录音上传（wss → aipassport.dlzr.cn）、工牌配置拉取、Flash 环形缓冲断网补传、模式状态机接线、主菜单/工牌页 LVGL UI。完成后真机刷机实测全链路。

**范围（用户已确认）**：工牌 MVP 全量。**不含**：陪练模式 MCP `set_scenario`、场景选择页、评分页（P2）；日常对话模式保持基座现状不动。

## Current State Analysis

### 服务端（已完成，作为协议事实来源）

- **WS `/record/v1/`**（[record.py](file:///home/zcm/AIpassport/peilian/server/app/ws/record.py)，L82 挂载）：
  - **hello**（首条消息，文本 JSON）：`{"type":"hello","device_id","token","resume_from_seq"}`；`device_id` 非空、`token` == DEVICE_TOKEN（预共享），否则回 `{error:{code:"UNAUTHORIZED"/"BAD_REQUEST"}}` 并 close(1008)；`resume_from_seq` 为 int，0=新会话
  - **hello_ack**：`{"type":"hello_ack","session_id","acked_seq","server_time"}`，`acked_seq` 初始 -1
  - **音频帧**：WebSocket **二进制帧**（非 base64）：8 字节头 `struct "<II"`（seq u32 + ts u32 小端）+ **Opus 裸载荷**（L35、L168-169）
  - **record.ack**：`{"type":"record.ack","acked_seq"}`，每 50 帧或 1 秒；`acked_seq` = 最大**连续**已确认 seq
  - **badge.get**：`{"type":"badge.get","device_id"}` → **badge.data**：`{"type":"badge.data","name","title","employee_no","qr_text","avatar_id","updated_at"}`
  - **record.stop**：`{"type":"record.stop","session_id"}` → **record.stopped**；**无 record.start/segment**——hello 即开始，二进制帧持续发送
  - 补传冲突：同 seq 不一致以重传覆盖；错误统一 `{error:{code,message}}` 信封
- **HTTP**（[devices.py](file:///home/zcm/AIpassport/peilian/server/app/api/devices.py)）：`GET /api/v1/devices/{id}/badge`（裸对象，404=未配置）、`GET /api/v1/devices/{id}/scenarios`（`[{scenario_id,name,description,difficulty,duration_minutes}]`）；鉴权头 **`X-Device-Token`**，IP 限流 120/min
- 域名链路已通：`wss://aipassport.dlzr.cn/record/v1/` 实测握手成功（frpc 隧道 + 宝塔 nginx + SSL）

### 固件（起点）

- [mode_manager.{h,c}](file:///home/zcm/AIpassport/peilian/firmware/xiaozhi-esp32/main/mvp/mode_manager.h)：`MvpMode` 枚举（MODE_BADGE/MODE_PRACTICE/MODE_CHAT/MODE_SETTINGS）+ 接口 stub，已入 CMakeLists SOURCES，未接线
- [websocket_protocol.cc](file:///home/zcm/AIpassport/peilian/firmware/xiaozhi-esp32/main/protocols/websocket_protocol.cc) L79-106：WS 抽象可复用（`network->CreateWebSocket(1)`、SendText/SendAudio、OnData JSON 解析）；NVS `Settings("websocket")` 读 `url`/`token`/`version`，`token` 加 `Bearer ` 走 Authorization 头；`Device-Id` = MAC —— **record 协议的 hello 契约不同（token 在消息体内），需独立客户端**
- Opus 编码在 `AudioService` 内部（`opus_encoder_`，esp-opus）：**16 kHz 单声道、60 ms 帧**（`AS_OPUS_ENC_CONFIG()`，audio_service.h L67-80）；板级 codec 24 kHz 经 `input_resampler_` 重采样到 16 kHz（audio_service.cc L73-75），**无需改采样率**
- 板级（folotoy/ai-passport）：ESP32-C3、8MB Flash、ST7789 240x320 竖屏 LCD + LVGL、UP/DOWN/OK 三键 ADC 阶梯（GPIO0）、ES8311
- 构建：`main/CMakeLists.txt` 显式 SOURCES 列表；`main/Kconfig.projbuild` 有 `OTA_URL` 配置模式可参照
- Git：`mvp` 分支，工作区已有未提交变更（`M main/CMakeLists.txt`、`?? main/mvp/`）
- 环境：ESP-IDF v6.1 在 `~/esp/esp-idf`（有历史构建日志），真机可刷机实测

## Proposed Changes

全部新增代码放 `main/mvp/`（遵循 firmware/README.md 约定），接线遵循基座 `AGENTS.md`：状态切换只经 `Application::SetDeviceState()`，回调里用 `Application::Schedule()` 调度，不阻塞主事件循环与音频任务。

### 1. 配置项：`main/mvp/Kconfig` + 读取模块 `mvp_config.{h,cc}`

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `CONFIG_MVP_RECORD_WS_URL` | `wss://aipassport.dlzr.cn/record/v1/` | 录音上传 WS 地址 |
| `CONFIG_MVP_BADGE_API_URL` | `https://aipassport.dlzr.cn/api/v1` | 配置拉取 HTTP 前缀 |
| `CONFIG_MVP_DEVICE_TOKEN` | `device-dev-token` | 预共享设备令牌（生产烧录前替换） |
| `CONFIG_MVP_DEVICE_ID` | 空 = 用 MAC | 设备 ID 策略 |

- `mvp_config.cc`：读取顺序 **NVS `Settings("mvp")` 同名键 > Kconfig 默认**（与 ota.cc L59-69 的 `ota_url` 模式一致），生产/局域网调试改 NVS 即可，不重编
- device_id 默认取 MAC（与 websocket_protocol.cc 的 `Device-Id` 一致）

### 2. record 协议客户端：`mvp/record_client.{h,cc}`

参照 websocket_protocol.cc 的 WS 抽象形态，独立实现 I1 契约：

- `Connect(device_id)`：建 WS → 发 hello（`type/device_id/token/resume_from_seq`）→ 等 hello_ack（记录 `session_id`、`acked_seq`）；鉴权失败/断线指数退避重连
- `SendFrame(seq, ts_ms, opus_payload)`：组二进制帧（`struct "<II"` LE 头 + Opus 载荷）经 WS 二进制发送；发送即入 flash_ring 标"未确认"，收到 record.ack 更新已确认水位并从 ring 释放
- `ResumeFromSeq()`：返回 `acked_seq+1`，重连 hello 带 `resume_from_seq`，ring 中未确认帧按 seq 顺序补发（服务端按 seq 去重覆盖）
- `RequestBadge()`：发 badge.get，OnData 收 badge.data 回调给 UI/缓存
- `StopSession()`：发 record.stop，收 record.stopped 后关会话
- OnData 统一解析 `{type, ...}` 与 `{error:{code,message}}` 信封

### 3. Flash 环形缓冲：`mvp/flash_ring.{h,c}`

- **分区**：查 `partitions/` 现有分区表与 8MB 余量，新增 `mvp_ring` 数据分区（约 1.5–2 MB），`esp_partition` API 读写
- **布局**：定长记录块（8 字节帧头 + ≤60ms Opus 载荷 + 标志位），顺序写环形；块头含 seq，启动时扫描重建索引
- **接口**：`FlashRing_Put(seq, ts, payload)`（在线时仅作发送队列备份）、`FlashRing_PeekUnacked(from_seq)` / `DropThrough(seq)`（ack 后释放）、写满覆盖最旧已确认块
- **补传**：重连后 record_client 从 `acked_seq+1` 起读 ring 补发，全部确认后清空

### 4. 模式状态机接线：`mode_manager.c` 完成 + `application.cc`/按键挂接

- `ModeManager_Init` 在应用启动调用；UP/DOWN 导航菜单、OK 确认进入模式（三键 ADC 事件挂到既有按键处理链）
- **工牌模式**（设计文档 §：操作流）：OK 开始录音 → 屏幕录音中（红点 + 计时）→ 再按 OK 停止（发 record.stop）；录音期间 `AudioService` 采集 + Opus 60ms 帧 → record_client 上传，断网自动落 flash_ring
- **模式互斥**（§3.1）：工牌/陪练与日常对话互斥占用音频采集——进入工牌模式暂停小智对话协议，退回 MODE_CHAT 时恢复；`Application::SetDeviceState()` 走状态机
- MODE_SETTINGS：显示/编辑 NVS 连接配置（MVP 最小：显示当前地址与 device_id，长按 OK 重置 NVS）
- 启动默认进主菜单（首次）；已选模式可记忆（NVS）

### 5. UI（LVGL，`main/display/` 内新增或 `mvp/` 内 UI 模块 + display 注册）

设计规范（§ UI 规范）：240x320 竖屏、深色风格（黑底白字、绿色强调）、主文本 ≥24px、字号档位 ≤2、底部固定按键提示条、一屏一任务。

- **主菜单页**：四模式列表（工牌/陪练/日常对话/设置），UP/DOWN 高亮（绿）、OK 进入；陪练项置灰标"敬请期待"
- **工牌页**：上 2/3 工牌卡片（badge.data：姓名/职位/工号/二维码文本；头像用预置 emoji 素材循环或姓名首字占位）；下 1/3 操作区："OK 开始录音" ↔ 录音中"● 录音中 03:25"（合规：录音全程可见提示）
- badge.data 启动/进入工牌模式时经 WS badge.get 拉取，NVS 缓存兜底（离线显示上次数据）
- 不动小智既有对话页渲染

### 6. 收尾

- 更新 [firmware/README.md](file:///home/zcm/AIpassport/peilian/firmware/README.md) §3.6 映射表状态
- 服务端无需改动（协议已实现并通过 smoke）

## Assumptions & Decisions

1. **音频格式**：复用 `AudioService` 现有 Opus 编码（16kHz/单声道/60ms 帧），帧内时间戳用相对会话毫秒；24kHz→16kHz 重采样已存在，不改板级采样率
2. **device_id** = 设备 MAC（十六进制串），与基座 Device-Id 一致；hello 的 token 放**消息体**（不是 Authorization 头），与小智协议不同
3. **会话边界**：hello 成功 → record.stop（或断线）为一个 session；record.stop 后重开需新 hello
4. **"多服务端连接配置"**（§3.6 完整版）MVP 降级为：record 客户端独立配置（第 1 节），日常对话沿用小智官方云端配置不动——按模式完全隔离的连接管理留 P2
5. **Flash ring 分区**具体大小以 partitions 现状定（实现时先看分区表余量，目标 ≥1.5MB，够断网 ~1 小时音频）
6. **MQTT/唤醒等基座能力不改**；评分页/场景页/MCP 不做

## 实施里程碑

| # | 内容 | 验收 |
|---|---|---|
| M1 | Kconfig/mvp_config + record_client 最小闭环 | 编译通过；本地 uvicorn 下真机/模拟发送 hello→二进制帧→ack 全流程 |
| M2 | flash_ring 断网补传 | 拔网录音→恢复后补传，服务端 seq 去重无重复分片 |
| M3 | 模式接线 + 主菜单/工牌页 UI | 三键导航、OK 录音/停止、录音中提示显示 |
| M4 | 联调 + 真机实测 + README 更新 | 见下 Verification |

## Verification

1. **编译**：`source ~/esp/esp-idf/export.sh && python3 scripts/build.py folotoy/ai-passport`（或 idf.py set-target esp32c3 + build）零错误
2. **协议端验证**（无真机时）：本地起服务端，用 [ws_smoke.py](file:///home/zcm/AIpassport/peilian/server/scripts/ws_smoke.py) 与 record_client 同构消息对拍；`bash scripts/smoke.sh` 回归服务端无破坏（既有 1 个演示数据导致的失败除外）
3. **真机全链路**：刷机 → 配网 → 主菜单进工牌页 → OK 录音 ≥30s → 停止 → 管理端 `https://aipassport.dlzr.cn` 录音列表出现该会话、分段 seq 连续、转写出文
4. **断网补传**：录音中断网 1 分钟 → 恢复 → 服务端收到补传帧、seq 无空洞无重复
5. **回归**：MODE_CHAT 日常对话模式（小智官方链路）功能不受影响；录音中屏幕持续显示录音提示
