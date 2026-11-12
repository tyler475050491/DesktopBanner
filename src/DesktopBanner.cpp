//==============================================================================
//  DesktopBanner.cpp —— 桌面滚动横幅动态库 · 实现
//
//  实现要点
//    * 横幅窗口 = 独立渲染线程 + WS_POPUP 分层窗口
//      ex 样式: WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
//               WS_EX_NOACTIVATE | WS_EX_TOPMOST
//      WS_EX_TRANSPARENT 让所有鼠标消息直接穿透; WM_NCHITTEST 再返回 HTTRANSPARENT 双保险。
//    * 逐像素 alpha: 自己合成一张 32 位 premultiplied-BGRA 位图, 用 UpdateLayeredWindow 提交,
//      所以"背景半透明 + 文字不透明清晰"可以同时成立(SetLayeredWindowAttributes 做不到)。
//    * 磨砂感: 背景层预生成一次 = 纯色 + 轻微垂直渐变 + 固定随机颗粒 + 上下 1px 高光/阴影边,
//      颗粒基于坐标哈希, 每帧不变, 不会闪烁。
//    * 文字: 先用 GDI 在 32 位"文字蒙版"上画白色抗锯齿文字, 再把蒙版亮度当覆盖率做合成,
//      这样 ClearType 彩色边缘不会跑到输出里。
//    * 滚动: QueryPerformanceCounter 计时间增量算位移, 与定时器精度无关, 速度稳定不抖。
//    * DPI: 只在渲染线程内 SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2),
//      不改变宿主进程的 DPI 感知设置, 拿到的坐标是物理像素, 文字不会糊。
//==============================================================================
#define DB_BUILD_DLL
#include "DesktopBanner.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

//------------------------------------------------------------------------------
// 常量
//------------------------------------------------------------------------------
const wchar_t  kWndClass[]      = L"DoubaoDesktopBannerWnd";
const UINT     WM_DB_REFRESH    = WM_APP + 0x11;   // 外部改了参数, 立即重绘
const UINT     WM_DB_QUIT       = WM_APP + 0x12;   // 关闭横幅
const UINT_PTR kRenderTimerId   = 1;
const UINT       kRenderTimerMs = 16;              // 约 60fps

const COLORREF kDefBgColor   = RGB(18, 20, 26);
const int      kDefBgAlpha   = 170;
const COLORREF kDefTextColor = RGB(96, 216, 255);
const int      kDefSpeed     = 120;
const int      kDefFontSize  = 26;
const int      kDefHeight    = 48;
const int      kDefFrost     = 40;
const wchar_t  kDefFontName[] = L"Microsoft YaHei UI";

inline int  Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline BYTE ClampB(int v)                 { return (BYTE)Clampi(v, 0, 255); }
inline BYTE PremByte(int v, int a)        { return ClampB((v * a + 127) / 255); }

