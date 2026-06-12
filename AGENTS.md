# AGENTS.md — 多 AI 协作守则

> 本项目由一位 PM 指挥多个 AI coding 工具（Claude Code / Trae / Cursor 等）协作开发。
> AI 工具之间**不直接通信**，所有协同通过仓库内工件同步：git 提交、WORKLOG.md、debug-*.md。
> 任何 AI 会话开工前必须完整阅读本文件。

## 1. 会话开工三步（必做，跳过任何一步都可能毁掉别人的工作）

1. 读 [WORKLOG.md](WORKLOG.md) 最新条目 + `git log --oneline -5` + `git status`
2. **工作树里有"不是你改的"未提交改动 → 停下问用户**。禁止基于未知改动继续写，
   禁止回退未知改动（本项目发生过两个 AI 用相反理由互相回退对方方案的事故）
3. 在 WORKLOG.md 顶部登记本次会话：日期、工具名、任务、计划触碰的文件域

## 2. 分域避撞

同一时间一个文件域只允许一个会话改动。开工登记时声明占用的域：

| 域 | 范围 |
| --- | --- |
| firmware-audio | `firmware/main/apps/app_ai_agent/`、`firmware/main/hal/board/cores3_audio_codec.*` |
| firmware-ui | `firmware/main/hal/board/stackchan_display.*`、`firmware/main/stackchan/`、`firmware/main/apps/`（除 app_ai_agent） |
| sdk-patches | `upstream/`、`patches/`、`scripts/` |
| docs-cloud | `docs/`、`debug-*.md`、云端智能体配置 |

跨域改动（如重构）：先在 WORKLOG.md 声明，确认无其他会话活跃后再动。

## 3. 硬件互斥（单台设备，最容易撞车）

只有一台 CoreS3，串口 `/dev/cu.usbmodem2101` 同时只能被一个进程持有：

- 烧录/监听前先 `lsof /dev/cu.usbmodem2101`，有占用先弄清是谁（常见：遗留的串口采集脚本）
- 长期串口采集进程**用完即杀**；采集日志统一放 `/tmp/stackchan_<topic>.log`
- 同一时间只有一个"持机会话"可以 flash/烧录验证；其他会话只做 `idf.py build` 编译验证
- 烧录失败报 "device reports readiness to read but returned no data" = 串口被占，先杀采集进程

## 4. 变更纪律

- **一个已验证的修复 = 一个提交，验证通过立即提交**。禁止跨会话堆积脏工作树
  （多个修复混在脏树里无法区分作者与意图，是本项目踩过的最大的坑）
- 提交信息加工具前缀：`[claude]` / `[trae]` / `[cursor]`，正文说清动机不只是动作
- 验证通过的提交及时 `git push origin main`；每次开工先 `git pull`
- **不同意他人最近的改动**：在对应 debug-*.md 里摆出你的证据，请用户裁决；
  不要直接改回——对方的改动可能有你没看到的实测依据
- 分支策略：日常走 main 直接小步提交（单台设备无法并行测试两个分支，分支收益低于成本）；
  仅大型重构（如 volc_agent.cpp 拆分）开 `refactor/<topic>` 分支

## 5. 调试知识落盘

调试过程写入 `debug-<topic>.md`，固定格式：Symptoms → Hypotheses → Evidence → Fix → Verification。

- **只追加，不删除他人的证据**；推翻假设时写明依据
- 解决后标 `Status: [RESOLVED]` 并写 Resolution 小节
- 新会话调试前先翻已有 debug 文档——大量根因已被定位过，别重复排查

## 6. 红线（违反会造成数据丢失或泄密）

- **凭据不进 git**：`firmware/sdkconfig.local`、含 AccessKey/Secret 的服务端日志、`.pcm` 录音
- **`upstream/` 的任何改动必须同提交重生成 `patches/<name>.patch`**
  （`scripts/fetch_repos.sh` 会 `reset --hard` 清掉裸改）
- `firmware/sdkconfig` 是生成文件：配置变更必须同步写入 `sdkconfig.defaults` 才能持久
- 不要修改 `firmware/xiaozhi-esp32/` vendored 代码，除非同步更新 `firmware/patches/xiaozhi-esp32.patch`

## 7. 构建/烧录速查（所有工具统一命令）

```bash
source /Users/bytedance/esp-idf-v5.5.2/export.sh
cd firmware
idf.py build                                  # 编译验证（任何会话可做）
idf.py -p /dev/cu.usbmodem2101 build flash    # 烧录（仅持机会话）
# 串口监听 115200；抓日志建议用 pyserial 脚本写 /tmp/stackchan_<topic>.log
```

## 8. 关键架构事实（别改回去的坑，详见对应 debug 文档）

- RTC opus RTP 参数必须 `48000/960`（RFC 7587 固定 48k 时钟），**严禁改回 16000/320**
  → debug-uplink-stream-drop.md
- 气泡字幕字体必须用 assets 分区完整字体（内置 basic 字体仅 800 字形，缺字形静默不渲染）
- Volc 会话期间 `WIFI_PS_NONE`，退出恢复 `WIFI_PS_MIN_MODEM`
- 锁序固定 `_send_mutex → _mutex`；网络发送不得持 `_mutex`
- volc SDK 上游有两个生命周期 bug，由 `patches/volc_conv_ai.patch` 修复，
  已提上游 PR volcengine/ConversationalAI-Embedded-Kit-2.0#5
- CoreS3 无硬件回采参考，AEC 不可用，半双工门控是当前设计约束
