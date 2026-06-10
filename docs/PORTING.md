# Porting Guide

本文档描述把现有 StackChan / xiaozhi-esp32 / volc_conv_ai 三个上游项目接入本仓库的具体步骤。整体架构见 [DESIGN.md](DESIGN.md)。

## 1. 仓库布局

```
StackChan/
├── README.md / LICENSE / THIRD_PARTY_NOTICE.md / .gitignore / .vefaasignore
├── repos.json                 # 上游仓库清单（dolphinai626/xiaozhi-board-hal、dolphinai626/xiaozhi-app）
├── docs/
│   ├── DESIGN.md
│   └── PORTING.md             # 本文
├── scripts/
│   ├── fetch_repos.sh
│   └── scan_secrets.py
├── firmware/                  # ESP-IDF 工程
│   ├── CMakeLists.txt / partitions.csv / sdkconfig.defaults / sdkconfig.local.example
│   └── main/
│       ├── stackchan/         # 仲裁器、事件桥、robot_tools（双 adapter）
│       ├── hal/               # hal_bridge（板级抽象，M1 替换为真实实现）
│       └── apps/              # app_launcher / app_ai_agent / app_volcengine_ai
├── upstream/                  # fetch_repos.sh 拉取（不入 git）
│   ├── xiaozhi-board-hal/     # dolphinai626 fork，hal-only 分支
│   ├── xiaozhi-app/           # dolphinai626 fork，app-only 分支
│   ├── volc_conv_ai/          # Volcengine 官方
│   └── stackchan/             # 仅参考，不修改
└── patches/                   # 可选：对 upstream 的补丁
```

## 2. 环境准备

- ESP-IDF v5.5.2（已在 macOS 验证）
- Python 3.10+, jq, git, gh（推荐）

```bash
. $IDF_PATH/export.sh
```

## 3. fork 上游

> 已完成：本仓库默认使用 `dolphinai626/xiaozhi-app:app-only` 与 `dolphinai626/xiaozhi-board-hal:hal-only` 两个 fork。如果你要换到自己的 GitHub 账号，按下面步骤再做一次 fork 并更新 `repos.json`。

### 3.1 fork xiaozhi-board-hal（同账号 fork 同仓库两次的解决方案）

GitHub 不允许同一账号 fork 同一个仓库两次，本项目用 "新建 + push 镜像" 的方式落地：

```bash
# 1. 在 GitHub 网页或用 gh 创建空仓库
gh repo create <your-account>/xiaozhi-board-hal --public --description "Board-level HAL fork of 78/xiaozhi-esp32"

# 2. 完整克隆 78/xiaozhi-esp32（不要 --depth 1，否则 push 会缺对象）
git clone https://github.com/78/xiaozhi-esp32.git /tmp/mirror
cd /tmp/mirror
git remote set-url origin https://github.com/<your-account>/xiaozhi-board-hal.git
git push -u origin main

# 3. 创建 hal-only 分支（业务剥离工作在此分支上做，M1 阶段进行）
git checkout -b hal-only
git push -u origin hal-only
```

### 3.2 fork xiaozhi-app

```bash
gh repo fork 78/xiaozhi-esp32 --clone=false --fork-name xiaozhi-app
# 在 fork 上创建 app-only 分支
git clone https://github.com/<your-account>/xiaozhi-app.git
cd xiaozhi-app
git checkout -b app-only
git push -u origin app-only
```

### 3.3 业务剥离（M1 阶段在 hal-only / app-only 分支上做）

`hal-only` 分支保留板级 HAL，删除业务逻辑：
```bash
git rm -r main/application.* main/protocols/ main/mcp_server.* \
          main/audio/wake_word/ main/audio/processors/ main/wifi_board.* \
          main/ota.* main/system_info.* main/settings.* main/assets.cc \
          docs/ scripts/ 2>/dev/null || true
git commit -am "hal-only: strip business logic, keep board HAL only"
git push
```

`app-only` 分支保留业务逻辑，删除板级 HAL：
```bash
git rm -r main/boards/ main/audio/codecs/ main/display/ main/led/ 2>/dev/null || true
# 改 CMakeLists.txt：REQUIRES 中加 xiaozhi_board_hal
git commit -am "app-only: strip board HAL, depend on xiaozhi_board_hal component"
git push
```