// 2D 坐标哈希, 用于稳定的磨砂颗粒
inline unsigned Hash2D(int x, int y)
{
    unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

//------------------------------------------------------------------------------
// 参数状态 (由宿主线程写, 渲染线程读, 用互斥锁保护)
//------------------------------------------------------------------------------
struct BannerState
{
    std::wstring text    = L"桌面滚动横幅 DesktopBanner";   // 默认文案, 可用 Banner_SetTextW 替换
    COLORREF     bgColor   = kDefBgColor;
    int          bgAlpha   = kDefBgAlpha;
    COLORREF     textColor = kDefTextColor;
    int          speed     = kDefSpeed;
    int          fontSize  = kDefFontSize;
    std::wstring fontName  = kDefFontName;
    int          height    = kDefHeight;
    int          anchor    = DB_ANCHOR_SCREEN_TOP;
    int          x         = 0;
    int          y         = 0;
    int          width     = 0;          // 0 = 铺满
    BOOL         topMost   = TRUE;
    int          gap       = 0;          // 0 = 自动
    int          frost     = kDefFrost;
};

std::mutex   g_stateMutex;
BannerState  g_state;

//------------------------------------------------------------------------------
// 生命周期
//------------------------------------------------------------------------------
struct Geometry { int x = 0, y = 0, w = 0, h = 0; };

struct RenderCtx
{
    HWND      hwnd      = nullptr;
    HDC       hdcScreen = nullptr;

    // 输出帧缓冲 (32bpp 自上而下)
    HBITMAP   bmpFrame    = nullptr;
    HBITMAP   bmpFrameOld = nullptr;
    HDC       hdcFrame    = nullptr;
    void*     bitsFrame   = nullptr;

    // 文字蒙版 (32bpp, 白色文字 / 黑底, 只取亮度当覆盖率)
    HBITMAP   bmpMask     = nullptr;
    HBITMAP   bmpMaskOld  = nullptr;
    HDC       hdcMask     = nullptr;
    void*     bitsMask    = nullptr;

    int       bufW = 0;
    int       bufH = 0;

    // 预生成的背景层 (premultiplied BGRA)
    std::vector<DWORD> bg;

    Geometry  geo;
    COLORREF  curBgColor   = 0xFFFFFFFF;
    int       curBgAlpha   = -1;
    int       curFrost     = -1;

    HFONT     font         = nullptr;
    int       curFontSize  = -1;
    std::wstring curFontName;

    int       textW        = -1;
    std::wstring curText;

    double    advance      = 0.0;
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER lastTick{};
};

struct RenderHost
{
    RenderCtx      ctx;
    HANDLE         readyEvent = nullptr;
    HANDLE         doneEvent  = nullptr;
    int            startResult = DB_OK;
};

std::mutex                  g_lifeMutex;      // 保护线程与窗口句柄
std::shared_ptr<RenderHost> g_host;
std::thread                 g_thread;
std::atomic<HWND>           g_hwnd{ nullptr };
std::atomic<DWORD>          g_renderThreadId{ 0 };
std::atomic<bool>           g_running{ false };
std::atomic<DWORD>          g_lastError{ DB_OK };

void SetLastErrorCode(int code) { g_lastError.store((DWORD)code); }

BannerState SnapshotState()
{
    std::lock_guard<std::mutex> lk(g_stateMutex);
    return g_state;
}

void PostRefresh()
{
    HWND h = g_hwnd.load();
    if (h) ::PostMessageW(h, WM_DB_REFRESH, 0, 0);
}

//------------------------------------------------------------------------------
// 几何计算 (渲染线程内调用, 该线程已被设为 Per-Monitor V2, 取到的是物理像素)
//------------------------------------------------------------------------------
Geometry ComputeGeometry(const BannerState& s)
{
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);

    RECT wa{ 0, 0, sw, sh };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    int h = (s.height > 0) ? s.height : kDefHeight;
    h = Clampi(h, 8, (sh / 2) > 16 ? (sh / 2) : 16);

    Geometry g;
    g.h = h;

    const int waW = wa.right - wa.left;

    switch (s.anchor)
    {
    case DB_ANCHOR_SCREEN_BOTTOM:
        g.x = 0; g.y = sh - h; g.w = (s.width > 0) ? s.width : sw;
        break;

    case DB_ANCHOR_WORK_TOP:
        g.x = wa.left; g.y = wa.top; g.w = (s.width > 0) ? s.width : waW;
        break;

    case DB_ANCHOR_WORK_BOTTOM:
        g.x = wa.left; g.y = wa.bottom - h; g.w = (s.width > 0) ? s.width : waW;
        break;

    case DB_ANCHOR_CUSTOM:
        g.x = s.x; g.y = s.y; g.w = (s.width > 0) ? s.width : sw;
        break;

    case DB_ANCHOR_SCREEN_TOP:
    default:
        g.x = 0; g.y = 0; g.w = (s.width > 0) ? s.width : sw;
        break;
    }

    g.w = Clampi(g.w, 16, sw * 2);
    return g;
}

//------------------------------------------------------------------------------
// 绘图缓冲
//------------------------------------------------------------------------------
void ReleaseBuffers(RenderCtx& c)
{
    if (c.hdcFrame)
    {
        if (c.bmpFrameOld) ::SelectObject(c.hdcFrame, c.bmpFrameOld);
        ::DeleteDC(c.hdcFrame);
        c.hdcFrame = nullptr;
    }
    if (c.bmpFrame)
    {
        ::DeleteObject(c.bmpFrame);
        c.bmpFrame = nullptr;
        c.bmpFrameOld = nullptr;
        c.bitsFrame = nullptr;
    }
    if (c.hdcMask)
    {
        if (c.bmpMaskOld) ::SelectObject(c.hdcMask, c.bmpMaskOld);
        ::DeleteDC(c.hdcMask);
        c.hdcMask = nullptr;
    }
    if (c.bmpMask)
    {
        ::DeleteObject(c.bmpMask);
        c.bmpMask = nullptr;
        c.bmpMaskOld = nullptr;
        c.bitsMask = nullptr;
    }
    c.bufW = c.bufH = 0;
}

