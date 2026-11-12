# 桌面滚动横幅 DesktopBanner

用 C++ 封装成 Windows 动态库：在屏幕（或工作区）的**顶端 / 底端**画一条
**半透明磨砂背景 + 横向滚动文字**的横幅，并**完全鼠标穿透**——点击、双击、右键、滚轮、拖拽都会直接落到下面的窗口，
不抢焦点、不占任务栏、不进 Alt+Tab，不影响任何桌面操作。

成品（`bin` 目录）：

| 文件 | 说明 |
|---|---|
| `DesktopBanner.dll` | 动态库本体，15 个导出函数，纯 C 接口 |
| `DesktopBanner.lib` | 静态链接用的导入库（可选） |
| `BannerDemo.exe` | 调用示例：控制面板 + 彩色调用日志，把 4 组接口全跑一遍 |

源码与文档：

```
电脑横幅滚动提示\
├─ bin\                    编译产物（dll / lib / demo exe / pdb）
├─ src\
│  ├─ DesktopBanner.h      公开头文件（C 接口 + BannerOptions + 错误码）
│  ├─ DesktopBanner.cpp    动态库实现（渲染线程 / 分层窗口 / 磨砂 / 滚动）
│  ├─ DesktopBanner.def    导出名清单
│  ├─ BannerDemo.cpp       调用示例（LoadLibrary 动态加载 + 控制面板 + 日志）
│  ├─ demo.rc / demo.manifest  示例程序的版本信息与 DPI 清单
├─ doc\
│  ├─ 接口文档.md           每个接口 / 结构体字段 / 错误码 / C# 与 Python 示例
│  └─ 实现与二次开发说明.md  线程模型、渲染管线、磨砂与滚动算法、限制与扩展点
├─ build.bat               一键编译（自动找 VS2022 的 vcvars64）
└─ README.md
```

---

## 一、快速开始

1. 编译（需要 VS2015 以上，本机验证为 VS2022 19.42 / x64）：

```bat
build.bat
```

2. 直接运行示例：

```
bin\BannerDemo.exe
```

程序启动后横幅立即出现在屏幕顶端，面板上可以实时改**背景色 / 透明度 / 文字颜色 / 字号 / 高度 / 位置 / 置顶 / 滚动速度 / 文字内容**，
底部日志会打印每一次接口调用、返回值与耗时。

---

## 二、最小调用示例

### 方式 A：静态链接导入库

工程里加 `src\DesktopBanner.h` 与 `bin\DesktopBanner.lib`，并定义宏 `DB_USE_IMPORT_LIB`：

```cpp
#include "DesktopBanner.h"
#pragma comment(lib, "DesktopBanner.lib")

int main()
{
    // 1) 初始化：背景色 / 透明度(0~255) / 文字颜色 / 滚动速度(像素/秒)
    Banner_Init(RGB(18, 20, 26), 175, RGB(96, 216, 255), 120);

    // 3) 修改文字内容
    Banner_SetTextW(L"★ 桌面滚动横幅 1.0 · 鼠标穿透不挡操作 ★");

    // 2) 运行中改样式（背景色 / 透明度 / 文字颜色 / 速度）
    Sleep(3000);
    Banner_SetStyle(RGB(60, 20, 70), 200, RGB(255, 235, 120), 80);
    Banner_SetFontSize(30);

    Sleep(5000);
    // 4) 关闭
    Banner_Close();
    return 0;
}
```

### 方式 B：LoadLibrary 动态调用（示例程序用的方式）

```cpp
HMODULE dll = ::LoadLibraryW(L"DesktopBanner.dll");
auto pfnInit   = (decltype(&Banner_Init))::GetProcAddress(dll, "Banner_Init");
auto pfnText   = (decltype(&Banner_SetTextW))::GetProcAddress(dll, "Banner_SetTextW");
auto pfnStyle  = (decltype(&Banner_SetStyle))::GetProcAddress(dll, "Banner_SetStyle");
auto pfnClose  = (decltype(&Banner_Close))::GetProcAddress(dll, "Banner_Close");
```

