//==============================================================================
//  BannerDemo.cpp —— DesktopBanner.dll 调用示例 (带控制面板)
//
//  演示内容
//    1) 动态加载 DesktopBanner.dll (LoadLibrary + GetProcAddress)
//    2) Banner_InitEx      : 初始化(背景色 / 透明度 / 文字颜色 / 滚动速度 / 字号 / 高度 / 位置)
//    3) Banner_SetStyle    : 运行中一次性修改 背景色 + 透明度 + 文字颜色 + 滚动速度
//    4) Banner_SetTextW    : 修改文字内容 (另有 SetTextA 演示 UTF-8 窄字符版本)
//    5) Banner_Close       : 关闭横幅
//    6) Banner_IsActive / Banner_GetLastError : 状态与错误码
//
//  日志面板: 天蓝色时间戳 / 白色信息 / 绿色成功 / 红色错误
//==============================================================================
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>

#include <string>
#include <cstdio>
#include <cstdarg>
#include <vector>

#include "DesktopBanner.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

//------------------------------------------------------------------------------
// 控件 ID
//------------------------------------------------------------------------------
enum
{
    IDC_SW_BG = 1001,
    IDC_SW_TX,
    IDC_TB_ALPHA,
    IDC_TB_SPEED,
    IDC_TB_FONT,
    IDC_TB_HEIGHT,
    IDC_CB_ANCHOR,
    IDC_CHK_TOP,
    IDC_EDIT_TEXT,
    IDC_BTN_TEXT,
    IDC_BTN_INIT,
    IDC_BTN_STYLE,
    IDC_BTN_CLOSE,
    IDC_BTN_ACTIVE,
    IDC_LOG,
    IDC_BTN_CLEAR,
    IDC_STATUS,
    IDC_TMR_STATUS
};

//------------------------------------------------------------------------------
// 动态接口表 (用 decltype 直接对齐头文件签名, 头文件改了这里自动跟着改)
//------------------------------------------------------------------------------
struct BannerApi
{
    decltype(&Banner_GetVersion)       GetVersion       = nullptr;
    decltype(&Banner_GetDefaultOptions) GetDefaultOptions = nullptr;
    decltype(&Banner_Init)             Init             = nullptr;
    decltype(&Banner_InitEx)           InitEx           = nullptr;
    decltype(&Banner_SetStyle)         SetStyle         = nullptr;
    decltype(&Banner_SetBackground)    SetBackground    = nullptr;
    decltype(&Banner_SetTextColor)     SetTextColor     = nullptr;
    decltype(&Banner_SetSpeed)         SetSpeed         = nullptr;
    decltype(&Banner_SetFontSize)      SetFontSize      = nullptr;
    decltype(&Banner_SetFrost)         SetFrost         = nullptr;
    decltype(&Banner_SetTextW)         SetTextW         = nullptr;
    decltype(&Banner_SetTextA)         SetTextA         = nullptr;
    decltype(&Banner_Close)            Close            = nullptr;
    decltype(&Banner_IsActive)         IsActive         = nullptr;
    decltype(&Banner_GetLastError)     GetLastErrorCode = nullptr;
};

template <typename T>
static T GetApi(HMODULE h, const char* name)
{
    return reinterpret_cast<T>(::GetProcAddress(h, name));
}

//------------------------------------------------------------------------------
// 全局状态
//------------------------------------------------------------------------------
static HMODULE    g_hDll   = nullptr;
static BannerApi  g_api;
static HWND       g_hwnd   = nullptr;
static bool       g_richOk = false;
static int        g_dpi    = 96;

static HWND g_hSwBg = nullptr, g_hSwTx = nullptr;
static HWND g_hTbAlpha = nullptr, g_hTbSpeed = nullptr, g_hTbFont = nullptr, g_hTbHeight = nullptr;
static HWND g_hCbAnchor = nullptr, g_hChkTop = nullptr;
static HWND g_hEditText = nullptr;
static HWND g_hLog = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hLblAlpha = nullptr, g_hLblSpeed = nullptr, g_hLblFont = nullptr, g_hLblHeight = nullptr;

static HBRUSH g_brSwBg = nullptr;
static HBRUSH g_brSwTx = nullptr;
static HFONT  g_fontUI = nullptr;
static HFONT  g_fontBold = nullptr;