bool CreateDibBuffer(HDC hdcRef, int w, int h,
                     HBITMAP* phbm, HBITMAP* pOld, HDC* phdc, void** pbits)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // 负数 = 自上而下
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = ::CreateDIBSection(hdcRef, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits)
    {
        if (hbm) ::DeleteObject(hbm);
        return false;
    }

    HDC hdc = ::CreateCompatibleDC(hdcRef);
    if (!hdc)
    {
        ::DeleteObject(hbm);
        return false;
    }

    *phbm  = hbm;
    *pOld  = (HBITMAP)::SelectObject(hdc, hbm);
    *phdc  = hdc;
    *pbits = bits;
    return true;
}

bool EnsureBuffers(RenderCtx& c, const Geometry& g)
{
    if (c.bufW == g.w && c.bufH == g.h && c.bitsFrame && c.bitsMask)
        return true;

    ReleaseBuffers(c);

    if (!CreateDibBuffer(c.hdcScreen, g.w, g.h, &c.bmpFrame, &c.bmpFrameOld, &c.hdcFrame, &c.bitsFrame))
        return false;
    if (!CreateDibBuffer(c.hdcScreen, g.w, g.h, &c.bmpMask, &c.bmpMaskOld, &c.hdcMask, &c.bitsMask))
    {
        ReleaseBuffers(c);
        return false;
    }

    c.bufW = g.w;
    c.bufH = g.h;
    c.textW = -1;              // 缓冲区换了, 需要重新测量文字
    return true;
}

//------------------------------------------------------------------------------
// 预生成背景层: 纯色 + 垂直渐变 + 固定颗粒 + 上下高光边  → 磨砂玻璃观感
//------------------------------------------------------------------------------
void BuildBackground(RenderCtx& c, const BannerState& s)
{
    const int W = c.bufW, H = c.bufH;
    if (W <= 0 || H <= 0) return;

    c.bg.assign((size_t)W * (size_t)H, 0);

    const int a    = Clampi(s.bgAlpha, 0, 255);
    const int br   = GetRValue(s.bgColor);
    const int bgc  = GetGValue(s.bgColor);
    const int bb   = GetBValue(s.bgColor);
    const int fr   = Clampi(s.frost, 0, 100);

    for (int y = 0; y < H; ++y)
    {
        int grad = 0, edge = 0;
        if (fr > 0)
        {
            // 顶部略亮 → 底部略暗, 制造玻璃厚度感 (最大 ±8)
            double t = (H <= 1) ? 0.0 : (double)y / (double)(H - 1);
            grad = (int)std::lround((0.5 - t) * 16.0);
            if (y == 0)      edge =  22;   // 顶边高光
            else if (y == H - 1) edge = -18;   // 底边阴影
        }
        const double amp = fr / 100.0 * 12.0;

        DWORD* row = &c.bg[(size_t)y * (size_t)W];
        for (int x = 0; x < W; ++x)
        {
            int noise = 0;
            if (fr > 0)
            {
                unsigned hsh = Hash2D(x, y);
                double r = (double)(hsh & 0xFFFFu) / 65535.0 * 2.0 - 1.0;
                noise = (int)std::lround(r * amp);
            }
            const int rr = Clampi(br + grad + edge + noise, 0, 255);
            const int gg = Clampi(bgc + grad + edge + noise, 0, 255);
            const int b2 = Clampi(bb + grad + edge + noise, 0, 255);

            row[x] = ((DWORD)PremByte(a, a) << 24)
                   | ((DWORD)PremByte(rr, a) << 16)
                   | ((DWORD)PremByte(gg, a) << 8)
                   | (DWORD)PremByte(b2, a);
        }
    }
}

//------------------------------------------------------------------------------
// 文字间隔
//------------------------------------------------------------------------------
inline int GapOf(const BannerState& s, int textW)
{
    if (s.gap > 0) return s.gap;
    const int autoGap = s.fontSize * 6;
    const int minGap  = (textW > 0) ? (textW > 1600 ? 160 : 120) : 120;
    return (autoGap > minGap) ? autoGap : minGap;
}

