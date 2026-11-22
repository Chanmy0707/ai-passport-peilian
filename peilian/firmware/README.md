# firmware/ — 固件工程（xiaozhi-esp32 基座二开）

## 目录

| 目录 | 说明 |
|---|---|
| `xiaozhi-esp32/` | 小智固件基座，clone 自 `78/xiaozhi-esp32`，所有二开提交落在 `mvp` 分支 |
| `xiaozhi-esp32/main/mvp/` | 二开新增代码统一放这里（模式状态机、工牌模式、Flash 环形缓冲等） |
| `reference/ai-passport/` | FoloToy AI Passport 官方仓库（BSP 引脚、硬件资料），只读参考，不修改 |

设计规格见仓库根目录 `MVP设计文档.md`（§3 设备端设计、§3.7 改动清单）。

## 环境与构建

- ESP-IDF：最低 v6.0.1，推荐 `release/v6.1` 分支（`v6.1` tag 有 ESP32-C3 Wi-Fi 初始化 bug，见基座 README）
- 芯片目标：ESP32-C3；板型：`folotoy/ai-passport`（基座已内置，`CONFIG_BOARD_TYPE_FOLOTOY_AI_PASSPORT`）

```sh
source /path/to/esp-idf/export.sh
cd xiaozhi-esp32

# 方式一：基座标准构建入口
python3 scripts/build.py --list-boards          # 查看板型/变体名
python3 scripts/build.py folotoy/ai-passport

# 方式二：idf.py 手动构建
idf.py set-target esp32c3
idf.py menuconfig                               # Board Type → FoloToy AI Passport
idf.py build flash monitor
```

刷机前先备份 flash（激活码、配网信息、校准数据都在 flash，换固件必做）：

```sh
esptool.py -p /dev/ttyUSB0 read_flash 0 0x800000 flash_backup.bin
```

OTA URL 指向自建服务：`idf.py menuconfig` 里配置 `OTA_URL`，或由后台下发（`main/ota.cc` 的 `GetCheckVersionUrl()` 优先读 NVS `settings["ota_url"]`，为空回退 `CONFIG_OTA_URL`）。小智官方云端绑定（日常对话模式）在固件刷入设备后按官方流程激活，激活码不固定、一次性配对。

## §3.7 改动点 → 源码映射

