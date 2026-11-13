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
| `FolderNote.exe` | 备注编辑、批量操作、统一设置、中键菜单，以及按需运行的桌面 / 文件对话框后台组件 |
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
- `quick_dialog.*`：标准打开 / 保存对话框的前台激活、地址栏校验与目录定位，失败时区分激活、输入和地址栏阶段。
- `quick_legacy.*`：旧式浏览文件夹签名识别，以及通过已有后台组件提交目录定位和回收跨进程路径缓冲。
- `quick_transfer.*`：当前视图的 Shell 选择快照，以及带系统进度与冲突提示的复制 / 移动。
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

双击空白处导航由扩展处理。后台组件的启用条件为 `quickEnabled() || (tabsSupported() && tabsEnabled() && preferNewTabEnabled())`：中键快速定位和 Windows 10 新标签接管共用 `--quick-helper` 及本工具自己的登录启动项，两者均停用时退出。只开启新标签接管时不应启用中键鼠标处理。安装、升级与设置保存统一调用 `quick::syncHelper`，卸载调用 `quick::stopHelper`，保留用户功能设置。文件对话框支持关闭时，资源管理器中键仍由扩展处理；开启时由后台组件处理资源管理器和文件对话框，避免重复触发。全局鼠标钩子只做轻量判断，完整窗口与权限校验留在处理线程。桌面窗口需匹配 Shell 进程与桌面宿主结构；点击菜单中的目录通过 Shell 或新标签请求打开，不对桌面调用目录导航。不要把可选组件改为所有安装都必须运行的后台服务。

“始终在新标签中打开文件夹”保存为当前用户 `Software\FolderNote` 下的 `ExplorerOpenInNewTab` DWORD，默认关闭。旧版 `saveSettings` 调用保留这个值，关闭多标签也不清除用户选择。只接管启用后新出现的资源管理器窗口，不合并已存在的窗口；没有可用标签宿主时保留首个普通窗口。BHO 在 SetSite 阶段向后台申请暂缓呈现，确认已有支持当前协议的目标后，在来源窗口第一次显示前设置 DWM APP cloak；不改变 WS_VISIBLE，保证 Shell 视图和标签栏继续初始化。窗口拥有独立的 20 秒恢复计时器，关闭过程不解除 cloak，失败、停用和超时则恢复显示。晚于首次显示才附着的 BHO 不强制隐藏既有窗口。交接确认遇到同目录的重复完成事件会限时重试，不重新增加标签；真实导航或选择变化仍取消关闭。标签右键“在新窗口打开”绕过接管，保留原标签。Windows 11 原生标签不在该功能范围内。

复制与移动直接通过普通中键菜单顶部的两个文字按钮选择，没有单独开关，也不注册 Ctrl / Shift 中键手势。菜单显示前校验前台和原视图，不依赖鼠标是否位于选中项目上，从 `IFolderView2::GetSelection(FALSE)` 克隆绝对 PIDL 数组；无选择时隐藏操作按钮与数量，有选择但无法生成有效文件快照时显示数量并禁用操作按钮；空白处点击沿用当前选择。按钮点击通过根分支的 `invoke` 回调原位切换模式，保留同一个窗口和焦点，清理旧子菜单；选中目标目录才返回执行命令。不能把无选择解释为当前目录，也不能在选好目标后重读另一批选择。通过 `IFileOperation::CopyItems` / `MoveItems` 批量提交，保留系统进度、错误、冲突和撤销界面；提交前再次核对中键总开关与菜单请求代次，弹出菜单的锁不得覆盖文件操作耗时。

菜单图标和子目录读取应保持异步、按需及有界。网络目录或 Shell 提供程序响应缓慢时，不能阻塞资源管理器菜单枚举，也不能让已关闭的中键菜单长期占用下一次打开的机会。

### 界面与输入

