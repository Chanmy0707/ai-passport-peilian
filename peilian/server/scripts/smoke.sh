#!/usr/bin/env bash
# 冒烟测试：起服务端 → HTTP 接口冒烟（curl）→ WS 录音链路冒烟 → 停服务。
# 服务端以假 MIMO key + 不可达 MIMO_BASE_URL 启动，顺带覆盖 §8 转写降级路径（验证项 4）。
# 使用独立的 smoke.db / smoke-audio 数据目录，不动真实数据，可重复执行。
set -euo pipefail

cd "$(dirname "$0")/.." # server/

PORT="${PORT:-8000}"
BASE="http://127.0.0.1:${PORT}"
export ADMIN_TOKEN="${ADMIN_TOKEN:-admin-dev-token}"
export DEVICE_TOKEN="${DEVICE_TOKEN:-device-dev-token}"
export DB_PATH="smoke.db"
export AUDIO_DIR="smoke-audio"
export WS_URL="ws://127.0.0.1:${PORT}/record/v1/"

# ---- 环境准备 ----
[ -f .env ] || cp .env.example .env
if [ ! -x .venv/bin/uvicorn ]; then
    python3 -m venv .venv
    .venv/bin/pip install -q -r requirements.txt
fi
rm -rf "$DB_PATH" "$AUDIO_DIR"

# ---- 启动服务端（假 key + 不可达 base）----
# LD_LIBRARY_PATH 置空：部分环境（如 IDE 沙箱）的 LD_LIBRARY_PATH 会用旧版
# libsqlite3 遮蔽系统库，导致 Python 3.12+ 的 sqlite3 无法导入。
LD_LIBRARY_PATH= MIMO_API_KEY=smoke-fake-key MIMO_BASE_URL=http://127.0.0.1:9 PRACTICE_MOCK_MODELS=1 \
    .venv/bin/uvicorn app.main:app --port "$PORT" --log-level warning &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 40); do
    curl -sf "$BASE/health" >/dev/null && break
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "[FAIL] 服务端启动即退出"; exit 1; }
    sleep 0.5
done
curl -sf "$BASE/health" >/dev/null
echo "[PASS] 服务端启动 GET /health"

jget() {
    .venv/bin/python -c '
import json, sys
d = json.load(sys.stdin)
for k in sys.argv[1].split("."):
    d = d[int(k)] if k.isdigit() else d[k]
print(d)' "$1"
}

# ---- HTTP 冒烟（验证项 2）----
# 工牌：422 校验路径 → PUT → GET 回读一致
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT "$BASE/api/v1/devices/smoke-dev/badge" \
    -H "X-Admin-Token: $ADMIN_TOKEN" -H 'Content-Type: application/json' -d '{"name":""}')
[ "$code" = "422" ] || { echo "[FAIL] badge 空 name 应返回 422，实际 $code"; exit 1; }
echo "[PASS] badge 校验 422"

curl -sf -X PUT "$BASE/api/v1/devices/smoke-dev/badge" \
    -H "X-Admin-Token: $ADMIN_TOKEN" -H 'Content-Type: application/json' \
    -d '{"name":"张三","title":"销售顾问","employee_no":"1024"}' >/dev/null
badge=$(curl -sf "$BASE/api/v1/devices/smoke-dev/badge" -H "X-Device-Token: $DEVICE_TOKEN")
echo "$badge" | grep -q '"name": *"张三"' || { echo "[FAIL] badge 回读不一致: $badge"; exit 1; }
echo "[PASS] badge PUT/GET 回读"

# 角色 CRUD 一轮
role_id=$(curl -sf -X POST "$BASE/api/v1/roles" \
    -H "X-Admin-Token: $ADMIN_TOKEN" -H 'Content-Type: application/json' \
    -d '{"name":"门店店长","personality":"亲和","system_prompt":"你是店长","tts_voice":"female"}' | jget data.id)
curl -sf "$BASE/api/v1/roles/$role_id" -H "X-Admin-Token: $ADMIN_TOKEN" >/dev/null
curl -sf -X PUT "$BASE/api/v1/roles/$role_id" \
    -H "X-Admin-Token: $ADMIN_TOKEN" -H 'Content-Type: application/json' \
    -d '{"personality":"耐心亲和"}' | grep -q '"personality": *"耐心亲和"'
curl -sf -X DELETE "$BASE/api/v1/roles/$role_id" -H "X-Admin-Token: $ADMIN_TOKEN" >/dev/null
echo "[PASS] 角色 CRUD"

# 场景 CRUD 一轮（建角色供绑定）
role_id=$(curl -sf -X POST "$BASE/api/v1/roles" \
    -H "X-Admin-Token: $ADMIN_TOKEN" -H 'Content-Type: application/json' \
    -d '{"name":"陪练官"}' | jget data.id)
scenario_id=$(curl -sf -X POST "$BASE/api/v1/scenarios" \
    -H "X-Admin-Token: $ADMIN_TOKEN" -H 'Content-Type: application/json' \
    -d "{\"name\":\"门店接待\",\"description\":\"迎宾与需求确认\",\"role_id\":$role_id,\"duration_minutes\":10,\"difficulty\":\"入门\"}" | jget data.id)
curl -sf "$BASE/api/v1/scenarios/$scenario_id" -H "X-Admin-Token: $ADMIN_TOKEN" | grep -q '"duration_minutes": *10'
curl -sf -X PUT "$BASE/api/v1/scenarios/$scenario_id" \
    -H "X-Admin-Token: $ADMIN_TOKEN" -H 'Content-Type: application/json' \
    -d '{"duration_minutes":15}' | grep -q '"duration_minutes": *15'
curl -sf -X DELETE "$BASE/api/v1/scenarios/$scenario_id" -H "X-Admin-Token: $ADMIN_TOKEN" >/dev/null
echo "[PASS] 场景 CRUD"

# 录音列表空 200（WS 冒烟前数据为空）
curl -sf "$BASE/api/v1/recordings" -H "X-Admin-Token: $ADMIN_TOKEN" | grep -q '"items": *\[\]' \
    || { echo "[FAIL] recordings 初始应为空列表"; exit 1; }
echo "[PASS] GET /api/v1/recordings 空列表 200"

# ---- WS 冒烟（验证项 3/4）----
LD_LIBRARY_PATH= .venv/bin/python scripts/ws_smoke.py

# ---- 陪练 WS 冒烟（验证项：智能陪练协议/评分落库，mock 引擎）----
BASE="$BASE" WS_URL2="ws://127.0.0.1:${PORT}/xiaozhi/v1/" DB_PATH="$DB_PATH" \
    LD_LIBRARY_PATH= .venv/bin/python scripts/practice_ws_smoke.py

echo "ALL SMOKE PASS"
