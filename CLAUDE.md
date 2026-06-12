# CLAUDE.md

本项目为多 AI 工具协作开发。协作守则、分域规则、硬件互斥、构建命令、架构关键事实
统一维护在 @AGENTS.md —— 开工前必须先执行其中的"会话开工三步"。

Claude Code 专属补充：

- 串口采集脚本（pyserial）历史上由 Claude 会话管理，烧录前记得 `pkill` 自己遗留的采集进程
- 上游 SDK 修改流程：改 `upstream/volc_conv_ai` → `git -C upstream/volc_conv_ai diff > patches/volc_conv_ai.patch` → 验证 `git apply --check`
