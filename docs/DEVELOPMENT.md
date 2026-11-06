# 开发指南

本项目使用 C++20、Win32、COM 和 CMake。支持目标是 Windows 10/11 x64；FolderNote 自带的多标签栏仅用于 Windows 10，Windows 11 使用系统标签。当前构建和主要实机验证环境为 Windows 10 22H2。自动测试通过不代表已经完成 Windows 11 各版本的实机验收。

## 开发环境

- Windows x64。
- Visual Studio 2022 或对应 Build Tools，安装“使用 C++ 的桌面开发”和 Windows SDK。
- CMake 3.24 或更新版本。
- PowerShell；Windows 11 测试包还需要 Windows SDK 中的 `MakeAppx.exe`。

项目静态链接 MSVC 运行库。CMake 会从已安装的 Visual Studio 和 SDK 中选择工具链，不应把某台机器的绝对路径写入项目配置。个人配置放在不提交的 `CMakeUserPresets.json` 中。

## 构建与测试

在项目根目录运行：

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release
```

预设使用 Visual Studio 2022 的 x64 工具链，生成目录为 `build/`。Release 程序位于 `build/Release/`。也可以使用等价的显式命令：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

修改局部模块时可以先构建并运行相关测试，再在交付前运行完整测试。例如：

```powershell
cmake --build build --config Release --target ExplorerTabsTests NavigationViewTests --parallel
ctest --test-dir build -C Release --output-on-failure -R "^(ExplorerTabsTests|NavigationViewTests)$"
```

构建本身不会注册资源管理器扩展，也不会替换已安装的程序。

## 目录与组件

| 路径 | 内容 |
| --- | --- |
| `src/` | 程序、Shell 扩展、导航、设置界面和安装器源码 |
| `tests/` | CTest 测试、隔离窗口夹具和人工诊断探针 |
| `assets/` | 图标源文件、生成脚本和程序所需图像资源 |
| `packaging/` | MSIX 清单模板、Windows 11 打包脚本与说明 |
| `docs/` | 使用说明和开发文档 |
| `build/` | CMake 生成文件与本机编译输出，不提交 |
| `dist/` | 本机发布目录和压缩包，不提交 |

| 构建产物 | 职责 |
| --- | --- |
| `FolderNote.exe` | 备注编辑、批量操作、统一设置、中键菜单，以及可选的文件对话框后台组件 |
| `FolderNoteShell.dll` | 传统菜单和 Windows 11 现代菜单命令，接收完整选择列表并启动主程序 |
| `FolderNoteNavigation.dll` | 资源管理器内部导航扩展、文件视图消息处理和 Windows 10 标签栏 |
| `Install.exe` | 通用版安装、版本目录切换、当前用户菜单和快捷方式注册 |
| `Uninstall.exe` | 完整卸载当前用户组件，处理被占用文件及 Windows 11 菜单包 |
| `Install-Win11.exe` | Windows 11 首层菜单测试包的安装入口 |

主要源码分工：

- `core.*`：备注解析、字符限制、文件写入和属性修复。
- `shell.cpp`：Shell 命令、单选与多选传递。菜单枚举阶段不读取备注文件。
- `navigation_extension.cpp`、`navigation_view.*`：COM 宿主、视图绑定和双击导航；`explorer_refresh.*` 负责相关备注列表刷新。
- `explorer_tabs.*`、`tabs_model.h`、`tabs_activation.cpp`：标签数据、原生 Rebar 承载、导航状态和启用状态核对。
- `quick_folders.*`、`quick_menu.*`、`quick_popup.*`、`quick_runtime.cpp`：置顶与历史、菜单交互、请求次序和可选后台组件。
- `theme.*`、`theme_scroll.*`、`quick_icons.*`：主题、滚动条和 Shell 图标读取。
- `navigation.cpp`：统一设置界面；`navigation_config.cpp` 保存功能配置；`navigation_diagnostics.cpp` 生成诊断信息。
- `install.cpp`、`modern_install.cpp`、`navigation_registration.*`：安装、升级、卸载和扩展注册。

## 开发约束

### 数据与安装

文件夹备注存于该目录自己的 `desktop.ini`，与功能开关分离。保存时保留未知节、键、注释及图标等设置；批量“修复”只恢复属性，不改写备注内容，不递归处理子目录。修改备注写入逻辑时，应验证失败后的原始文件或恢复备份仍可用。

功能配置和置顶 / 历史保存在当前用户注册表中。安装、功能开关、卸载只操作本工具拥有的项；卸载不遍历并删除用户文件夹备注，不清理未知文件。版本目录用于避免覆盖正在被资源管理器加载的 DLL。自动加载入口涉及机器级注册，但用户 DLL 路径仍按当前用户注册，不能使其他用户加载该用户目录内的 DLL。

### 资源管理器内代码

资源管理器的稳定性优先。窗口过程、COM 边界和异步完成回调不能向宿主抛出未处理异常。窗口子类化应发生在窗口所属线程；对 `IShellBrowser` 等对象的调用按所在 COM apartment 管理。

Shell 查询和导航可能重入窗口消息，甚至在调用尚未返回时更换或释放站点。保持站点代次、查询代次及导航事务的检查；在旧结果回写前确认对象、窗口和事务仍有效。不要同时发起多个标签导航，不要让过期错误或计时器回滚较新的导航。

双击空白处导航和仅资源管理器模式的快速定位由扩展处理，没有独立常驻进程。仅启用“中键快速定位”及“同时支持打开 / 保存文件对话框”时，才运行 `--quick-helper`，并登记本工具自己的登录启动项。该组件使用的全局鼠标钩子只做轻量判断，完整窗口与权限校验留在处理线程；关闭功能时退出。不要把可选组件改为所有安装都必须运行的后台服务。

菜单图标和子目录读取应保持异步、按需及有界。网络目录或 Shell 提供程序响应缓慢时，不能阻塞资源管理器菜单枚举，也不能让已关闭的中键菜单长期占用下一次打开的机会。

### 界面与输入

共享主题、DPI 和窗口定位实现。主题需兼容浅色、深色、跟随系统及高对比度；界面里的版本号来自生成文件，不能手写另一个版本值。输入处理只消费属于本功能的手势，文件列表正常滚动和其他应用输入应保留。文件对话框跳转必须核对地址栏，不能提交“打开 / 保存”按钮或覆盖文件名控件。

## 测试与实机验证

当前 CTest 注册了 18 组测试。测试通过隐藏窗口、临时目录和进程内注册表重定向隔离状态，不应修改真实置顶、历史、功能开关或安装注册。需要新增安装或注册测试时，沿用现有隔离夹具，不要把实际 `Install.exe --quiet` 当成普通单元测试步骤。

| 改动范围 | 优先运行的测试 |
| --- | --- |
| 备注与刷新 | `CoreTests`、`ExplorerRefreshTests` |
| 右键菜单和多选 | `ShellTests`、`ManifestTests` |
| 安装、注册和卸载 | `NavigationRegistrationTests`、`UninstallTests` |
| 双击导航与扩展生命周期 | `NavigationTests`、`NavigationExtensionTests`、`NavigationViewTests` |
| 中键菜单和记录 | `QuickFoldersTests`、`QuickRuntimeTests`、`QuickMenuGateTests`、`QuickPopupTests` |
| 标签栏 | `ExplorerTabsTests`、`NavigationViewTests`、`NavigationExtensionTests` |
| 主题、设置与图标 | `ThemeControlsTests`、`SettingsHubTests`、`QuickIconTests`、`WindowPositionTests` |

部分测试夹具保留在系统临时目录的 `FolderNote-Test-*` 下，便于检查。`QuickDialogProbe` 和 `TabsActivationProbe` 是额外诊断程序，不是全部验收的替代品。后者默认读取当前 Shell 窗口和标签栏状态；不要在自动测试中使用会改变真实窗口的激活操作。

实机验收至少记录系统版本、程序版本、是否加载旧 DLL、缩放比例和主题。发布前按改动覆盖安装 / 升级 / 卸载、连续菜单开关、多选、普通目录与慢速路径、背景标签关闭、滚轮快速切换及真实文件对话框跳转。安装器不会强制重启资源管理器；若旧窗口仍加载旧扩展，应在没有复制或移动任务时关闭相关窗口并重新打开，必要时由使用者安排重启资源管理器。

1.8.6 已在 Windows 10 22H2 完成 Release 编译和 18 组自动测试，并验证新资源管理器窗口加载新版标签栏。Windows 11 24H2/25H2、U 盘 / NAS、拒绝访问 ACL、只读介质、全部 DPI / 主题及真实文件对话框等场景仍需相应实机验收。不要仅凭编译、测试桩或 MSIX 清单校验声称这些场景已完成。

## 版本与打包

版本号只在 `CMakeLists.txt` 的 `project(FolderNote VERSION ...)` 中维护。重新配置后，CMake 生成窗口版本文字、资源版本及 MSIX 清单版本。发布时同时更新 [版本记录](../CHANGELOG.md) 和受影响的说明；不要直接编辑 `build/generated/` 下的生成文件。

### 通用版

完成 Release 构建和测试后：

```powershell
cmake --install build --config Release --prefix dist/FolderNote
Compress-Archive -Path dist/FolderNote -DestinationPath dist/FolderNote-win-x64.zip -Force
```

这里的 `cmake --install` 仅把运行文件和配置好的文档复制到指定目录，不会完成 Windows 菜单或导航扩展注册。通用包应包含 `FolderNote.exe`、两个 DLL、`Install.exe`、`Uninstall.exe` 和说明文件；使用者完整解压后运行 `Install.exe`。

### Windows 11 首层菜单测试包

```powershell
.\packaging\build-win11.ps1
```

脚本读取 `build/AppxManifest.xml` 和 `build/Release/`，检查主程序与清单版本一致，调用 SDK 中的 `MakeAppx.exe` 校验并打包，输出 `dist/FolderNote-Win11-<版本>/` 及 `dist/FolderNote-<版本>-win11-test.zip`。因此运行脚本前应使用上面的默认 `build/` 目录完成 Release 构建。

目前 MSIX 为未签名的个人测试包。MakeAppx 成功只说明包结构通过校验；正式分发需要准备签名，并在目标 Windows 11 系统验证安装、菜单分组和卸载。更多安装限制见 [Windows 11 说明](../packaging/README-Win11.md)。

不要提交编译输出、发布 ZIP、机器专用配置、证书私钥、访问令牌或本机 `desktop.ini`。在发布记录中区分自动测试结果和实际手工验证结果。
