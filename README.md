# HIY Terminal

本地及共享多标签终端。Windows 10 1809 或更新版本使用 ConPTY；Linux 和 macOS 使用 PTY。
调用本机已安装的 PowerShell、CMD、Bash 或系统 shell，不安装或内嵌 shell。

在已有 `test` 构建目录中构建应用。

```powershell
cmake --build test/cmake-build-debug --target hiyui_terminal
```

也可从源码独立配置静态应用。

```powershell
cmake -S test/app/terminal -B test/app/terminal/build -DCMAKE_BUILD_TYPE=Release
cmake --build test/app/terminal/build --config Release --target hiyui_terminal
```

应用、HIY-UI 和 libvterm 静态链接，图标沿用库内嵌资源，正文使用系统字体。
应用运行不读取工作区资源目录。发布保留安装规则收集的许可文本；系统库和本机 shell
属于运行环境。单个可执行文件的发布配置使用静态依赖及静态 CRT，保持
`HIYUI_ENABLE_FFMPEG=OFF`；启用 FFmpeg 的构建仍需提供其动态库。

| 操作 | 快捷键 |
|---|---|
| 新建所选 shell 会话 | Ctrl+Shift+T |
| 关闭当前会话 | Ctrl+Shift+W |
| 复制选区 | Ctrl+Shift+C，macOS 也支持 Command+C |
| 粘贴 | Ctrl+Shift+V、Shift+Insert，macOS 也支持 Command+V |
| 查看滚动历史 | 滚轮、Shift+PageUp / PageDown |
| 中断前台程序 | Ctrl+C |
| 向 shell 发送 Meta 组合键 | 左 Alt+字符，右 Alt 保留 AltGr 文本输入 |

拖动选择文本。程序启用终端鼠标报告时，按 Shift 可选择文本及滚动历史。
关闭标签终止对应 shell 会话；shell 自行退出后保留该标签的输出供选择复制。

共享当前本地终端时，打开工具栏“共享 / 连接”，填写监听地址和端口，选择权限后点击
“开始共享”。默认监听 `127.0.0.1:8765`，权限为 `readOnly`；在其他机器上连接时，
监听地址填写主机的局域网地址或 `0.0.0.0`，连接地址使用主机可达的地址。

点击“复制令牌”，将令牌交给连接方。连接方填写 `ws://主机:端口/terminal` 和令牌，
点击“连接”，打开一个远程标签页。每个监听端口只共享一个本地标签，不允许远端新建 shell。

| 权限或操作 | 行为 |
|---|---|
| `readOnly` | 查看、选择和复制当前屏幕；服务端拒绝键盘、粘贴、焦点和鼠标输入 |
| `all` | 允许输入、粘贴和终端鼠标操作，能以本地 shell 的身份执行命令 |
| 修改共享权限 | 应用于该标签的所有连接；服务端立即按新权限检查后续输入 |
| 停止共享 | 断开远端并关闭监听，本地 shell 继续运行 |
| 重新开始共享 | 生成新令牌，原令牌失效 |
| 关闭远程标签 | 仅断开此连接，不终止主机 shell |
| 关闭本地标签 | 停止该标签的共享并终止对应 shell |

令牌由系统随机数生成器生成，含 256 位随机数据。当前传输为 **未加密的 `ws://`**，
令牌和终端内容会经过明文连接；用于本机、受信局域网或加密隧道。当前应用不提供 TLS
证书配置或浏览器客户端，不接受带 `Origin` 的 WebSocket 升级请求。

主机决定终端行列数，远端缩小字体以容纳同一网格。新连接立即获得当前彩色屏幕、标题、
光标和鼠标模式，包含备用屏幕中的内容；远端不复制主机滚屏历史。断开后保留最后画面，
再次点击“连接”建立新标签。最多同时连接 8 个客户端，共享网格上限为 512 列 × 128 行，
单次输入上限为 64 KiB；超出权限或协议范围的输入会被拒绝并断开连接。

`TerminalModel` 负责 VT 状态和输入编码，`Terminal` 负责界面与原始输入事件。
`LocalSession` 管理本地 shell，`RemoteSession` 管理网络连接，`ShareServer` 认证并检查权限。
客户端发送原始操作，主机按当前终端模式编码；终端查询响应只由主机产生。
每个客户端保留最新完整画面，连接变慢时跳过中间帧，后续画面不依赖丢弃的帧。

传输使用应用私有的 cpp-httplib 0.58.0 fork，固定提交见
[`third_party/cpp-httplib/ORIGIN.md`](third_party/cpp-httplib/ORIGIN.md)。头文件静态编入应用，
不进入 HIY-UI 嵌入式公共依赖，不需要附带网络库 DLL。

协议实现来自 libvterm 0.3.3。支持 VT 控制序列、16/256/真彩色、UTF-8、宽字符、
备用屏幕、括号粘贴及鼠标报告。图片协议和 Kitty 键盘协议不在当前接口范围内。

运行验收操作及预期结果。

| 操作 | 预期结果 |
|---|---|
| 在 CMD、PowerShell、Bash 输入中文及长命令，连续退格、Delete、Home/End | 输入、光标及显示保持一致 |
| 使用方向键历史、Tab 补全、Ctrl+C、Bash Ctrl+Z | 由实际 shell 处理 |
| 输出 16 色、256 色和 RGB 文本，使用回车刷新进度 | 字体和背景颜色正确，原行被覆盖 |
| 运行 vim、less 或 top，退出后滚动 | 备用屏幕及主屏幕恢复，历史内容可读 |
| 创建多个标签、拖动排序、重命名并关闭 | 输入与输出始终对应各自会话 |
| 连续输出时关闭标签或窗口 | 窗口可退出，管道与工作线程完成回收 |
| 改变窗口大小、跨 DPI 显示器、切换主题 | 字符网格及 shell 窗口尺寸更新，输入法候选位置跟随光标 |

协议单元测试不创建窗口；C 接口测试在启用现有 C 绑定构建时加入。

```powershell
cmake --build test/cmake-build-debug --target test_terminal_buffer
ctest --test-dir test/cmake-build-debug -R '^terminal_buffer$' --output-on-failure
```

启用 C 绑定后，构建并运行 C 接口测试。

```powershell
cmake --build test/cmake-build-debug --target test_terminal_c_api
ctest --test-dir test/cmake-build-debug -R '^terminal_c_api$' --output-on-failure
```

组件测试沿用画廊的自动化入口，覆盖键盘、文本提交、选区、事件与生命周期、主题和 DPI。

```powershell
cmake -S test -B test/cmake-build-debug -DHIYUI_ENABLE_COMPONENT_TESTS=ON
cmake --build test/cmake-build-debug --target demo_component_gallery
ctest --test-dir test/cmake-build-debug -R 'Terminal\.' --output-on-failure
```

模型、共享协议与真实 PTY/WebSocket 回环测试覆盖输入编码、彩色画面、晚加入、认证、
权限撤销、多客户端同步、端口占用、退出和令牌失效。

```powershell
cmake --build test/cmake-build-debug --target test_terminal_model test_terminal_sharing
ctest --test-dir test/cmake-build-debug -R '^terminal_(model|sharing)$' --output-on-failure
```
