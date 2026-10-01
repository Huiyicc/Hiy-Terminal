# HIY Terminal

本地多标签终端。Windows 10 1809 或更新版本使用 ConPTY；Linux 和 macOS 使用 PTY。
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