//------------------------------------------------------------------------------
// 参数同步: 几何 / 背景 / 字体 / 文字宽度
//------------------------------------------------------------------------------
void SyncBanner(RenderCtx& c, const BannerState& s, bool force)
{
    if (!c.hwnd) return;

    Geometry g = ComputeGeometry(s);
    const bool geomChanged = force
        || g.x != c.geo.x || g.y != c.geo.y || g.w != c.geo.w || g.h != c.geo.h;

    if (geomChanged)
    {
        c.geo = g;
        if (!EnsureBuffers(c, g))
        {
            SetLastErrorCode(DB_ERR_WINDOW);
            return;
        }

        ::SetWindowPos(c.hwnd,
                       s.topMost ? HWND_TOPMOST : HWND_BOTTOM,
                       g.x, g.y, g.w, g.h,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);

        BuildBackground(c, s);
        c.curBgColor = s.bgColor;
        c.curBgAlpha = s.bgAlpha;
        c.curFrost   = s.frost;
    }
    else if (c.curBgColor != s.bgColor || c.curBgAlpha != s.bgAlpha || c.curFrost != s.frost)
    {
        BuildBackground(c, s);
        c.curBgColor = s.bgColor;
        c.curBgAlpha = s.bgAlpha;
        c.curFrost   = s.frost;
    }

    // 字体
    if (!c.font || c.curFontSize != s.fontSize || c.curFontName != s.fontName)
    {
        c.curFontSize = s.fontSize;
        c.curFontName = s.fontName;
        if (c.font) ::DeleteObject(c.font);

        const std::wstring face = s.fontName.empty() ? std::wstring(kDefFontName) : s.fontName;
        c.font = ::CreateFontW(-(LONG)Clampi(s.fontSize, 6, 400), 0, 0, 0,
                               FW_MEDIUM, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY,          // 灰度抗锯齿, 便于取覆盖率
                               DEFAULT_PITCH | FF_DONTCARE,
                               face.c_str());
        c.textW = -1;
    }

    // 文字
    if (c.curText != s.text)
    {
        c.curText = s.text;
        c.textW   = -1;
        c.advance = 0.0;        // 文字变了, 从最右侧重新滚动
    }

    if (c.textW < 0)
    {
        if (s.text.empty() || !c.font || !c.hdcMask)
        {
            c.textW = 0;
        }
        else
        {
            ::SelectObject(c.hdcMask, c.font);
            SIZE sz{ 0, 0 };
            ::GetTextExtentPoint32W(c.hdcMask, s.text.c_str(), (int)s.text.size(), &sz);
            c.textW = sz.cx;
        }
    }
}

