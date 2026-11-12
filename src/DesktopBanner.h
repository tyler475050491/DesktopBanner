//==============================================================================
//  DesktopBanner.h —— 桌面滚动横幅动态库 · 公开接口
//  版本 : 1.0.0
//  平台 : Windows 7 及以上 (x64 / x86 均可重编译)
//
//  功能 : 在屏幕(或工作区)的顶端/底端创建一条"半透明磨砂背景 + 横向滚动文字"的横幅。
//         横幅为一个 WS_EX_LAYERED(分层, 逐像素 alpha) + WS_EX_TRANSPARENT(鼠标穿透)
//         的无边框置顶窗口, 鼠标左键/右键/双击/滚轮/拖拽全部穿透到下层窗口,
//         不会抢焦点、不占任务栏、不出现在 Alt+Tab, 完全不影响桌面与其它程序的正常操作。
//
//  调用方式
//    1) 静态链接 : 工程里链接 bin\DesktopBanner.lib, 并定义宏 DB_USE_IMPORT_LIB
//    2) 动态调用 : LoadLibrary + GetProcAddress (示例程序 BannerDemo.exe 用的是这种方式)
//
//  本接口为纯 C 接口, 不跨 DLL 传递 C++ 对象, C / C++ / C#(P/Invoke) / Python(ctypes)
//  都可以直接调用。
//==============================================================================
#ifndef DESKTOP_BANNER_H
#define DESKTOP_BANNER_H

#include <windows.h>

#if defined(DB_BUILD_DLL)
#  define DB_API __declspec(dllexport)      // 编译 DLL 时
#elif defined(DB_USE_IMPORT_LIB)
#  define DB_API __declspec(dllimport)      // 静态链接导入库时
#else
#  define DB_API                            // 动态 GetProcAddress 时不需要
#endif

#ifndef DB_CALL
#  define DB_CALL __stdcall
#endif

// 版本号: 主版本*0x10000 + 次版本*0x100 + 修订
#define DB_VERSION          0x00010000
#define DB_VERSION_STRING   "1.0.0"

//------------------------------------------------------------------------------
// 返回码 (Banner_GetLastError 的返回值)
//------------------------------------------------------------------------------
#define DB_OK               0    // 成功
#define DB_ERR_PARAM        1    // 参数无效(空指针 / cbSize 不符 / 文案为空)
#define DB_ERR_THREAD       2    // 创建工作线程失败
#define DB_ERR_WINDOW       3    // 创建横幅窗口或绘图缓冲失败
#define DB_ERR_TIMEOUT      4    // 关闭时等待渲染线程退出超时
#define DB_ERR_UNKNOWN      5    // 其它未知错误

//------------------------------------------------------------------------------
// 横幅停靠位置 (BannerOptions::anchor)
//------------------------------------------------------------------------------
#define DB_ANCHOR_SCREEN_TOP     0   // 屏幕顶端 (y = 0, 会覆盖任务栏区域)
#define DB_ANCHOR_SCREEN_BOTTOM  1   // 屏幕底端
#define DB_ANCHOR_WORK_TOP       2   // 工作区顶端 (任务栏之外的区域, 推荐)
#define DB_ANCHOR_WORK_BOTTOM    3   // 工作区底端
#define DB_ANCHOR_CUSTOM         4   // 自定义坐标, 使用 x / y

//------------------------------------------------------------------------------
// 配置结构体 (Banner_InitEx 使用)
//   建议先调用 Banner_GetDefaultOptions() 取一份默认值, 再按需覆盖字段。
//   零初始化的结构体会导致 bgAlpha = 0(背景完全透明), 请注意。
//------------------------------------------------------------------------------
#pragma pack(push, 8)
typedef struct BannerOptions
{
    DWORD     cbSize;      // [必须] sizeof(BannerOptions)
    COLORREF  bgColor;     // 背景色 (RGB 宏)
    BYTE      bgAlpha;     // 背景不透明度 0(全透明) ~ 255(不透明), 建议 120~220
    COLORREF  textColor;   // 文字颜色
    int       speed;       // 滚动速度, 单位: 像素/秒; >0 向左滚, <0 向右滚, 0 静止
    int       fontSize;    // 字体像素高度, 例如 26; <=0 用默认 26
    LPCWSTR   fontName;    // 字体名, 例如 L"Microsoft YaHei UI"; NULL 用默认
    int       height;      // 横幅高度(像素); <=0 用默认 48
    int       anchor;      // 停靠位置, 见 DB_ANCHOR_* ; 默认 DB_ANCHOR_SCREEN_TOP
    int       x;           // anchor == DB_ANCHOR_CUSTOM 时的左上角 X
    int       y;           // anchor == DB_ANCHOR_CUSTOM 时的左上角 Y
    int       width;       // 横幅宽度; <=0 表示铺满所在屏幕(或工作区)宽度
    BOOL      topMost;     // TRUE: 始终置顶(压在其它窗口之上); FALSE: 沉到桌面层
    int       gap;         // 两段文字之间的间隔像素; <=0 自动 (字号*6, 至少 120)
    int       frost;       // 磨砂颗粒强度 0~100; 0 表示纯色平涂, 默认 40
    DWORD     reserved[4]; // 保留, 必须为 0
} BannerOptions;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