共享主题、DPI 和窗口定位实现。主题需兼容浅色、深色、跟随系统及高对比度；界面里的版本号来自生成文件，不能手写另一个版本值。输入处理只消费属于本功能的手势，文件列表正常滚动和其他应用输入应保留。标准打开 / 保存对话框跳转必须核对地址栏，不能提交“打开 / 保存”按钮或覆盖文件名控件。跨线程激活后先通过有界 `WM_NULL` 消息等待目标处理激活通知，再核对前台；使用 `WM_NEXTDLGCTL` 请求原对话框聚焦已验证的地址框，不发送可能被其他应用注册的全局 `Alt+D`。写入路径前后都校验原窗口、进程、线程、请求及地址焦点，回车只发给地址控件。旧式浏览文件夹对话框只发送目录选择消息，不能点击“确定”，不能按树节点显示名称推测完整路径。

旧式窗口需匹配经典控件 ID 组合，或 namespace 宿主与树控件组合，并检查确认 / 取消按钮；不能把任意含树控件的普通对话框当作文件夹浏览器。跨进程路径数据由现有后台组件处理，沿用前台窗口、进程身份、功能开关与完整性级别检查。路径缓冲在消息完成回调后释放，超时不能视为消息已经停止执行。菜单提交成功只表示请求已受理，不保证目标应用的根目录或过滤规则接受该路径。

## 测试与实机验证

当前工作版本的 CTest 注册了 21 组测试（1.8.7 为 19 组）。`QuickDesktopMenuTests` 使用生产菜单模型和窗口控制器，验证复制 / 移动文字按钮原位切换、取消、禁用、鼠标与键盘操作、三档缩放和目标命令；`QuickRuntimeTests` 覆盖桌面识别以及旧修饰键手势不再被拦截；`QuickTransferTests` 覆盖当前视图选择、空选择无父目录回退、快照、系统接口参数和取消，并在临时目录实际复制 / 移动中文、多选及嵌套文件夹。测试通过隐藏窗口、临时目录和进程内注册表重定向隔离状态，不应修改真实置顶、历史、功能开关或安装注册。需要新增安装或注册测试时，沿用现有隔离夹具，不要把实际 `Install.exe --quiet` 当成普通单元测试步骤。

| 改动范围 | 优先运行的测试 |
| --- | --- |
| 备注与刷新 | `CoreTests`、`ExplorerRefreshTests` |
| 右键菜单和多选 | `ShellTests`、`ManifestTests` |
| 安装、注册和卸载 | `NavigationRegistrationTests`、`UninstallTests` |
| 双击导航与扩展生命周期 | `NavigationTests`、`NavigationExtensionTests`、`NavigationViewTests` |
| 中键菜单和记录 | `QuickFoldersTests`、`QuickRuntimeTests`、`QuickDesktopMenuTests`、`QuickTransferTests`、`LegacyFolderDialogTests`、`QuickMenuGateTests`、`QuickPopupTests` |
| 标签栏 | `ExplorerTabsTests`、`NavigationViewTests`、`NavigationExtensionTests` |
| 主题、设置与图标 | `ThemeControlsTests`、`SettingsHubTests`、`QuickIconTests`、`WindowPositionTests` |

部分测试夹具保留在系统临时目录的 `FolderNote-Test-*` 下，便于检查。`QuickDialogProbe` 和 `TabsActivationProbe` 是额外诊断程序，不是全部验收的替代品。`QuickDialogProbe --verify` 显示自己的系统保存对话框，等待前台后验证目录实际变化和文件名保留，并自动取消；它会影响前台焦点，应单独运行，不放入常规 CTest。无参数模式保留窗口供人工验证，不写入文件。`TabsActivationProbe` 默认读取当前 Shell 窗口和标签栏状态；不要在自动测试中使用会改变真实窗口的激活操作。

实机验收至少记录系统版本、程序版本、是否加载旧 DLL、缩放比例和主题。发布前按改动覆盖安装 / 升级 / 卸载、连续菜单开关、多选、普通目录与慢速路径、背景标签关闭、滚轮快速切换及真实文件对话框跳转。安装器不会强制重启资源管理器；若旧窗口仍加载旧扩展，应在没有复制或移动任务时关闭相关窗口并重新打开，必要时由使用者安排重启资源管理器。