//------------------------------------------------------------------------------
// 绘制一帧并提交给分层窗口
//------------------------------------------------------------------------------
void RenderBanner(RenderCtx& c, const BannerState& s)
{
    if (!c.hwnd || !c.bitsFrame || !c.bitsMask || c.bufW <= 0 || c.bufH <= 0) return;

    const int W = c.bufW;
    const int H = c.bufH;

    // 1) 清空文字蒙版
    RECT rcAll{ 0, 0, W, H };
    ::FillRect(c.hdcMask, &rcAll, (HBRUSH)::GetStockObject(BLACK_BRUSH));

    // 2) 在蒙版上按周期重复绘制白色文字 (覆盖整条横幅, 两个方向都适用)
    const int textW = c.textW;
    if (textW > 0 && !s.text.empty() && c.font)
    {
        ::SelectObject(c.hdcMask, c.font);
        ::SetBkMode(c.hdcMask, TRANSPARENT);
        ::SetTextColor(c.hdcMask, RGB(255, 255, 255));

        const int cycle = textW + GapOf(s, textW);
        const int baseX = W - (int)std::lround(c.advance);
        const int n     = (cycle > 0 ? (W / cycle) : 0) + 2;

        for (int k = -n; k <= n; ++k)
        {
            const int x = baseX + k * cycle;
            if (x > W) continue;
            if (x + textW + 6 < 0) continue;

            RECT rc{ x, 0, x + textW + 6, H };
            ::DrawTextW(c.hdcMask, s.text.c_str(), (int)s.text.size(), &rc,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
        }
    }

    // 3) 背景 + 文字覆盖率合成 (premultiplied BGRA)
    DWORD*       dst  = (DWORD*)c.bitsFrame;
    const DWORD* mask = (const DWORD*)c.bitsMask;

    ::memcpy(dst, c.bg.data(), (size_t)W * (size_t)H * sizeof(DWORD));

    const int tr = GetRValue(s.textColor);
    const int tg = GetGValue(s.textColor);
    const int tb = GetBValue(s.textColor);

    const size_t total = (size_t)W * (size_t)H;
    for (size_t i = 0; i < total; ++i)
    {
        const int cov = (int)(mask[i] & 0xFF);      // 蒙版为白色文字, 取蓝色通道即覆盖率
        if (cov == 0) continue;

        const int k   = 255 - cov;
        const DWORD p = dst[i];

        const int ba = (int)((p >> 24) & 0xFF);
        const int br = (int)((p >> 16) & 0xFF);
        const int bg = (int)((p >> 8) & 0xFF);
        const int bb = (int)(p & 0xFF);

        const int oa = cov + (ba * k + 127) / 255;
        const int orr = (tr * cov + br * k + 127) / 255;
        const int og = (tg * cov + bg * k + 127) / 255;
        const int ob = (tb * cov + bb * k + 127) / 255;

        dst[i] = ((DWORD)ClampB(oa) << 24) | ((DWORD)ClampB(orr) << 16)
               | ((DWORD)ClampB(og) << 8) | (DWORD)ClampB(ob);
    }

    // 4) 提交
    POINT ptSrc{ 0, 0 };
    SIZE  sz{ W, H };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    ::UpdateLayeredWindow(c.hwnd, c.hdcScreen, nullptr, &sz, c.hdcFrame,
                          &ptSrc, 0, &bf, ULW_ALPHA);
}

//------------------------------------------------------------------------------
// 窗口过程
//------------------------------------------------------------------------------
LRESULT CALLBACK BannerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RenderCtx* c = (RenderCtx*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_NCCREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }

    case WM_NCHITTEST:
        return HTTRANSPARENT;          // 鼠标完全穿透(与 WS_EX_TRANSPARENT 双保险)

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;          // 永不抢焦点

    case WM_ERASEBKGND:
        return 1;                      // 内容由 UpdateLayeredWindow 提供, 不擦背景防闪

    case WM_TIMER:
        if (c && wp == kRenderTimerId)
        {
            BannerState s = SnapshotState();

            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            double dt = 0.0;
            if (c->lastTick.QuadPart != 0 && c->qpcFreq.QuadPart > 0)
            {
                dt = (double)(now.QuadPart - c->lastTick.QuadPart) / (double)c->qpcFreq.QuadPart;
                if (dt > 0.2) dt = 0.2;      // 系统卡顿/休眠回来, 不要一下跳很远
            }
            c->lastTick = now;

            SyncBanner(*c, s, false);

            if (c->textW > 0 && s.speed != 0)
            {
                const double cycle = (double)c->textW + (double)GapOf(s, c->textW);
                if (cycle > 0.0)
                {
                    c->advance += (double)s.speed * dt;
                    c->advance = std::fmod(c->advance, cycle);
                    if (c->advance < 0.0) c->advance += cycle;
                }
            }

            RenderBanner(*c, s);
        }
        return 0;

    case WM_DB_REFRESH:
        if (c)
        {
            BannerState s = SnapshotState();
            SyncBanner(*c, s, false);
            RenderBanner(*c, s);
        }
        return 0;

    case WM_DB_QUIT:
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (c)
        {
            c->geo = Geometry{ -1, -1, -1, -1 };   // 强制重算几何(分辨率/任务栏变化)
            BannerState s = SnapshotState();
            SyncBanner(*c, s, true);
            RenderBanner(*c, s);
        }
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

//------------------------------------------------------------------------------
// 渲染线程
//------------------------------------------------------------------------------
void RunBannerThread(std::shared_ptr<RenderHost> host)
{
    RenderCtx& c = host->ctx;
    host->startResult = DB_ERR_WINDOW;

    // 仅本线程切换 DPI 感知, 不影响宿主进程
    typedef void* (WINAPI *PFN_SetThreadDpi)(void*);
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll"))
    {
        PFN_SetThreadDpi fn = (PFN_SetThreadDpi)(void*)::GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        if (fn) fn((void*)-4);            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    ::timeBeginPeriod(1);                 // 提高定时器精度, 滚动更顺

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = 0;
    wc.lpfnWndProc   = BannerWndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;

    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        SetLastErrorCode(DB_ERR_WINDOW);
        if (host->readyEvent) ::SetEvent(host->readyEvent);
        if (host->doneEvent)  ::SetEvent(host->doneEvent);
        ::timeEndPeriod(1);
        return;
    }

    BannerState s = SnapshotState();
    Geometry    g = ComputeGeometry(s);

    if (!::QueryPerformanceFrequency(&c.qpcFreq)) c.qpcFreq.QuadPart = 1;

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (s.topMost) exStyle |= WS_EX_TOPMOST;

    c.hwnd = ::CreateWindowExW(exStyle, kWndClass, L"DesktopBanner", WS_POPUP,
                               g.x, g.y, g.w, g.h, nullptr, nullptr,
                               wc.hInstance, &c);

    if (!c.hwnd)
    {
        SetLastErrorCode(DB_ERR_WINDOW);
        if (host->readyEvent) ::SetEvent(host->readyEvent);
        if (host->doneEvent)  ::SetEvent(host->doneEvent);
        ::timeEndPeriod(1);
        return;
    }

    g_hwnd.store(c.hwnd);
    g_renderThreadId.store(::GetCurrentThreadId());
    c.hdcScreen = ::GetDC(nullptr);

    ::ShowWindow(c.hwnd, SW_SHOWNOACTIVATE);
    ::SetWindowPos(c.hwnd, s.topMost ? HWND_TOPMOST : HWND_BOTTOM,
                   g.x, g.y, g.w, g.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SyncBanner(c, s, true);
    RenderBanner(c, s);

    host->startResult = DB_OK;
    SetLastErrorCode(DB_OK);
    if (host->readyEvent) ::SetEvent(host->readyEvent);

    ::SetTimer(c.hwnd, kRenderTimerId, kRenderTimerMs, nullptr);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // 收尾
    ReleaseBuffers(c);
    if (c.font) { ::DeleteObject(c.font); c.font = nullptr; }
    if (c.hdcScreen) { ::ReleaseDC(nullptr, c.hdcScreen); c.hdcScreen = nullptr; }
    c.hwnd = nullptr;
    g_hwnd.store(nullptr);
    g_renderThreadId.store(0);

    ::timeEndPeriod(1);

    if (host->readyEvent) ::SetEvent(host->readyEvent);   // 防极端情况下的等待挂死
    if (host->doneEvent)  ::SetEvent(host->doneEvent);
}

//------------------------------------------------------------------------------
// 启动 / 停止 (调用者需持有 g_lifeMutex)
//------------------------------------------------------------------------------
void StopInternal()
{
    if (!g_running.load() && !g_thread.joinable())
        return;

    HWND h = g_hwnd.load();
    if (h) ::PostMessageW(h, WM_DB_QUIT, 0, 0);

    if (g_thread.joinable())
    {
        std::shared_ptr<RenderHost> host = g_host;
        bool joined = false;

        if (host && host->doneEvent)
        {
            DWORD wr = ::WaitForSingleObject(host->doneEvent, 3000);
            if (wr == WAIT_OBJECT_0)
            {
                g_thread.join();
                joined = true;
            }
        }
        else
        {
            g_thread.join();
            joined = true;
        }

        if (!joined)
        {
            // 极端情况: 渲染线程卡死。脱离线程, 避免宿主被拖住(句柄保留, 由系统回收)。
            g_thread.detach();
            SetLastErrorCode(DB_ERR_TIMEOUT);
            g_running.store(false);
            g_hwnd.store(nullptr);
            return;
        }
    }

    g_running.store(false);
    g_hwnd.store(nullptr);
    g_renderThreadId.store(0);

    if (g_host)
    {
        if (g_host->readyEvent) ::CloseHandle(g_host->readyEvent);
        if (g_host->doneEvent)  ::CloseHandle(g_host->doneEvent);
        g_host->readyEvent = nullptr;
        g_host->doneEvent  = nullptr;
        g_host.reset();
    }
}

int StartBanner(const BannerState& st)
{
    std::lock_guard<std::mutex> lk(g_lifeMutex);

    if (g_running.load()) StopInternal();

    BannerState st2 = st;
    {
        std::lock_guard<std::mutex> sl(g_stateMutex);
        // Init 只负责样式/几何, 不清空已有文案; 要清空请调用 Banner_SetTextW(L"")
        if (st2.text.empty()) st2.text = g_state.text;
        g_state = st2;
    }

    std::shared_ptr<RenderHost> host = std::make_shared<RenderHost>();
    host->readyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    host->doneEvent  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!host->readyEvent || !host->doneEvent)
    {
        if (host->readyEvent) ::CloseHandle(host->readyEvent);
        if (host->doneEvent)  ::CloseHandle(host->doneEvent);
        SetLastErrorCode(DB_ERR_THREAD);
        return DB_ERR_THREAD;
    }

    g_running.store(true);
    g_host = host;

    g_thread = std::thread([host]() { RunBannerThread(host); });

    DWORD wr = ::WaitForSingleObject(host->readyEvent, 5000);
    if (wr != WAIT_OBJECT_0 || host->startResult != DB_OK)
    {
        int err = (host->startResult != DB_OK) ? host->startResult : DB_ERR_THREAD;
        // 启动失败, 回收线程
        if (g_thread.joinable())
        {
            HWND h = g_hwnd.load();
            if (h) ::PostMessageW(h, WM_DB_QUIT, 0, 0);
            DWORD wr2 = ::WaitForSingleObject(host->doneEvent, 2000);
            if (wr2 == WAIT_OBJECT_0) g_thread.join();
            else                      g_thread.detach();
        }
        g_running.store(false);
        g_hwnd.store(nullptr);
        if (host->readyEvent) ::CloseHandle(host->readyEvent);
        if (host->doneEvent)  ::CloseHandle(host->doneEvent);
        host->readyEvent = nullptr;
        host->doneEvent  = nullptr;
        if (g_host == host) g_host.reset();
        SetLastErrorCode(err);
        return err;
    }

    SetLastErrorCode(DB_OK);
    return DB_OK;
}

void FillDefaults(BannerOptions& o)
{
    ::ZeroMemory(&o, sizeof(o));
    o.cbSize    = (DWORD)sizeof(BannerOptions);
    o.bgColor   = kDefBgColor;
    o.bgAlpha   = (BYTE)kDefBgAlpha;
    o.textColor = kDefTextColor;
    o.speed     = kDefSpeed;
    o.fontSize  = kDefFontSize;
    o.fontName  = kDefFontName;
    o.height    = kDefHeight;
    o.anchor    = DB_ANCHOR_SCREEN_TOP;
    o.x         = 0;
    o.y         = 0;
    o.width     = 0;
    o.topMost   = TRUE;
    o.gap       = 0;
    o.frost     = kDefFrost;
    for (int i = 0; i < 4; ++i) o.reserved[i] = 0;
}

} // anonymous namespace

