/* pdfrender.cpp - 用系统 Windows.Data.Pdf 把 PDF 页面渲染成位图（忠实版式）
 *
 * 整体流程（每一步都在下面代码里对应一个小节）：
 *
 *   1. RoInitialize             —— 初始化 WinRT
 *   2. RoGetActivationFactory   —— 拿到 PdfDocument 的静态工厂 IPdfDocumentStatics
 *   3. CreateRandomAccessStreamOnFile —— 用系统 API 把"磁盘文件"包成 WinRT 需要的
 *      IRandomAccessStream，省得自己实现一堆流接口
 *   4. LoadFromStreamAsync      —— 异步加载文档，用轮询 get_Status 等它完成
 *   5. GetPage + RenderWithOptionsToStreamAsync —— 逐页渲染到临时 PNG
 *   6. GDI+ 解码 PNG            —— 变成 HBITMAP，交给阅读视图直接贴图
 *
 * 为什么要轮询而不是注册回调：IAsyncOperation 的 Completed 是一个 WinRT 委托对象，
 * 要在 C 里手搓一个 IUnknown + 委托虚表才能接；而 get_Status 是同步查询，轮询几毫秒
 * 就好，代码量差一个数量级。渲染一页通常 30~200 ms，轮询完全可接受。
 */

/* 下面两个宏必须在所有头文件之前定义：CreateRandomAccessStreamOnFile 在 shcore.h 里
 * 受 NTDDI_VERSION 保护，不定义就拿不到声明。
 * 这里用 #undef 而不是 #ifndef：build.bat 给整个工程传的是 -D_WIN32_WINNT=0x0601
 * （Windows 7 基线），而本文件需要 Win10 的 shcore API，必须强行改成本文件自己的值。 */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000

#include "pdfrender.h"

#include <roapi.h>       /* RoInitialize / RoGetActivationFactory / RoUninitialize */
#include <winstring.h>   /* WindowsCreateString / WindowsDeleteString */
#include <inspectable.h>
#include <asyncinfo.h>   /* IAsyncInfo / AsyncStatus */
#include <shcore.h>      /* CreateRandomAccessStreamOnFile */
#include <gdiplus.h>     /* PNG 解码 */

#include <vector>
#include <map>

namespace {

/* ------------------------------------------------------------------ *
 * 一、手写 WinRT 接口的 C++ 声明
 *
 * MinGW 没有 C++/WinRT，也没有 windows.data.pdf.h，所以这里按 Windows 元数据把用到
 * 的接口逐个声明出来。虚表顺序严格按元数据顺序排列：IInspectable 的 6 个槽之后，
 * 才是接口自己的方法。IID 全部来自 Windows 元数据（已核对）。
 * ------------------------------------------------------------------ */

/* Windows.Foundation.IAsyncInfo：get_Id, get_Status, get_ErrorCode, Cancel, Close */
static const GUID kIID_IAsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

/* IAsyncOperation<T>：IAsyncInfo 之后再接 put_Completed / get_Completed / GetResults */
struct IAsyncOperationX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void** results) = 0;
};

/* IAsyncAction：只有 put_Completed / get_Completed，没有 GetResults */
struct IAsyncActionX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void** handler) = 0;
};