1.8.6 已在 Windows 10 22H2 完成 Release 编译和 18 组自动测试，并验证新资源管理器窗口加载新版标签栏。Windows 11 24H2/25H2、U 盘 / NAS、拒绝访问 ACL、只读介质、全部 DPI / 主题及真实文件对话框等场景仍需相应实机验收。不要仅凭编译、测试桩或 MSIX 清单校验声称这些场景已完成。

1.8.7 已完成无警告 Release 全量构建，19 组 CTest 全部通过（14.71 秒）。`LegacyFolderDialogTests` 在真实隐藏的 64 位及 32 位目标上各通过 39 项断言，覆盖经典 / 可调整大小样式、跨进程中文 / 空格 / Emoji 路径、重复跳转、不自动确认、过期请求和权限校验回调拒绝。权限回调拒绝是隔离测试，不代表已经验证实际提权安装器；第三方应用的权限、根目录与过滤规则仍需对应实机验证。

跨位数回归使用单独的 32 位夹具，不改变产品的 x64 构建目标：

```powershell
cmake -S . -B build-x86 -A Win32
cmake --build build-x86 --config Release --target LegacyDialogFixture
.\build\Release\LegacyFolderDialogTests.exe .\build-x86\Release\LegacyDialogFixture.exe
```

这些隔离夹具结果不能视为所有应用、Windows 11 24H2/25H2 或管理员安装器的兼容性验收。

## 版本与打包

版本号只在 `CMakeLists.txt` 的 `project(FolderNote VERSION ...)` 中维护。重新配置后，CMake 生成窗口版本文字、资源版本及 MSIX 清单版本。发布时同时更新 [版本记录](../CHANGELOG.md) 和受影响的说明；不要直接编辑 `build/generated/` 下的生成文件。

### 本地生成安装包

完成 Release 构建和测试后：

```powershell
.\packaging\build-release.ps1
```

脚本在 `dist` 生成通用版、Windows 11 首层菜单测试版和 SHA-256 校验清单。通用版完整解压后运行 `Install.exe`，首层菜单测试版运行 `Install-Win11.exe`。

### 首次发布到 GitHub

在 GitHub 建立一个公开的空仓库，以便程序无需登录即可读取 Releases。现有 `origin` 可继续用于 NAS。在项目目录审阅 `git status` 和提交内容后，执行以下命令，将 `OWNER` 替换为自己的 GitHub 用户名或组织名。

```powershell
git add .github packaging src tests docs CMakeLists.txt README.md CHANGELOG.md
git diff --cached --stat
git diff --cached
git commit -m "Release FolderNote 1.9.2"
git remote add github https://github.com/OWNER/FolderNote.git
git push github main
git tag -a v1.9.2 -m "FolderNote 1.9.2"
git push github v1.9.2
```

推送 `main` 会在 Actions 生成构建产物，推送与 CMake 版本一致的 `v<版本>` 标签后会自动发布 Releases。CI 将当前 GitHub 仓库写入发布版，设置窗口便能检查后续发布。本地若要启用同一更新源，构建前用 `cmake --preset windows-x64 -DFOLDERNOTE_UPDATE_REPOSITORY=OWNER/FolderNote` 配置。升级时从 Releases 下载与你当前安装类型相同的包。

### Windows 11 首层菜单测试包

```powershell
.\packaging\build-win11.ps1
```

脚本读取 `build/AppxManifest.xml` 和 `build/Release/`，检查主程序与清单版本一致，调用 SDK 中的 `MakeAppx.exe` 校验并打包，输出 `dist/FolderNote-Win11-<版本>/` 及 `dist/FolderNote-<版本>-win11-test.zip`。因此运行脚本前应使用上面的默认 `build/` 目录完成 Release 构建。

目前 MSIX 为未签名的个人测试包。MakeAppx 成功只说明包结构通过校验；正式分发需要准备签名，并在目标 Windows 11 系统验证安装、菜单分组和卸载。更多安装限制见 [Windows 11 说明](../packaging/README-Win11.md)。

不要提交编译输出、发布 ZIP、机器专用配置、证书私钥、访问令牌或本机 `desktop.ini`。在发布记录中区分自动测试结果和实际手工验证结果。