//==============================================================================
// 导出接口实现
//==============================================================================
extern "C"
{

DB_API int DB_CALL Banner_GetVersion(void)
{
    return DB_VERSION;
}

DB_API int DB_CALL Banner_GetDefaultOptions(BannerOptions* options)
{
    if (!options) { SetLastErrorCode(DB_ERR_PARAM); return DB_ERR_PARAM; }
    FillDefaults(*options);
    SetLastErrorCode(DB_OK);
    return DB_OK;
}

DB_API BOOL DB_CALL Banner_Init(COLORREF bgColor, BYTE bgAlpha, COLORREF textColor, int speed)
{
    BannerState st;
    st.text.clear();      // 初始化只改样式, 文案沿用已设置的内容
    st.bgColor   = bgColor;
    st.bgAlpha   = Clampi((int)bgAlpha, 0, 255);
    st.textColor = textColor;
    st.speed     = speed;

    return (StartBanner(st) == DB_OK) ? TRUE : FALSE;
}

DB_API BOOL DB_CALL Banner_InitEx(const BannerOptions* options)
{
    if (!options)
    {
        SetLastErrorCode(DB_ERR_PARAM);
        return FALSE;
    }
    if (options->cbSize != 0 && options->cbSize != (DWORD)sizeof(BannerOptions))
    {
        SetLastErrorCode(DB_ERR_PARAM);
        return FALSE;
    }

    BannerState st;
    st.text.clear();      // 初始化只改样式, 文案沿用已设置的内容
    st.bgColor   = options->bgColor;
    st.bgAlpha   = Clampi((int)options->bgAlpha, 0, 255);
    st.textColor = options->textColor;
    st.speed     = options->speed;
    st.fontSize  = (options->fontSize > 0) ? options->fontSize : kDefFontSize;
    st.fontName  = options->fontName ? std::wstring(options->fontName) : std::wstring(kDefFontName);
    st.height    = (options->height > 0) ? options->height : kDefHeight;
    st.anchor    = options->anchor;
    st.x         = options->x;
    st.y         = options->y;
    st.width     = (options->width > 0) ? options->width : 0;
    st.topMost   = options->topMost ? TRUE : FALSE;
    st.gap       = (options->gap > 0) ? options->gap : 0;
    st.frost     = Clampi(options->frost, 0, 100);

    return (StartBanner(st) == DB_OK) ? TRUE : FALSE;
}

DB_API void DB_CALL Banner_SetStyle(COLORREF bgColor, BYTE bgAlpha, COLORREF textColor, int speed)
{
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.bgColor   = bgColor;
        g_state.bgAlpha   = Clampi((int)bgAlpha, 0, 255);
        g_state.textColor = textColor;
        g_state.speed     = speed;
    }
    PostRefresh();
}