| §3.7 改动 | 落点 | 状态 |
|---|---|---|
| 模式状态机与主菜单（新增） | `main/mvp/mode_manager.{h,c}`（四模式枚举）+ `main/mvp/mvp_app.{h,cc}`（按键分发/录音编排/陪练编排）+ `main/mvp/mvp_ui.{h,cc}`（主菜单/工牌页/陪练页 LVGL 覆盖层）；按键接线 `main/boards/folotoy/ai-passport/ai_passport_board.cc`，挂载 `main/application.cc`（`#ifdef CONFIG_BOARD_TYPE_FOLOTOY_AI_PASSPORT` 板型门控） | 已实现（主菜单三键导航 + 工牌页 + 陪练三页面 + 设置页） |
| 工牌模式（UI + 上传协议）（新增） | `main/mvp/record_client.{h,cc}`（record WS 上传客户端，协议契约见设计文档 §5.2 I1）+ `main/mvp/mvp_config.{h,cc}`（NVS 配置）；工牌配置 HTTP `GET /devices/{id}/badge` 拉取 + NVS 缓存兜底；音频采集/Opus 编码复用 `main/audio/`（`audio_service.cc`） | MVP 已实现 |
| Flash 环形缓冲与补传（新增） | `main/mvp/flash_ring.{h,c}`，工牌模式专属；分区表 `main/partitions/v2/8m_mvp.csv`（`main/boards/folotoy/ai-passport/config.json` 指向） | MVP 已实现（断网写环形缓冲，重连从 `acked_seq+1` 补传，服务端按 seq 去重） |
| 头像 emoji 按需下载与本地缓存（新增） | `main/mvp/emoji_cache.{h,c}`（`mvp_emoji` 分区 64KB 单槽缓存，magic+id+crc 校验）+ `main/mvp/record_client.cc`（`DeliverAvatar`：缓存命中直接用，未命中按 `avatar_url` 下载 64px GIF 写缓存）+ `main/mvp/mvp_ui.cc`（`ApplyAvatarGif`：LvglGif 解码循环播放，失败/缺省回落姓名首字）+ `main/mvp/mvp_app.cc`（回调接线） | 已实现（素材不预置打包，云端素材库按需下载） |
| 场景切换 MCP 调用（新增） | 陪练模式场景选中后由 `main/mvp/mvp_app.cc` 发起协议层 `mcp tools/call set_scenario`，`main/application.cc` 转发 JSON-RPC 响应（不注册设备侧 mcp_server 工具），契约见 [智能陪练详细设计.md](../../智能陪练详细设计.md) §2.2 | 已实现（智能陪练） |
| 多服务端连接配置（新增） | 陪练连自建陪练服务、日常对话连小智官方云端：`main/mvp/mvp_app.cc` 进入陪练时缓存并改写 `Settings("websocket")` 的 url/token/version，退出恢复（`main/application.cc` 新增 `CloseAudioChannel`），契约见 [智能陪练详细设计.md](../../智能陪练详细设计.md) §1.4 | 已实现（陪练连接切换） |
| 设置页（新增） | `main/mvp/mvp_app.cc`（MODE_SETTINGS 编排：导航/音量持久化/重配网/恢复出厂）+ `main/mvp/mvp_ui.{h,cc}`（设置菜单 + 设备信息/音量/恢复出厂确认子页）；重配网走 `WifiBoard::EnterWifiConfigMode()`（不清旧 SSID，换网由配网流程完成），恢复出厂照 `system_reset.cc` 模式擦 NVS 后重启 | 已实现 |
| 配网、音频管线、小智对话协议（不改） | 配网 `main/boards/common/wifi_board.cc` + `blufi.h`（设备热点/BLUFI）；音频管线 `main/audio/*`；对话协议 `main/protocols/websocket_protocol.cc` | 直接复用 |

板级硬件事实（`main/boards/folotoy/ai-passport/config.h`，源自 `reference/ai-passport/` BSP）：ESP32-C3、8MB Flash、无 PSRAM、ES8311 全双工 I2S（MCLK=6 / WS=3 / BCLK=5 / DIN=4 / DOUT=2）、UP/DOWN/OK 三键共用 ADC 电阻阶梯（GPIO0）、USB Serial/JTAG 控制台。板级 config 采样率为 24kHz，无需为陪练对齐（音频服务编码前已把上行音频重采样为 16kHz）。

## mvp 说明

`main/mvp/`：MVP 模式实现——`mode_manager.{h,c}`（四模式枚举）、`mvp_app.{h,cc}`（三键导航/录音编排/智能陪练编排/设置编排）、`mvp_ui.{h,cc}`（主菜单/工牌页/陪练页/设置页覆盖层）、`record_client.{h,cc}`（record WS 上传 + badge 配置拉取 + 头像按需下载）、`mvp_config.{h,cc}`（NVS 配置，含 practice_ws_url/practice_api_url）、`flash_ring.{h,c}`（断网环形缓冲补传）、`emoji_cache.{h,c}`（头像 GIF 本地缓存，`mvp_emoji` 分区单槽）。智能陪练（场景选择/对话/评分三页面 + 陪练服务连接切换）契约见 [智能陪练详细设计.md](../../智能陪练详细设计.md)。仅在 ai-passport 板型编译（CMakeLists `if(CONFIG_BOARD_TYPE_FOLOTOY_AI_PASSPORT)` + `application.cc` 同名 `#ifdef` 门控），其他板型基线行为不变。接线遵循基座 `AGENTS.md`：运行状态只经 `Application::SetDeviceState()` 切换，回调里用 `Application::Schedule()` 调度变更，不阻塞主事件循环与音频任务。