struct UiConfig
{
    COLORREF bgColor   = RGB(18, 20, 26);
    COLORREF textColor = RGB(96, 216, 255);
    int      alpha     = 175;
    int      speed     = 120;
    int      fontSize  = 26;
    int      height    = 52;
    int      anchor    = DB_ANCHOR_SCREEN_TOP;
    bool     topMost   = true;
    std::wstring text  = L"\u2605 \u684c\u9762\u6eda\u52a8\u6a2a\u5e45 1.0 \u00b7 \u80cc\u666f\u534a\u900f\u660e\u78e8\u7802 \u00b7 \u9f20\u6807\u7a7f\u900f\u4e0d\u6321\u64cd\u4f5c \u00b7 \u80cc\u666f\u8272/\u900f\u660e\u5ea6/\u6587\u5b57\u989c\u8272/\u5b57\u53f7/\u901f\u5ea6\u5747\u53ef\u5728\u7ebf\u4fee\u6539 \u2605";
};

static UiConfig g_cfg;

//------------------------------------------------------------------------------
// 日志面板: 天蓝色时间戳 + 分级颜色
//------------------------------------------------------------------------------
enum LogLevel { LOG_INFO = 0, LOG_OK = 1, LOG_ERR = 2, LOG_WARN = 3 };

static void LogAppend(const std::wstring& s, COLORREF cr, bool bold)
{
    if (!g_hLog) return;

    if (g_richOk)
    {
        CHARFORMAT2W cf{};
        cf.cbSize     = sizeof(cf);
        cf.dwMask     = CFM_COLOR | CFM_BOLD;
        cf.crTextColor = cr;
        cf.dwEffects  = bold ? CFE_BOLD : 0;

        ::SendMessageW(g_hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        ::SendMessageW(g_hLog, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        ::SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
        ::SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
    }
    else
    {
        std::wstring out = s;
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i] == L'\r') out[i] = L'\n';
        ::SendMessageW(g_hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        ::SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)out.c_str());
    }
}

