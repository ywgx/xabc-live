<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport · ChatGPT 语音伙伴

本项目基于 [FoloToy AI Passport](https://github.com/folotoy/ai-passport) 开源固件，把它变成一个随身的实时语音助手。设备通过 Wi-Fi 直接连接
[XSI Router](https://xsirouter.com/blog/gpt-live-1-openai-live-api/) 提供的 OpenAI 官方 Live API，使用全双工语音模型
`gpt-live-1`：开机联网后自动进入对话，全双工——它说话时你可以直接开口插话；五分钟没有对话自动结束。对话中屏幕只有
状态圆球和一行状态。

## 怎么用

| 按键 | 作用 |
| --- | --- |
| OK 短按 | 空闲时开始对话；助手说话时立即打断它（也可以直接开口插话）；出错后重试 |
| OK 长按 | 结束对话 |
| ▲ 长按 | 进入手机扫码设置（换 Wi-Fi / Key） |
| ▲ / ▼ 短按 | 音量加 / 减 10%（默认 150% 最大；0–100% 为芯片音量，110–150% 再叠加 1.4–3 倍数字增益，带软限幅不破音） |

屏幕中央的圆球表示状态：

| 颜色 | 状态 |
| --- | --- |
| 绿色 | 已就绪，按 OK 开始（与左上角 Wi-Fi 绿点同色；对话中顶部标题、Wi-Fi 点和电量隐藏） |
| 浅绿色（呼吸） | 正在连接 Wi-Fi、检查网络或连接 ChatGPT |
| 绿色（随你的声音变大） | 正在听你说话 |
| 紫色（随回答的声音变大） | 助手正在回答 |
| 红色 | 出错，状态行显示原因，按 OK 重试 |
| 二维码 | 设置模式：手机扫码连接热点后填写 Wi-Fi 和 API Key |

说“拜拜”“再见”“关闭吧”之类的简短告别，助手说完再见后对话自动结束（只认 12 个字以内的整句，避免“怎么关闭
电脑更新”这类问题误关；告别后你又接着说话则不结束）。

**自动关机**：对话结束后（或连不上网时）10 分钟没有对话、也没按键，屏幕提示“10 分钟未使用，已自动关机”后进入
深度睡眠（Wi-Fi 先正常断开）；按任意键开机，开机后直接进入对话。连着电脑（USB 有主机）时不会自动关机，方便
调试和烧录；只接充电器照常关机。

计费提示：`gpt-live-1` 按会话时长计费，静音也算时间。五分钟没有任何对话会自动结束，单次会话最长 30 分钟；
开机自动开始对话可用 `autostart off` 关闭。

## 第一次配置（手机扫码，约 1 分钟）

1. 设备第一次开机（还没配置）时，屏幕显示二维码和“扫码连接 ChatGPT-XXXX”。
2. 用手机相机扫码，连上设备热点，设置页会自动弹出（没弹出就在浏览器打开任意网址或 `192.168.4.1`）。
3. 第 1 步：从附近网络里选 Wi-Fi（只支持 2.4 GHz），输入密码（明文显示）。
4. 第 2 步：粘贴 [XSI Router](https://xsirouter.com) 的 API Key，点“保存并重启”。
5. 设备先验证这个 Wi-Fi 能连上才正式保存，然后自动进入对话；连不上会重新显示二维码并在页面上说明原因，
   原来的配置不受影响。

以后要换 Wi-Fi 或 Key：**长按 ▲** 重新进入设置（已有 Key 可留空保持不变）；设置时按 OK 退出。
中文输入法打出的全角标点会自动转成半角，首尾空格会自动去掉。API 地址固定为 `api.xsirouter.com`。

开发者也可以用 USB 串口配置（Key 不会显示或写入文件）：

```bash
. ~/esp/esp-idf-v5.5.3/export.sh          # 提供 pyserial
python3 tools/live_provision.py --ssid 你的WiFi名称
```

其他选项：`--host`、`--voice`（默认 `marin`）、`--prompt "助手指令"`、`--show`（查看当前配置，Key 打码）。
从 `0x0` 烧录合并固件 `full.bin` 会清空配置；只更新程序时把 `build/firmware/<hash>/FoloToy-AI-Passport.bin`
写到 `0x10000`，配置会保留。

## 工作原理

```text
麦克风 ──► μ-law ──► 上行发送任务 ──► session.input_audio.append ──────────────┐
                                                                              ▼
                                   wss://api.xsirouter.com/v1/live/sessions（gpt-live-1）
                                                                              │
喇叭 ◄── 抖动缓冲（自动追延迟）◄── Base64 解码 ◄── session.output_audio.delta ◄───┤
屏幕状态 ◄──────────────────── session.input/output_transcript.delta ◄────────┘
```

- **音频格式**：双向 8 kHz G.711 μ-law（`audio/pcmu`），每个方向约 11 KB/s，在弱网下也稳定。gpt-live-1 不支持
  上下行使用不同格式；16 kHz PCM 在这块没有 PSRAM 的芯片上内存不够，已移除。
- **网络层**：自带精简 WebSocket 客户端（`live_ws.c`），一个网络任务独占 TLS 连接、统一收发，不再使用
  `esp_websocket_client`（它收帧时持锁，链路一抖就饿死上行）。只在整条 TLS 记录到齐时才读（mbedTLS 动态缓冲
  不能处理半条记录），开启 `TCP_NODELAY`。
- **上行节奏**：gpt-live-1 按输入音频的时间节奏输出，所以上行必须严格实时。麦克风任务只负责采样入队、从不阻塞。
- **TCP 调优**：到美东的跨洋链路白天有丢包，lwIP 默认不发 SACK，丢包后服务端拥塞窗口塌缩、下行只剩 2–5 条/秒。
  已开启 `CONFIG_LWIP_TCP_SACK_OUT`、乱序缓存 8 包、TCP 定时器 50 ms。
- **下行缓冲**：先缓冲 600 ms 再播；网络抖动积压超过 1.2 s 时跳过静音帧追回实时，不剪掉正在说的话。
- **全双工**：板上没有回声消除，麦克风一直上传原声，由模型自己区分你的插话和它自己的回声（指令里提示了这一点），
  直接开口就能打断它。按 OK 只在本地立即清空播放、短暂丢弃路上的下行音频，方便你开口，不给模型发任何指令。
- **多个 Wi-Fi**：最多保存 4 个网络，开机和断线后扫描，连信号最强的可用网络，失败自动换下一个（例如家里的
  Wi-Fi，不在家时用手机热点）。
- **网络自愈**：联网后和每次开会话前，先对 API 服务器做一次 TCP 自检；不通时干净地断开并重新关联 Wi-Fi 再试。
  会话中 6 秒收不到任何下行数据即判定断线，屏幕提示重连，不会静默卡住。
- **不联网查询**：设备不提供任何查询工具，指令要求模型直接用已有知识回答。gpt-live-1 的委派无法关闭，万一模型
  仍发出 `session.delegation.created`，设备立即回一句“这里没有联网查询，请直接回答”，避免它一直等待。
- **字库**：16 px 中文字库只包含界面状态文字实际用到的字（由 `tools/gen_live_font.py` 从源码提取，约 400 个
  字形）。服务端报错原文可能含字库外的字，会显示为占位框。

## USB 控制台命令

设备的 USB 串口（`passport>`）支持以下命令，`tools/live_provision.py` 和 `tools/live_autotest.py` 都通过它工作：

| 命令 | 作用 |
| --- | --- |
| `wifi <ssid> [密码]`、`wifi_list`、`wifi_del <ssid>` | 添加/更新、列出、删除已保存的 Wi-Fi（最多 4 个，自动连信号最强的可用网络） |
| `key <api-key>`、`host`、`voice`、`prompt "<指令>"` | 写入配置（NVS） |
| `config`、`clear`、`restart`、`poweroff`、`setup` | 查看配置（Key 打码）、清空配置、重启、立即关机、进入扫码设置 |
| `debug on\|off` | 在日志里打印会话统计和双方转写（默认关，不保存） |
| `talk`、`hangup` | 等同 OK 短按、OK 长按 |
| `autostart on\|off` | 开机后是否自动进入对话（默认开） |
| `nettest` | Wi-Fi 信号、DNS 和到 API 服务器 / 百度的 TCP 建连耗时 |
| `testmic on\|off`、`testplay <url>`、`volume <0..150>` | 自动化测试用：用局域网下载的音频代替麦克风、临时音量 |

注意：打开 USB 串口会让芯片复位。

## 自动化测试

```bash
. ~/esp/esp-idf-v5.5.3/export.sh
python3 tools/live_autotest.py --rounds 3 --nettest
```

脚本全程只开一次串口，等设备联网后逐轮执行：`talk` → 用 macOS `say` 合成问题，经本机临时 HTTP 服务让设备下载并代替
麦克风实时上传（默认静音测试，设备音量临时为 0）→ 收集双方转写和每 2 秒的统计 → `hangup`，最后给出每轮的
PASS/FAIL：是否识别到语音、是否有回答、上下行速率、卡顿次数、最低内存、错误日志。`--speaker` 改为用电脑喇叭真实播放；`--barge` 在助手讲长故事时插入新问题（默认按 OK，加
`--voice-barge` 只靠开口）；`--followup 再讲一个` 在同一会话里追问（加 `--expect-close` 检查说拜拜后自动结束）。

## 代码结构

| 路径 | 内容 |
| --- | --- |
| `main/main.c` | 启动顺序与按键映射 |
| `main/app_live.c` | 会话状态机、网络任务（TLS + WebSocket）、协议事件、断线检测、自动开始/结束 |
| `main/app_audio.c` | 麦克风 / 播放两个音频任务、μ-law 缓冲、延迟追赶、OK 打断时的播放清空、测试麦克风 |
| `main/app_ui.c` | 极简界面（状态圆球、一行状态；空闲时显示标题、Wi-Fi 点、电量；设置二维码） |
| `main/app_wifi.c` | Wi-Fi、TCP 自检与干净重连 |
| `main/app_portal.c` | 手机扫码设置：热点、DNS 劫持与设置网页 |
| `main/app_power.c`、`main/app_diag.c` | 自动关机（深度睡眠）、看门狗诊断 |
| `main/app_config.c`、`main/app_console.c`、`main/app_testplay.c` | NVS 配置、USB 控制台命令、测试音频下载 |
| `main/live_proto.c`、`main/live_ws.c`、`main/live_g711.c`、`main/live_policy.c`、`main/live_config.c`、`main/live_form.c` | 不依赖 ESP-IDF 的纯逻辑，有主机测试 |
| `tests/test_live_*.c` | 协议解析、Base64、G.711、委派事件、打断窗口、延迟追赶、音量、告别识别、配置和设置网页辅助函数的测试 |
| `tools/live_provision.py`、`tools/live_autotest.py` | USB 配置脚本、端到端自动化测试 |
| `tools/gen_live_font.py`、`assets/fonts/` | 中文字库的生成脚本、字库与许可 |

`main/demo_*.c` 是基线的硬件测试页，保留给它们的主机测试使用，不编进这个应用。

## 构建与验证

```bash
. ~/esp/esp-idf-v5.5.3/export.sh
./tools/validate.sh            # 主机测试 + 固件构建 + 合并镜像校验
```

产物：`build/FoloToy-AI-Passport-full.bin`，从 `0x0` 烧录。开发说明见 [`AGENTS.md`](AGENTS.md) 与
[文档索引](docs/README.md)。

## 已知限制与网络注意事项

- 没有声学回声消除：全双工依赖模型自己分辨回声，音量很大或环境安静时偶尔可能把回声当成插话；遇到时可调低音量。
- API Key 存在设备 Flash 中，拿到设备的人可以读出；请只在自己的设备上使用，必要时在 XSI 控制台限制额度或轮换 Key。
- 只实现了 XSI 的 OpenAI 官方 Live API（`/v1/live/sessions`）。Codex Live V3 只支持 WebRTC，需要额外的中转服务。
- 实测部分 Wi-Fi 7 路由器在设备未正常断开（例如复位）后重新关联时，可能保留错误的客户端状态：DHCP/DNS 正常
  但所有 TCP 都被丢弃。固件的 TCP 自检会自动干净地重连来恢复；建议给设备使用不在 MLO 组里的 2.4 GHz WPA2 SSID。
- ESP-IDF 5.5.3 的 linenoise 在“dumb”终端模式下，输入行达到命令行上限时会越界写；不要向控制台发送超长行
  （测试音频因此改走局域网 HTTP）。