//==============================================================================
// 版本 / 默认配置
//==============================================================================

// 取版本号, 例如 DB_VERSION 的值 0x00010000
DB_API int DB_CALL Banner_GetVersion(void);

// 填充一份默认配置 (推荐在调用 Banner_InitEx 前先调它)
// 返回值: DB_OK / DB_ERR_PARAM
DB_API int DB_CALL Banner_GetDefaultOptions(BannerOptions* options);

//==============================================================================
// 1. 初始化 —— 背景色 / 透明度 / 文字颜色 / 滚动速度
//==============================================================================

// 最简初始化: 四个参数 + 其余全部使用默认值(顶端、48px 高、微软雅黑 26px、
// 铺满主屏宽度、置顶、磨砂强度 40、文字间隔自动)。
//   bgColor   : 背景色, 例如 RGB(18,20,26)
//   bgAlpha   : 背景不透明度 0~255
//   textColor : 文字颜色, 例如 RGB(96,216,255)
//   speed     : 滚动速度 像素/秒, 正数向左, 负数向右
// 返回: TRUE 成功(此时横幅已显示) / FALSE 失败(可用 Banner_GetLastError 查询原因)
// 说明: 重复调用会先销毁旧横幅再重建, 因此也可用于整体更换高度/位置/置顶方式。
DB_API BOOL DB_CALL Banner_Init(COLORREF bgColor, BYTE bgAlpha, COLORREF textColor, int speed);

// 完整初始化, 参数见 BannerOptions 注释
DB_API BOOL DB_CALL Banner_InitEx(const BannerOptions* options);

//==============================================================================
// 2. 运行中修改样式 —— 背景色 / 透明度 / 文字颜色 / 滚动速度
//==============================================================================

// 一次性修改 背景色 + 透明度 + 文字颜色 + 滚动速度 (最常用)
DB_API void DB_CALL Banner_SetStyle(COLORREF bgColor, BYTE bgAlpha, COLORREF textColor, int speed);

// 只改背景色与透明度 (保持磨砂强度不变)
DB_API void DB_CALL Banner_SetBackground(COLORREF bgColor, BYTE bgAlpha);

// 只改文字颜色
DB_API void DB_CALL Banner_SetTextColor(COLORREF textColor);

// 只改滚动速度 像素/秒 (正数向左, 负数向右, 0 静止)
DB_API void DB_CALL Banner_SetSpeed(int speed);

// 只改字号(像素高度), 立即重绘并重新测量文字宽度
DB_API void DB_CALL Banner_SetFontSize(int pixelHeight);

// 只改磨砂颗粒强度 0~100 (0 = 纯色平涂)
DB_API void DB_CALL Banner_SetFrost(int strength);

//==============================================================================
// 3. 修改文字内容
//==============================================================================

// 设置横幅文字(宽字符)。传 L"" 则只显示背景条。
// 文字变更后滚动位置会复位到最右侧重新开始。
// 返回: TRUE 成功 / FALSE 参数为空
DB_API BOOL DB_CALL Banner_SetTextW(LPCWSTR text);

// 设置横幅文字(UTF-8 窄字符), 内部自动转 UTF-16
DB_API BOOL DB_CALL Banner_SetTextA(LPCSTR utf8Text);

//==============================================================================
// 4. 关闭 / 状态
//==============================================================================

// 关闭横幅并回收渲染线程(同步等待线程退出, 最长 3 秒)。可重复调用。
// 关闭后所有 Set 接口仍可用, 只是修改结果会在下次 Banner_Init 时生效。
DB_API void DB_CALL Banner_Close(void);

// 横幅是否正在显示
DB_API BOOL DB_CALL Banner_IsActive(void);

// 最近一次调用的错误码, 见 DB_OK / DB_ERR_* ; 0 表示无错误
DB_API DWORD DB_CALL Banner_GetLastError(void);

#ifdef __cplusplus
}
#endif

#endif // DESKTOP_BANNER_H