/* Windows.Storage.Streams.IRandomAccessStream（顺序不能乱，只声明不实现） */
struct IRandomAccessStreamX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Size(UINT64 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInputStreamAt(UINT64 pos, void** stream) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOutputStreamAt(UINT64 pos, void** stream) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(UINT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE Seek(UINT64 pos) = 0;
    virtual HRESULT STDMETHODCALLTYPE CloneStream(void** stream) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanRead(BOOLEAN* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanWrite(BOOLEAN* v) = 0;
};

/* Windows.Data.Pdf.IPdfPageRenderOptions：前两个 SourceRect 用不到，只占位保证顺序 */
struct IPdfPageRenderOptionsX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(void* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(const void* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(DWORD* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(DWORD v) = 0;   /* Windows.UI.Color 是 4 字节 */
    virtual HRESULT STDMETHODCALLTYPE get_IsIgnoringHighContrast(BOOLEAN* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsIgnoringHighContrast(BOOLEAN v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapEncoderId(GUID* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BitmapEncoderId(GUID v) = 0;
};

/* Windows.Data.Pdf.IPdfPage：
 *   [0] RenderToStreamAsync(stream)      [1] RenderWithOptionsToStreamAsync(stream, opts)
 *   [2] PreparePageAsync                 [3] Index  [4] Size  [5] Dimensions
 *   [6] Rotation                         [7] PreferredZoom
 */
struct IPdfPageX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IRandomAccessStreamX* out, IAsyncActionX** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IRandomAccessStreamX* out,
                                                                    IPdfPageRenderOptionsX* opts,
                                                                    IAsyncActionX** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(IAsyncActionX** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(void* size) = 0;         /* Windows.Foundation.Size */
    virtual HRESULT STDMETHODCALLTYPE get_Dimensions(void** dims) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Rotation(int* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PreferredZoom(float* v) = 0;
};

/* Windows.Data.Pdf.IPdfDocument：GetPage / PageCount / IsPasswordProtected */
struct IPdfDocumentX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32 index, IPdfPageX** page) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(BOOLEAN* v) = 0;
};

/* Windows.Data.Pdf.IPdfDocumentStatics：四个静态方法，顺序按元数据 */
struct IPdfDocumentStaticsX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(void* file, IAsyncOperationX** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPwdAsync(void* file, HSTRING pwd, IAsyncOperationX** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IRandomAccessStreamX* stream, IAsyncOperationX** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPwdAsync(IRandomAccessStreamX* stream, HSTRING pwd,
                                                                 IAsyncOperationX** op) = 0;
};

/* 接口 IID（全部取自 Windows 元数据） */
const GUID kIID_IRandomAccessStream   = { 0x905a0fe1, 0xbc53, 0x11df, { 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda } };
const GUID kIID_IPdfDocumentStatics   = { 0x433a0b5f, 0xc007, 0x4788, { 0x90, 0xf2, 0x08, 0x14, 0x3d, 0x92, 0x25, 0x99 } };
const GUID kIID_IPdfPageRenderOptions = { 0x3c98056f, 0xb7cf, 0x4c29, { 0x9a, 0x04, 0x52, 0xd9, 0x02, 0x67, 0xf4, 0x25 } };

/* ------------------------------------------------------------------ *
 * 二、模块内部状态
 * ------------------------------------------------------------------ */

struct RenderEntry {
    HBITMAP hbm = nullptr;
    int     page = -1;    /* 这张位图是哪一页（缓存命中必须同时比对页号和宽度） */
    int     width = 0;    /* 位图实际宽度 */
    int     height = 0;
    int     wantW = 0;    /* 请求时的目标宽度，用来判断缓存还能不能复用 */
};

struct Module {
    bool                  roInited = false;     /* RoInitialize 是否成功过 */
    bool                  gdipInited = false;   /* GDI+ 是否初始化过 */
    ULONG_PTR             gdipToken = 0;
    IPdfDocumentStaticsX* statics = nullptr;
    IPdfDocumentX*        doc = nullptr;
    int                   pageCount = 0;
    std::wstring          path;
    int                   tmpSeq = 0;           /* 临时文件名序号 */
    std::vector<RenderEntry> cache;             /* 最近渲染过的页面（最多 3 张） */
    std::wstring          lastErr;              /* 最近一次失败原因（自检用） */
};

Module& M()
{
    static Module m;
    return m;
}

/* 记录"哪一步失败 + HRESULT"，方便 rendertest.exe 定位问题 */
void SetErr(const wchar_t* step, HRESULT hr)
{
    wchar_t buf[64];
    wsprintfW(buf, L"0x%08X", (unsigned)hr);
    M().lastErr = std::wstring(step) + L" 失败（" + buf + L"）";
}

void ReleaseEntry(RenderEntry& e)
{
    if (e.hbm) { DeleteObject(e.hbm); e.hbm = nullptr; }
    e.width = e.height = 0;
    e.wantW = 0;
}

/* 临时 PNG 目录：%TEMP%\NovelReaderPdf\，没有就建 */
std::wstring EnsureTempDir()
{
    wchar_t buf[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, buf);
    std::wstring dir(buf);
    dir += L"NovelReaderPdf";
    CreateDirectoryW(dir.c_str(), nullptr);   /* 已存在时返回 FALSE，不用管 */
    return dir;
}

/* ------------------------------------------------------------------ *
 * 三、WinRT 小工具：HSTRING、激活、等待异步
 * ------------------------------------------------------------------ */

HSTRING MakeHString(const wchar_t* s)
{
    HSTRING h = nullptr;
    WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

/* 激活一个 WinRT 类的指定接口；失败时 *out 为 nullptr */
template <typename T>
bool Activate(const wchar_t* className, const GUID& iid, T** out)
{
    *out = nullptr;
    HSTRING h = MakeHString(className);
    if (!h) return false;
    void* p = nullptr;
    HRESULT hr = RoGetActivationFactory(h, iid, &p);
    WindowsDeleteString(h);
    if (FAILED(hr) || !p) return false;
    *out = static_cast<T*>(p);
    return true;
}

/* 轮询等待 IAsyncOperation 完成并取结果 */
HRESULT WaitOperation(IAsyncOperationX* op, void** results, DWORD timeoutMs)
{
    if (results) *results = nullptr;
    if (!op) return E_POINTER;

    IAsyncInfo* info = nullptr;
    if (FAILED(op->QueryInterface(kIID_IAsyncInfo, (void**)&info)) || !info)
        return E_NOINTERFACE;

    DWORD waited = 0;
    AsyncStatus st = Started;
    while (waited <= timeoutMs) {
        st = Started;
        if (FAILED(info->get_Status(&st))) { info->Release(); return E_FAIL; }
        if (st != Started) break;
        Sleep(5);
        waited += 5;
    }
    if (st == Started) { info->Release(); return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }

    HRESULT hr = E_FAIL;
    if (st == Completed) {
        if (results) op->GetResults(results);
        hr = S_OK;
    } else if (st == Error) {
        HRESULT ec = E_FAIL;
        info->get_ErrorCode(&ec);
        hr = FAILED(ec) ? ec : E_FAIL;
    } else {
        hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    info->Release();
    return hr;
}

/* 轮询等待 IAsyncAction 完成（没有结果可取） */
HRESULT WaitAction(IAsyncActionX* act, DWORD timeoutMs)
{
    if (!act) return E_POINTER;

    IAsyncInfo* info = nullptr;
    if (FAILED(act->QueryInterface(kIID_IAsyncInfo, (void**)&info)) || !info)
        return E_NOINTERFACE;

    DWORD waited = 0;
    AsyncStatus st = Started;
    while (waited <= timeoutMs) {
        st = Started;
        if (FAILED(info->get_Status(&st))) { info->Release(); return E_FAIL; }
        if (st != Started) break;
        Sleep(5);
        waited += 5;
    }
    info->Release();
    if (st == Started) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return st == Completed ? S_OK : E_FAIL;
}

/* ------------------------------------------------------------------ *
 * 四、GDI+ 解码 PNG 成 HBITMAP
 * ------------------------------------------------------------------ */

bool GdiPlusInit()
{
    Module& m = M();
    if (m.gdipInited) return true;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&m.gdipToken, &input, nullptr) != Gdiplus::Ok) return false;
    m.gdipInited = true;
    return true;
}

HBITMAP DecodePngToBitmap(const std::wstring& file, int* outW, int* outH)
{
    if (!GdiPlusInit()) return nullptr;

    Gdiplus::Bitmap bmp(file.c_str(), FALSE);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;

    const int w = (int)bmp.GetWidth();
    const int h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return nullptr;

    HBITMAP hbm = nullptr;
    /* 背景给白色：PDF 页面常带透明底，按黑底合成会出现黑边 */
    if (bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &hbm) != Gdiplus::Ok) return nullptr;

    if (outW) *outW = w;
    if (outH) *outH = h;
    return hbm;
}

/* IActivationFactory：普通 WinRT 类（没有静态方法的那种，例如 PdfPageRenderOptions）
 * 只能先拿激活工厂再 ActivateInstance 建对象，不能用 RoGetActivationFactory 直接要接口。 */
struct IActivationFactoryX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable** instance) = 0;
};

const GUID kIID_IActivationFactory = { 0x00000035, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

/* 创建一个 WinRT 对象并 QI 到指定接口 */
template <typename T>
bool CreateInstance(const wchar_t* className, const GUID& iid, T** out)
{
    *out = nullptr;
    HSTRING h = MakeHString(className);
    if (!h) return false;

    IActivationFactoryX* factory = nullptr;
    HRESULT hr = RoGetActivationFactory(h, kIID_IActivationFactory, (void**)&factory);
    WindowsDeleteString(h);
    if (FAILED(hr) || !factory) return false;

    IInspectable* inst = nullptr;
    hr = factory->ActivateInstance(&inst);
    factory->Release();
    if (FAILED(hr) || !inst) return false;

    hr = inst->QueryInterface(iid, (void**)out);
    inst->Release();
    return SUCCEEDED(hr) && *out != nullptr;
}

/* ------------------------------------------------------------------ *
 * 五、渲染一页：PDF -> 临时 PNG -> HBITMAP
 * ------------------------------------------------------------------ */

HBITMAP RenderPageToBitmap(int index, int targetW, int* outW, int* outH)
{
    Module& m = M();
    m.lastErr.clear();
    if (!m.doc || index < 0 || index >= m.pageCount) {
        m.lastErr = L"文档未打开或页码越界";
        return nullptr;
    }

    IPdfPageX* page = nullptr;
    HRESULT hrPage = m.doc->GetPage((UINT32)index, &page);
    if (FAILED(hrPage) || !page) {
        SetErr(L"GetPage", hrPage);
        return nullptr;
    }

    /* 页面原始尺寸（点）；用它把目标像素宽度换算成渲染高度 */
    double pageWpt = 612.0, pageHpt = 792.0;    /* 兜底用 Letter */
    {
        struct SizeF { float w, h; } sz = { 0, 0 };   /* Windows.Foundation.Size */
        if (SUCCEEDED(page->get_Size(&sz)) && sz.w > 1.0f && sz.h > 1.0f) {
            pageWpt = sz.w;
            pageHpt = sz.h;
        }
    }

    int destW = targetW > 0 ? targetW : (int)pageWpt;
    if (destW < 200) destW = 200;
    if (destW > 4000) destW = 4000;

    /* 2 倍超采样，缩小显示时中文笔画才不会糊 */
    const int renderW = destW * 2;
    int renderH = (int)(pageHpt * renderW / pageWpt + 0.5);
    if (renderH < 1) renderH = 1;

    /* 临时文件名（序号递增，避免上一轮文件句柄没释放就被覆盖） */
    wchar_t name[64];
    wsprintfW(name, L"\\p%d_%d.png", index, (int)(++m.tmpSeq));
    std::wstring tmp = EnsureTempDir() + name;
    DeleteFileW(tmp.c_str());

    IRandomAccessStreamX* outStream = nullptr;
    /* 先把文件建出来：CreateRandomAccessStreamOnFile 以写模式打开时要求文件已存在，
     * 否则返回 0x80070002（找不到文件）。 */
    {
        HANDLE hf = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    }
    HRESULT hr = CreateRandomAccessStreamOnFile(tmp.c_str(), GENERIC_WRITE,
                                               kIID_IRandomAccessStream, (void**)&outStream);
    if (FAILED(hr) || !outStream) {
        SetErr(L"CreateRandomAccessStreamOnFile(写)", hr);
        page->Release();
        return nullptr;
    }

    /* 渲染参数：目标宽高 + 白底 + 忽略高对比反色。
     * PdfPageRenderOptions 没有静态方法，必须走 ActivateInstance 建对象。 */
    IPdfPageRenderOptionsX* opts = nullptr;
    CreateInstance(L"Windows.Data.Pdf.PdfPageRenderOptions", kIID_IPdfPageRenderOptions, &opts);
    if (opts) {
        opts->put_DestinationWidth((UINT32)renderW);
        opts->put_DestinationHeight((UINT32)renderH);
        opts->put_BackgroundColor(0xFFFFFFFF);      /* ARGB 白 */
        opts->put_IsIgnoringHighContrast(TRUE);
    } else {
        m.lastErr = L"创建渲染参数对象失败，将按 96 DPI 渲染";
    }

    IAsyncActionX* act = nullptr;
    if (opts) hr = page->RenderWithOptionsToStreamAsync(outStream, opts, &act);
    else      hr = page->RenderToStreamAsync(outStream, &act);

    if (FAILED(hr) || !act) {
        SetErr(L"RenderToStreamAsync", hr);
        if (act) act->Release();
        if (opts) opts->Release();
        outStream->Release();
        page->Release();
        return nullptr;
    }

    HRESULT hrWait = WaitAction(act, 30000);
    act->Release();
    if (opts) opts->Release();
    outStream->Release();       /* 释放时把缓冲刷到磁盘 */
    page->Release();
    if (FAILED(hrWait)) {
        SetErr(L"等待渲染完成", hrWait);
        DeleteFileW(tmp.c_str());
        return nullptr;
    }

    HBITMAP hbm = DecodePngToBitmap(tmp, outW, outH);
    if (!hbm) {
        m.lastErr = L"PNG 解码失败（渲染出的文件不是有效 PNG）";
        DeleteFileW(tmp.c_str());
        return nullptr;
    }
    DeleteFileW(tmp.c_str());   /* 解完就删，磁盘不留垃圾 */
    return hbm;
}

} /* namespace */

/* ------------------------------------------------------------------ *
 * 六、对外接口
 * ------------------------------------------------------------------ */

bool PdfRenderOpen(const std::wstring& path, std::wstring& err)
{
    err.clear();
    PdfRenderClose();

    Module& m = M();

    if (!m.roInited) {
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        /* 已经初始化过（S_FALSE / RPC_E_CHANGED_MODE）也算可用 */
        if (FAILED(hr) && hr != (HRESULT)0x80010106L /* RPC_E_CHANGED_MODE */) {
            err = L"初始化 WinRT 失败，当前系统可能不支持 PDF 渲染";
            return false;
        }
        m.roInited = true;
    }

    if (!m.statics) {
        if (!Activate(L"Windows.Data.Pdf.PdfDocument", kIID_IPdfDocumentStatics, &m.statics)) {
            err = L"系统里没有 PDF 引擎（需要 Windows 10 及以上）";
            return false;
        }
    }

    /* 把 PDF 文件包成流再交给 PdfDocument 加载 */
    IRandomAccessStreamX* inStream = nullptr;
    HRESULT hr = CreateRandomAccessStreamOnFile(path.c_str(), GENERIC_READ,
                                               kIID_IRandomAccessStream, (void**)&inStream);
    if (FAILED(hr) || !inStream) {
        err = L"打不开这个 PDF 文件（可能被占用或没有读取权限）";
        return false;
    }

    IAsyncOperationX* op = nullptr;
    hr = m.statics->LoadFromStreamAsync(inStream, &op);
    inStream->Release();
    if (FAILED(hr) || !op) {
        err = L"PDF 引擎不接受这个文件";
        return false;
    }

    void* docPtr = nullptr;
    hr = WaitOperation(op, &docPtr, 30000);
    op->Release();
    if (FAILED(hr) || !docPtr) {
        err = L"PDF 解析失败（文件可能损坏或有密码保护）";
        return false;
    }
    m.doc = static_cast<IPdfDocumentX*>(docPtr);

    UINT32 n = 0;
    if (FAILED(m.doc->get_PageCount(&n)) || n == 0) {
        err = L"这份 PDF 里没有可显示的页面";
        PdfRenderClose();
        return false;
    }
    m.pageCount = (int)n;
    m.path = path;
    return true;
}

void PdfRenderClose()
{
    Module& m = M();
    for (size_t i = 0; i < m.cache.size(); i++) ReleaseEntry(m.cache[i]);
    m.cache.clear();
    if (m.doc) { m.doc->Release(); m.doc = nullptr; }
    m.pageCount = 0;
    m.path.clear();
}

bool PdfRenderOk()
{
    Module& m = M();
    return m.doc != nullptr && m.pageCount > 0;
}

int PdfRenderPageCount()
{
    return M().pageCount;
}

bool PdfRenderPageSizePt(int index, double* wPt, double* hPt)
{
    Module& m = M();
    if (!m.doc || index < 0 || index >= m.pageCount) return false;

    IPdfPageX* page = nullptr;
    if (FAILED(m.doc->GetPage((UINT32)index, &page)) || !page) return false;

    struct SizeF { float w, h; } sz = { 0, 0 };
    bool ok = SUCCEEDED(page->get_Size(&sz)) && sz.w > 1.0f && sz.h > 1.0f;
    page->Release();
    if (!ok) return false;
    if (wPt) *wPt = sz.w;
    if (hPt) *hPt = sz.h;
    return true;
}

void PdfRenderDropCache()
{
    Module& m = M();
    for (size_t i = 0; i < m.cache.size(); i++) ReleaseEntry(m.cache[i]);
    m.cache.clear();
}

HBITMAP PdfRenderPageBitmap(int index, int targetW)
{
    Module& m = M();
    if (!m.doc || index < 0 || index >= m.pageCount) return nullptr;

    /* 先看缓存：**同页**且请求宽度差不到 10% 就直接复用，来回翻页不用重渲染。
     * 页号必须一起比：只看宽度的话，翻到下一页会把上一页的图当成"命中"返回，
     * 界面上就会一直显示同一页（这个 bug 曾经真实出现过）。 */
    for (size_t i = 0; i < m.cache.size(); i++) {
        RenderEntry& e = m.cache[i];
        if (e.hbm && e.page == index && e.wantW > 0 &&
            abs(e.wantW - targetW) * 10 <= e.wantW) {
            if (i != 0) { RenderEntry tmp = m.cache[0]; m.cache[0] = e; e = tmp; }
            return m.cache[0].hbm;
        }
    }

    RenderEntry fresh;
    fresh.page = index;
    fresh.wantW = targetW;
    fresh.hbm = RenderPageToBitmap(index, targetW, &fresh.width, &fresh.height);
    if (!fresh.hbm) return nullptr;

    if (m.cache.size() >= 3) {
        ReleaseEntry(m.cache.back());
        m.cache.pop_back();
    }
    m.cache.insert(m.cache.begin(), fresh);
    return m.cache[0].hbm;
}

std::wstring PdfRenderLastError()
{
    return M().lastErr;
}