DB_API void DB_CALL Banner_SetBackground(COLORREF bgColor, BYTE bgAlpha)
{
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.bgColor = bgColor;
        g_state.bgAlpha = Clampi((int)bgAlpha, 0, 255);
    }
    PostRefresh();
}

DB_API void DB_CALL Banner_SetTextColor(COLORREF textColor)
{
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.textColor = textColor;
    }
    PostRefresh();
}

DB_API void DB_CALL Banner_SetSpeed(int speed)
{
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.speed = speed;
    }
    PostRefresh();
}

DB_API void DB_CALL Banner_SetFontSize(int pixelHeight)
{
    pixelHeight = Clampi(pixelHeight, 6, 400);
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.fontSize = pixelHeight;
    }
    PostRefresh();
}

DB_API void DB_CALL Banner_SetFrost(int strength)
{
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.frost = Clampi(strength, 0, 100);
    }
    PostRefresh();
}

DB_API BOOL DB_CALL Banner_SetTextW(LPCWSTR text)
{
    if (!text)
    {
        SetLastErrorCode(DB_ERR_PARAM);
        return FALSE;
    }
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.text = text;
    }
    PostRefresh();
    SetLastErrorCode(DB_OK);
    return TRUE;
}

DB_API BOOL DB_CALL Banner_SetTextA(LPCSTR utf8Text)
{
    if (!utf8Text)
    {
        SetLastErrorCode(DB_ERR_PARAM);
        return FALSE;
    }

    std::wstring w;
    bool ok = false;

    int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8Text, -1, nullptr, 0);
    if (need > 0)
    {
        w.resize((size_t)need);
        if (::MultiByteToWideChar(CP_UTF8, 0, utf8Text, -1, &w[0], need) > 0)
        {
            while (!w.empty() && w.back() == L'\0') w.pop_back();
            ok = true;
        }
    }

    if (!ok)   // UTF-8 解析失败, 退回本地代码页
    {
        need = ::MultiByteToWideChar(CP_ACP, 0, utf8Text, -1, nullptr, 0);
        if (need > 0)
        {
            w.resize((size_t)need);
            if (::MultiByteToWideChar(CP_ACP, 0, utf8Text, -1, &w[0], need) > 0)
            {
                while (!w.empty() && w.back() == L'\0') w.pop_back();
                ok = true;
            }
        }
    }

    if (!ok)
    {
        SetLastErrorCode(DB_ERR_PARAM);
        return FALSE;
    }

    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_state.text = w;
    }
    PostRefresh();
    SetLastErrorCode(DB_OK);
    return TRUE;
}

DB_API void DB_CALL Banner_Close(void)
{
    std::lock_guard<std::mutex> lk(g_lifeMutex);
    StopInternal();
}

DB_API BOOL DB_CALL Banner_IsActive(void)
{
    return (g_running.load() && g_hwnd.load() != nullptr) ? TRUE : FALSE;
}

DB_API DWORD DB_CALL Banner_GetLastError(void)
{
    return g_lastError.load();
}

} // extern "C"