两种方式都不需要宿主有消息循环——DLL 内部自带渲染线程和消息泵。

---

## 三、接口一览

| # | 接口 | 作用 |
|---|---|---|
| 1 | `Banner_Init(bg, alpha, textColor, speed)` | 最简初始化，四个参数即生效 |
| | `Banner_InitEx(&options)` | 完整初始化（字号 / 高度 / 位置 / 置顶 / 磨砂 / 间隔） |
| 2 | `Banner_SetStyle(bg, alpha, textColor, speed)` | 一次性改四项 |
| | `Banner_SetBackground / SetTextColor / SetSpeed / SetFontSize / SetFrost` | 单项修改 |
| 3 | `Banner_SetTextW(text)` / `Banner_SetTextA(utf8)` | 改文字（宽字符 / UTF-8） |
| 4 | `Banner_Close()` | 关闭横幅并回收线程 |
| 附 | `Banner_IsActive()` / `Banner_GetLastError()` / `Banner_GetVersion()` / `Banner_GetDefaultOptions()` | 状态、错误码、版本、默认配置 |

`COLORREF` 就是 `RGB(r,g,b)`；`alpha` 取 0（全透明）~ 255（不透明），推荐 120~220。
`speed` 单位是**像素/秒**，正数向左滚、负数向右滚、0 静止。

完整参数说明见 [doc/接口文档.md](doc/接口文档.md)。

---

## 四、参数怎么调

| 想要的效果 | 怎么调 |
|---|---|
| 更"通透"的磨砂 | `bgAlpha` 调低（100~150）+ `frost` 保持 30~50 |
| 纯色平涂、不要颗粒 | `frost = 0` |
| 文字看不清 | `textColor` 换成高对比色（浅色底配深字），或把 `bgAlpha` 调到 200 以上 |
| 太亮/太抢眼 | `bgAlpha` 降到 90~130，文字色改为柔和的灰白 |
| 只想在本机桌面顶部、不想压住所有窗口 | `topMost = FALSE`（会沉到窗口层，被其它窗口遮挡） |
| 不压住任务栏 | `anchor = DB_ANCHOR_WORK_TOP`（工作区顶端） |

---

## 五、已实测验证（本机 Windows 10 / 1920×1080 / 100% 缩放）

- 横幅窗口 `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST` 全部生效，矩形 `(0,0)-(1920,52)`；
- 鼠标穿透：`WindowFromPoint(500,20)`、`WindowFromPoint(1000,30)` 命中的都是**下层窗口**（浏览器 / 资源管理器），不是横幅，确认点击不会被拦；
- 滚动：间隔 700ms 两次抓屏，顶部 1920×60 区域有 1224 个采样点发生变化，确认文字在动；
- 四个接口：`SetStyle` 原地换色（窗口句柄不变）、`Close` 后窗口句柄消失、`Init/InitEx` 重建（新句柄），日志逐条打印成功/失败；
- 示例程序日志面板：天蓝色时间戳 + 绿色成功 + 红色错误，`Msftedit` 失败会自动退化为普通 EDIT。

## 六、已知限制

1. **不是真正的背景模糊**：Windows 的 DWM 毛玻璃（`ACCENT_ENABLE_BLURBEHIND`）与分层窗口不兼容，所以"磨砂感"是用
   半透明底色 + 轻微垂直渐变 + 稳定颗粒 + 上下高光边模拟的。需要真模糊的做法见 `doc/实现与二次开发说明.md` 第 9 节。
2. 默认只铺**主显示器**；多屏扩展需要改 `ComputeGeometry()`（文档里给了改法）。
3. 横幅是**单行滚动文字**，不支持换行/圆角/图标。
4. 全屏独占的游戏、UAC 安全桌面看不到横幅（UIPI 限制，属正常现象）。
5. 宿主退出前建议调用 `Banner_Close()`；即使不调用，进程结束时窗口也会被系统回收。
