# Trae 项目规则

本项目为多 AI 工具协作开发。所有协作守则统一维护在仓库根目录的 **AGENTS.md**：

本地主工作目录：`/Users/bytedance/Documents/trae_projects/StackChan`
云端仓库：`https://github.com/dolphinai626/StackChan_VolcengineRTC`
ClaudeWork 规则入口：`/Users/bytedance/ClaudeWork/Documents/trae_projects/StackChan/AGENTS.md`

1. 开工前先读 AGENTS.md 并执行其中的"会话开工三步"（读 WORKLOG.md / 查 git 状态 / 登记会话）
2. 工作树里有不是本会话产生的未提交改动时，停下来问用户，不要覆盖或回退
3. 烧录设备前确认串口 /dev/cu.usbmodem2101 未被其他进程（串口采集脚本）占用
4. 每个已验证的修复立即提交，提交信息加 `[trae]` 前缀
5. 调试结论写入 debug-<topic>.md（Symptoms/Hypotheses/Evidence/Fix/Verification 格式）