static void Log(LogLevel level, const wchar_t* fmt, ...)
{
    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    wchar_t ts[48]{};
    swprintf_s(ts, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    LogAppend(ts, RGB(0, 200, 255), false);

    wchar_t body[2048]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    COLORREF cr = RGB(215, 219, 225);
    bool bold = false;
    switch (level)
    {
    case LOG_OK:  cr = RGB(96, 220, 140); break;
    case LOG_ERR: cr = RGB(255, 92, 92);  bold = true; break;
    case LOG_WARN: cr = RGB(255, 190, 90); break;
    default: break;
    }

    LogAppend(std::wstring(body) + L"\r", cr, bold);
}

//------------------------------------------------------------------------------
// 小工具
//------------------------------------------------------------------------------
static int Px(int v) { return ::MulDiv(v, g_dpi, 96); }

// 诊断日志: 写到 %TEMP%\banner_demo_dbg.log, 方便定位控件/DLL 创建失败
static void DbgWrite(const wchar_t* fmt, ...)
{
    wchar_t path[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"banner_demo_dbg.log");

    wchar_t body[1024]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t line[1200]{};
    swprintf_s(line, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f)
    {
        fwprintf_s(f, L"%s", line);
        fclose(f);
    }
}

static COLORREF ContrastColor(COLORREF c)
{
    const int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
    return (lum > 140) ? RGB(24, 24, 28) : RGB(245, 245, 245);
}

static void RefreshSwatchBrushes()
{
    if (g_brSwBg) { ::DeleteObject(g_brSwBg); g_brSwBg = nullptr; }
    if (g_brSwTx) { ::DeleteObject(g_brSwTx); g_brSwTx = nullptr; }
    g_brSwBg = ::CreateSolidBrush(g_cfg.bgColor);
    g_brSwTx = ::CreateSolidBrush(g_cfg.textColor);

    if (g_hSwBg) ::InvalidateRect(g_hSwBg, nullptr, TRUE);
    if (g_hSwTx) ::InvalidateRect(g_hSwTx, nullptr, TRUE);
}

static HWND MakeCtrl(LPCWSTR cls, LPCWSTR text, DWORD style,
                     int x, int y, int w, int h, int id)
{
    if (id < 0) id = 0;   // 纯标签不需要 ID, 避免 -1 落到 HMENU 上
    HWND hwndCtl = ::CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                               Px(x), Px(y), Px(w), Px(h),
                               g_hwnd, (HMENU)(INT_PTR)id, ::GetModuleHandleW(nullptr), nullptr);
    if (hwndCtl && g_fontUI) ::SendMessageW(hwndCtl, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
    return hwndCtl;
}

static void UpdateValueLabels()
{
    wchar_t b[64];
    if (g_hLblAlpha)
    {
        swprintf_s(b, L"%d/255", g_cfg.alpha);
        ::SetWindowTextW(g_hLblAlpha, b);
    }
    if (g_hLblSpeed)
    {
        swprintf_s(b, L"%+d px/s", g_cfg.speed);
        ::SetWindowTextW(g_hLblSpeed, b);
    }
    if (g_hLblFont)
    {
        swprintf_s(b, L"%d px", g_cfg.fontSize);
        ::SetWindowTextW(g_hLblFont, b);
    }
    if (g_hLblHeight)
    {
        swprintf_s(b, L"%d px", g_cfg.height);
        ::SetWindowTextW(g_hLblHeight, b);
    }
}

static void UpdateStatus()
{
    if (!g_hStatus || !g_api.IsActive) return;

    wchar_t b[256];
    if (g_api.IsActive())
    {
        swprintf_s(b, L"状态：运行中   DLL 版本 0x%08X   错误码 %lu",
                   g_api.GetVersion ? g_api.GetVersion() : 0u,
                   g_api.GetLastErrorCode ? g_api.GetLastErrorCode() : 0ul);
    }
    else
    {
        swprintf_s(b, L"状态：未运行   DLL 版本 0x%08X",
                   g_api.GetVersion ? g_api.GetVersion() : 0u);
    }
    ::SetWindowTextW(g_hStatus, b);
}

//------------------------------------------------------------------------------
// 调用接口
//------------------------------------------------------------------------------
static void CallInit()
{
    if (!g_api.InitEx) return;

    BannerOptions opt{};
    if (g_api.GetDefaultOptions) g_api.GetDefaultOptions(&opt);

    opt.bgColor   = g_cfg.bgColor;
    opt.bgAlpha   = (BYTE)g_cfg.alpha;
    opt.textColor = g_cfg.textColor;
    opt.speed     = g_cfg.speed;
    opt.fontSize  = g_cfg.fontSize;
    opt.height    = g_cfg.height;
    opt.anchor    = g_cfg.anchor;
    opt.topMost   = g_cfg.topMost ? TRUE : FALSE;

    DWORD t0 = ::GetTickCount();
    BOOL  ok = g_api.InitEx(&opt);
    DWORD cost = ::GetTickCount() - t0;

    if (ok)
        Log(LOG_OK, L"Banner_InitEx() = TRUE  (耗时 %lu ms)  背景 RGB(%u,%u,%u) alpha=%u  文字 RGB(%u,%u,%u)  速度 %+d px/s  字号 %d  高度 %d%s",
            cost, GetRValue(opt.bgColor), GetGValue(opt.bgColor), GetBValue(opt.bgColor), opt.bgAlpha,
            GetRValue(opt.textColor), GetGValue(opt.textColor), GetBValue(opt.textColor),
            opt.speed, opt.fontSize, opt.height,
            opt.topMost ? L"  置顶" : L"  沉底");
    else
        Log(LOG_ERR, L"Banner_InitEx() = FALSE  错误码 %lu", g_api.GetLastErrorCode ? g_api.GetLastErrorCode() : 0ul);

    if (ok && g_api.SetTextW)
    {
        BOOL tok = g_api.SetTextW(g_cfg.text.c_str());
        Log(tok ? LOG_OK : LOG_ERR, L"Banner_SetTextW() = %s  文字长度 %d 字",
            tok ? L"TRUE" : L"FALSE", (int)g_cfg.text.size());
    }

    UpdateStatus();
}

static DWORD g_lastStyleLog = 0;
static bool  g_styleLogPend = false;

static void CallSetStyle(bool forceLog)
{
    if (!g_api.SetStyle) return;

    g_api.SetStyle(g_cfg.bgColor, (BYTE)g_cfg.alpha, g_cfg.textColor, g_cfg.speed);
    if (g_api.SetFontSize) g_api.SetFontSize(g_cfg.fontSize);

    DWORD now = ::GetTickCount();
    if (forceLog || now - g_lastStyleLog > 400)
    {
        g_lastStyleLog = now;
        Log(LOG_INFO, L"Banner_SetStyle(背景 RGB(%u,%u,%u), alpha=%u, 文字 RGB(%u,%u,%u), 速度 %+d) + Banner_SetFontSize(%d)",
            GetRValue(g_cfg.bgColor), GetGValue(g_cfg.bgColor), GetBValue(g_cfg.bgColor), g_cfg.alpha,
            GetRValue(g_cfg.textColor), GetGValue(g_cfg.textColor), GetBValue(g_cfg.textColor),
            g_cfg.speed, g_cfg.fontSize);
    }
    else
    {
        g_styleLogPend = true;
    }
}

static void CallSetText()
{
    if (!g_api.SetTextW) return;

    wchar_t buf[1024]{};
    ::GetWindowTextW(g_hEditText, buf, _countof(buf));

    g_cfg.text = buf;
    BOOL ok = g_api.SetTextW(g_cfg.text.c_str());
    Log(ok ? LOG_OK : LOG_ERR, L"Banner_SetTextW() = %s  \"%s\"",
        ok ? L"TRUE" : L"FALSE", g_cfg.text.c_str());

    // 顺便演示 UTF-8 窄字符版本 (Banner_SetTextA)
    if (ok && g_api.SetTextA)
    {
        char utf8[1024]{};
        if (::WideCharToMultiByte(CP_UTF8, 0, g_cfg.text.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr) > 0)
        {
            BOOL ok2 = g_api.SetTextA(utf8);
            Log(ok2 ? LOG_OK : LOG_ERR, L"Banner_SetTextA(UTF-8) = %s  相同文案走窄字符接口",
                ok2 ? L"TRUE" : L"FALSE");
        }
    }
}

static void CallClose()
{
    if (!g_api.Close) return;
    DWORD t0 = ::GetTickCount();
    g_api.Close();
    Log(LOG_OK, L"Banner_Close() 完成 (耗时 %lu ms), Banner_IsActive() = %s",
        ::GetTickCount() - t0,
        (g_api.IsActive && g_api.IsActive()) ? L"TRUE" : L"FALSE");
    UpdateStatus();
}

static void CallQuery()
{
    if (!g_api.IsActive) return;
    BOOL act = g_api.IsActive();
    Log(LOG_INFO, L"Banner_IsActive() = %s, Banner_GetLastError() = %lu",
        act ? L"TRUE" : L"FALSE",
        g_api.GetLastErrorCode ? g_api.GetLastErrorCode() : 0ul);
    UpdateStatus();
}

//------------------------------------------------------------------------------
// 窗口过程
//------------------------------------------------------------------------------
static void InitializeUi(HWND hwnd);

LRESULT CALLBACK DemoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_hwnd = hwnd;
        InitializeUi(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC:
    {
        HWND h = (HWND)lp;
        if (h == g_hSwBg && g_brSwBg)
        {
            HDC dc = (HDC)wp;
            ::SetBkColor(dc, g_cfg.bgColor);
            ::SetTextColor(dc, ContrastColor(g_cfg.bgColor));
            return (LRESULT)g_brSwBg;
        }
        if (h == g_hSwTx && g_brSwTx)
        {
            HDC dc = (HDC)wp;
            ::SetBkColor(dc, g_cfg.textColor);
            ::SetTextColor(dc, ContrastColor(g_cfg.textColor));
            return (LRESULT)g_brSwTx;
        }
        break;
    }

    case WM_HSCROLL:
    {
        HWND h = (HWND)lp;
        if (h == g_hTbAlpha)
            g_cfg.alpha = (int)::SendMessageW(h, TBM_GETPOS, 0, 0);
        else if (h == g_hTbSpeed)
            g_cfg.speed = (int)::SendMessageW(h, TBM_GETPOS, 0, 0);
        else if (h == g_hTbFont)
            g_cfg.fontSize = (int)::SendMessageW(h, TBM_GETPOS, 0, 0);
        else if (h == g_hTbHeight)
            g_cfg.height = (int)::SendMessageW(h, TBM_GETPOS, 0, 0);

        UpdateValueLabels();

        if (h == g_hTbHeight)
        {
            Log(LOG_WARN, L"横幅高度改为 %d px —— 高度/位置属于窗口几何, 点[① 初始化/重建] 生效", g_cfg.height);
        }
        else
        {
            CallSetStyle(false);
            UpdateStatus();
        }
        return 0;
    }

    case WM_COMMAND:
    {
        const int id = (int)LOWORD(wp);
        switch (id)
        {
        case IDC_SW_BG:
        {
            COLORREF rgb[16]{};
            rgb[0] = g_cfg.bgColor;
            CHOOSECOLORW cc{};
            cc.hwndOwner      = hwnd;
            cc.lpCustColors   = rgb;
            cc.rgbResult      = g_cfg.bgColor;
            cc.Flags          = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
            if (::ChooseColorW(&cc))
            {
                g_cfg.bgColor = cc.rgbResult;
                RefreshSwatchBrushes();
                CallSetStyle(true);
                UpdateStatus();
            }
            return 0;
        }

        case IDC_SW_TX:
        {
            COLORREF rgb[16]{};
            rgb[0] = g_cfg.textColor;
            CHOOSECOLORW cc{};
            cc.hwndOwner    = hwnd;
            cc.lpCustColors = rgb;
            cc.rgbResult    = g_cfg.textColor;
            cc.Flags        = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
            if (::ChooseColorW(&cc))
            {
                g_cfg.textColor = cc.rgbResult;
                RefreshSwatchBrushes();
                CallSetStyle(true);
                UpdateStatus();
            }
            return 0;
        }

        case IDC_CB_ANCHOR:
            g_cfg.anchor = (int)::SendMessageW(g_hCbAnchor, CB_GETCURSEL, 0, 0);
            if (g_cfg.anchor < 0) g_cfg.anchor = DB_ANCHOR_SCREEN_TOP;
            Log(LOG_INFO, L"停靠位置改为 %d (0=屏幕顶端 1=屏幕底端 2=工作区顶端 3=工作区底端)", g_cfg.anchor);
            return 0;

        case IDC_CHK_TOP:
            g_cfg.topMost = (::SendMessageW(g_hChkTop, BM_GETCHECK, 0, 0) == BST_CHECKED);
            Log(LOG_INFO, L"置顶显示 = %s (需点[① 初始化/重建] 生效)", g_cfg.topMost ? L"TRUE" : L"FALSE");
            return 0;

        case IDC_BTN_INIT:   CallInit();      return 0;
        case IDC_BTN_STYLE:  CallSetStyle(true); UpdateStatus(); return 0;
        case IDC_BTN_TEXT:   CallSetText();   return 0;
        case IDC_BTN_CLOSE:  CallClose();     return 0;
        case IDC_BTN_ACTIVE: CallQuery();     return 0;

        case IDC_BTN_CLEAR:
            if (g_hLog) ::SetWindowTextW(g_hLog, L"");
            return 0;

        default:
            break;
        }
        break;
    }

    case WM_TIMER:
        if (wp == IDC_TMR_STATUS)
        {
            if (g_styleLogPend && ::GetTickCount() - g_lastStyleLog > 400)
            {
                g_styleLogPend = false;
                g_lastStyleLog = ::GetTickCount();
                Log(LOG_INFO, L"Banner_SetStyle(背景 RGB(%u,%u,%u), alpha=%u, 文字 RGB(%u,%u,%u), 速度 %+d) + Banner_SetFontSize(%d)  [拖动结束]",
                    GetRValue(g_cfg.bgColor), GetGValue(g_cfg.bgColor), GetBValue(g_cfg.bgColor), g_cfg.alpha,
                    GetRValue(g_cfg.textColor), GetGValue(g_cfg.textColor), GetBValue(g_cfg.textColor),
                    g_cfg.speed, g_cfg.fontSize);
            }
            UpdateStatus();
        }
        return 0;

    case WM_CLOSE:
        ::KillTimer(hwnd, IDC_TMR_STATUS);
        CallClose();
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_brSwBg) { ::DeleteObject(g_brSwBg); g_brSwBg = nullptr; }
        if (g_brSwTx) { ::DeleteObject(g_brSwTx); g_brSwTx = nullptr; }
        if (g_fontUI) { ::DeleteObject(g_fontUI); g_fontUI = nullptr; }
        if (g_fontBold) { ::DeleteObject(g_fontBold); g_fontBold = nullptr; }
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

//------------------------------------------------------------------------------
// 建界面
//------------------------------------------------------------------------------
static void InitializeUi(HWND hwnd)
{
    const DWORD LBL = SS_LEFT;

    ::CreateWindowExW(0, L"BUTTON", L"① 横幅参数与样式（接口 1 / 2）",
                      BS_GROUPBOX | WS_CHILD | WS_VISIBLE,
                      Px(8), Px(8), Px(784), Px(180), hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    // ---- 第 1 行: 背景色 / 文字颜色 / 透明度 / 位置
    MakeCtrl(L"STATIC", L"背景色", LBL, 20, 40, 54, 20, -1);
    g_hSwBg = MakeCtrl(L"STATIC", L"背景", SS_NOTIFY | SS_CENTER, 76, 34, 66, 28, IDC_SW_BG);

    MakeCtrl(L"STATIC", L"文字颜色", LBL, 152, 40, 56, 20, -1);
    g_hSwTx = MakeCtrl(L"STATIC", L"文字", SS_NOTIFY | SS_CENTER, 210, 34, 66, 28, IDC_SW_TX);

    MakeCtrl(L"STATIC", L"透明度", LBL, 288, 40, 44, 20, -1);
    g_hTbAlpha = MakeCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
                          336, 32, 170, 28, IDC_TB_ALPHA);
    g_hLblAlpha = MakeCtrl(L"STATIC", L"", LBL, 512, 40, 50, 20, -1);

    MakeCtrl(L"STATIC", L"位置", LBL, 568, 40, 30, 20, -1);
    g_hCbAnchor = MakeCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                           602, 34, 160, 200, IDC_CB_ANCHOR);

    // ---- 第 2 行: 速度 / 字号 / 置顶
    MakeCtrl(L"STATIC", L"滚动速度", LBL, 20, 84, 92, 20, -1);
    g_hTbSpeed = MakeCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
                          114, 76, 190, 28, IDC_TB_SPEED);
    g_hLblSpeed = MakeCtrl(L"STATIC", L"", LBL, 308, 84, 64, 20, -1);

    MakeCtrl(L"STATIC", L"字号", LBL, 376, 84, 30, 20, -1);
    g_hTbFont = MakeCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
                         408, 76, 130, 28, IDC_TB_FONT);
    g_hLblFont = MakeCtrl(L"STATIC", L"", LBL, 542, 84, 44, 20, -1);

    g_hChkTop = MakeCtrl(L"BUTTON", L"始终置顶", BS_AUTOCHECKBOX,
                         598, 82, 104, 24, IDC_CHK_TOP);

    // ---- 第 3 行: 高度 / 状态
    MakeCtrl(L"STATIC", L"横幅高度", LBL, 20, 128, 60, 20, -1);
    g_hTbHeight = MakeCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
                           86, 120, 160, 28, IDC_TB_HEIGHT);
    g_hLblHeight = MakeCtrl(L"STATIC", L"", LBL, 250, 128, 50, 20, -1);

    MakeCtrl(L"STATIC", L"状态", LBL, 314, 128, 30, 20, -1);
    g_hStatus = MakeCtrl(L"STATIC", L"", LBL, 350, 124, 420, 24, IDC_STATUS);
    if (g_fontBold) ::SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

    // ---- ② 文字
    ::CreateWindowExW(0, L"BUTTON", L"② 文字内容（接口 3）",
                      BS_GROUPBOX | WS_CHILD | WS_VISIBLE,
                      Px(8), Px(196), Px(784), Px(108), hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    MakeCtrl(L"STATIC", L"文字内容", LBL, 20, 220, 60, 20, -1);
    g_hEditText = MakeCtrl(L"EDIT", g_cfg.text.c_str(),
                           WS_BORDER | ES_AUTOHSCROLL, 86, 214, 540, 26, IDC_EDIT_TEXT);
    MakeCtrl(L"BUTTON", L"设置文字 (SetTextW)", BS_PUSHBUTTON, 634, 212, 140, 30, IDC_BTN_TEXT);

    MakeCtrl(L"STATIC",
             L"接口 Banner_SetTextW(宽字符) / Banner_SetTextA(UTF-8 窄字符) · 支持中文与符号 · 文字变更后自动从最右侧重新滚动",
             LBL, 20, 256, 750, 40, -1);

    // ---- ③ 操作
    ::CreateWindowExW(0, L"BUTTON", L"③ 操作",
                      BS_GROUPBOX | WS_CHILD | WS_VISIBLE,
                      Px(8), Px(312), Px(784), Px(60), hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    MakeCtrl(L"BUTTON", L"初始化 / 重建 (InitEx)", BS_PUSHBUTTON, 20, 322, 168, 36, IDC_BTN_INIT);
    MakeCtrl(L"BUTTON", L"应用样式 (SetStyle)", BS_PUSHBUTTON, 198, 322, 152, 36, IDC_BTN_STYLE);
    MakeCtrl(L"BUTTON", L"关闭横幅 (Close)", BS_PUSHBUTTON, 360, 322, 136, 36, IDC_BTN_CLOSE);
    MakeCtrl(L"BUTTON", L"查询状态 (IsActive)", BS_PUSHBUTTON, 508, 322, 140, 36, IDC_BTN_ACTIVE);
    MakeCtrl(L"BUTTON", L"清空日志", BS_PUSHBUTTON, 660, 322, 116, 36, IDC_BTN_CLEAR);

    // ---- ④ 日志
    ::CreateWindowExW(0, L"BUTTON", L"④ 调用日志",
                      BS_GROUPBOX | WS_CHILD | WS_VISIBLE,
                      Px(8), Px(380), Px(784), Px(276), hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    g_hLog = ::CreateWindowExW(WS_EX_CLIENTEDGE,
                               g_richOk ? MSFTEDIT_CLASS : L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                               Px(20), Px(402), Px(700), Px(242),
                               hwnd, (HMENU)(INT_PTR)IDC_LOG, ::GetModuleHandleW(nullptr), nullptr);

    if (!g_hLog)
    {
        // 富文本控件建不出来则退化为普通 EDIT, 保证日志面板始终可用
        DWORD e1 = ::GetLastError();
        g_richOk = false;
        g_hLog = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                   Px(20), Px(402), Px(700), Px(242),
                                   hwnd, (HMENU)(INT_PTR)IDC_LOG, ::GetModuleHandleW(nullptr), nullptr);
        DbgWrite(L"log control fallback: richedit err=%lu, edit=%p err=%lu", e1, (void*)g_hLog, ::GetLastError());
    }
    ::SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fontUI, TRUE);

    if (g_richOk)
    {
        ::SendMessageW(g_hLog, EM_SETBKGNDCOLOR, FALSE, (LPARAM)RGB(24, 26, 30));
        ::SendMessageW(g_hLog, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    }
    else
    {
        ::SendMessageW(g_hLog, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    }

    // 下拉框与滑块初始值
    const wchar_t* items[] = { L"屏幕顶端", L"屏幕底端", L"工作区顶端", L"工作区底端" };
    for (int i = 0; i < 4; ++i)
        ::SendMessageW(g_hCbAnchor, CB_ADDSTRING, 0, (LPARAM)items[i]);
    ::SendMessageW(g_hCbAnchor, CB_SETCURSEL, 0, 0);

    ::SendMessageW(g_hTbAlpha, TBM_SETRANGE, TRUE, MAKELONG(0, 255));
    ::SendMessageW(g_hTbAlpha, TBM_SETPOS, TRUE, g_cfg.alpha);

    ::SendMessageW(g_hTbSpeed, TBM_SETRANGE, TRUE, MAKELONG(-400, 400));
    ::SendMessageW(g_hTbSpeed, TBM_SETTICFREQ, 100, 0);
    ::SendMessageW(g_hTbSpeed, TBM_SETPOS, TRUE, g_cfg.speed);

    ::SendMessageW(g_hTbFont, TBM_SETRANGE, TRUE, MAKELONG(12, 72));
    ::SendMessageW(g_hTbFont, TBM_SETTICFREQ, 12, 0);
    ::SendMessageW(g_hTbFont, TBM_SETPOS, TRUE, g_cfg.fontSize);

    ::SendMessageW(g_hTbHeight, TBM_SETRANGE, TRUE, MAKELONG(24, 160));
    ::SendMessageW(g_hTbHeight, TBM_SETTICFREQ, 32, 0);
    ::SendMessageW(g_hTbHeight, TBM_SETPOS, TRUE, g_cfg.height);

    ::SendMessageW(g_hChkTop, BM_SETCHECK, BST_CHECKED, 0);

    RefreshSwatchBrushes();
    UpdateValueLabels();
    UpdateStatus();

    ::SetTimer(hwnd, IDC_TMR_STATUS, 1000, nullptr);
}

//------------------------------------------------------------------------------
// 加载 DLL
//------------------------------------------------------------------------------
static bool LoadBannerDll(std::wstring& err)
{
    wchar_t exe[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);

    std::wstring dir(exe);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos + 1);

    std::wstring dll = dir + L"DesktopBanner.dll";

    g_hDll = ::LoadLibraryW(dll.c_str());
    if (!g_hDll)
    {
        wchar_t b[512];
        swprintf_s(b, L"LoadLibraryW 失败(错误 %lu)。\n\n请确认 DesktopBanner.dll 与本程序在同一目录:\n%s",
                   ::GetLastError(), dll.c_str());
        err = b;
        return false;
    }

    // 使用宏逐项取地址, 避免手写 15 个类型转换
#define LOADAPI(field, name) \
    g_api.field = GetApi<decltype(g_api.field)>(g_hDll, name);

    LOADAPI(GetVersion,        "Banner_GetVersion")
    LOADAPI(GetDefaultOptions, "Banner_GetDefaultOptions")
    LOADAPI(Init,              "Banner_Init")
    LOADAPI(InitEx,            "Banner_InitEx")
    LOADAPI(SetStyle,          "Banner_SetStyle")
    LOADAPI(SetBackground,     "Banner_SetBackground")
    LOADAPI(SetTextColor,      "Banner_SetTextColor")
    LOADAPI(SetSpeed,          "Banner_SetSpeed")
    LOADAPI(SetFontSize,       "Banner_SetFontSize")
    LOADAPI(SetFrost,          "Banner_SetFrost")
    LOADAPI(SetTextW,          "Banner_SetTextW")
    LOADAPI(SetTextA,          "Banner_SetTextA")
    LOADAPI(Close,             "Banner_Close")
    LOADAPI(IsActive,          "Banner_IsActive")
    LOADAPI(GetLastErrorCode,  "Banner_GetLastError")
#undef LOADAPI

    if (!g_api.Init || !g_api.InitEx || !g_api.SetStyle || !g_api.SetTextW || !g_api.Close)
    {
        err = L"DesktopBanner.dll 中缺少必需的导出函数, 请确认 DLL 版本与头文件一致";
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------
// 入口
//------------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    ::SetProcessDPIAware();   // manifest 已声明 PerMonitorV2, 这里只是老系统兜底

    {
        HDC dc = ::GetDC(nullptr);
        if (dc)
        {
            g_dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
            ::ReleaseDC(nullptr, dc);
        }
        if (g_dpi <= 0) g_dpi = 96;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&icc);

    // RichEdit 用于彩色日志, 失败则退化为普通 EDIT
    g_richOk = (::LoadLibraryW(L"Msftedit.dll") != nullptr);

    g_fontUI = ::CreateFontW(-::MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_fontBold = ::CreateFontW(-::MulDiv(9, g_dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    std::wstring err;
    if (!LoadBannerDll(err))
    {
        ::MessageBoxW(nullptr, err.c_str(), L"DesktopBanner Demo", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = DemoProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::CreateSolidBrush(RGB(246, 247, 250));
    wc.lpszClassName = L"DesktopBannerDemoWnd";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(0, L"DesktopBannerDemoWnd",
                                  L"桌面滚动横幅 DLL 调用示例  v1.0",
                                  WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  Px(816), Px(700),
                                  nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    Log(LOG_INFO, L"DesktopBanner.dll 加载成功, 版本 0x%08X", g_api.GetVersion ? g_api.GetVersion() : 0);
    Log(LOG_INFO, L"提示：横幅窗口为 WS_EX_LAYERED|WS_EX_TRANSPARENT, 鼠标点击/拖拽直接穿透到下层, 不会挡住桌面操作");

    CallInit();

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (g_api.Close) g_api.Close();
    if (g_hDll) { ::FreeLibrary(g_hDll); g_hDll = nullptr; }
    return 0;
}