> 实际剥离细节随 upstream 版本浮动，M1 阶段会逐文件 review。

## 4. 拉取上游

```bash
./scripts/fetch_repos.sh
```

依据 [repos.json](../repos.json) 拉取到 `upstream/`：
- `upstream/xiaozhi-board-hal/`（dolphinai626/xiaozhi-board-hal:hal-only）
- `upstream/xiaozhi-app/`（dolphinai626/xiaozhi-app:app-only）
- `upstream/volc_conv_ai/`（volcengine/ConversationalAI-Embedded-Kit-2.0:main）
- `upstream/stackchan/`（m5stack/StackChan:main，仅参考）

> 注：私有 fork 拉取需要 `git` 已配置好 GitHub 凭据（HTTPS Keychain 或 SSH key）。

## 5. 首次构建

```bash
cd firmware
cp sdkconfig.local.example sdkconfig.local
$EDITOR sdkconfig.local                      # 填入凭据
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

骨架阶段（未拉 upstream）也能 `idf.py build`：
- `adapter_xiaozhi.cpp` / `adapter_volcengine.cpp` 内部用 `__has_include` 检测，缺失时打 warning 不报错
- `hal_bridge.cpp` 是 stub，板级 API 全部返回 false，不真实驱动硬件

## 6. 凭据管理

### 6.1 本地开发

私有凭据存放在仓库外：例如 `~/.secrets/stackchan-volc.md`。

把对应字段填入 `firmware/sdkconfig.local`：
```
CONFIG_VOLC_BOT_ID="bot<your-id>"
CONFIG_VOLC_INSTANCE_ID="..."
CONFIG_VOLC_PRODUCT_KEY="..."
CONFIG_VOLC_PRODUCT_SECRET="..."
CONFIG_VOLC_DEVICE_NAME="esp32_xxxx"
```

### 6.2 量产配网

走 `app_setup` 的 SoftAP 流程，凭据写入 NVS namespace `volc_agent`。

固件二进制不带凭据，可公开发布。

### 6.3 secret 扫描

每次 `fetch_repos.sh` 后自动跑：

```bash
./scripts/scan_secrets.py upstream/
./scripts/scan_secrets.py firmware/
```

CI 中作为 pre-merge gate。

## 7. 服务端配置

### 7.1 火山方舟智能体

1. 登录 https://console.volcengine.com/ark
2. 创建对话型应用，记录 `BotID` / `InstanceID`
3. 在 "Function 配置" 中粘贴 [DESIGN.md §5.3](DESIGN.md#53-服务端-schema火山方舟--xiaozhi-后端通用) 的 6 个 schema
4. system prompt 使用 [DESIGN.md §5.4](DESIGN.md) 模板

### 7.2 Xiaozhi 后端

Xiaozhi 协议下，工具由端侧 `mcp_server.AddTool` 注册（见 `adapter_xiaozhi.cpp`），云端无需配置。
仅需在 [xiaozhi.me](https://xiaozhi.me) 控制台绑定设备。

## 8. 验证清单

构建：
- [ ] `idf.py build` 通过
- [ ] `nm build/main.a | grep ai_backend::acquire` 有输出
- [ ] `nm build/main.a | grep robot_tools::dispatch` 有输出（adapter_volcengine 内部）

运行：
- [ ] 启动后 launcher 可见两张 AI 卡片
- [ ] 进入 AppAiAgent，xiaozhi 协议握手成功
- [ ] 退回 launcher 后 AppAiAgent 卡片回到非 owner 状态
- [ ] 进入 AppVolcengineAi，火山 RTC 协议握手成功
- [ ] 切换时无 I2S 抢占 panic
- [ ] 6 个 robot tool 在两个后端各自被云端正确调用

## 9. 已知限制

- `hal_bridge` 当前是 stub；M1 阶段接入 `xiaozhi-board-hal` 真实板级实现
- Mooncake 调度器尚未集成；M2 阶段引入
- OTA 走整 bin；按后端拆 OTA 暂不规划
