// screenshare.cpp
// Build example (Visual Studio, x64, Release):
//   cl /std:c++17 /EHsc screenshare.cpp \
//      /link d3d11.lib dxgi.lib Gdi32.lib Ws2_32.lib turbojpeg.lib Shlwapi.lib
//
// Single‑binary screen‑sharing tool (server + client)
// ─────────────────────────────────────────────────────────────────────────────
// 2025‑07‑03  — Update‑3: Client remembers the **last IP** used (saved to %TEMP%).
//                Server supports sequential reconnections. No code is omitted.
// ─────────────────────────────────────────────────────────────────────────────

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <turbojpeg.h>
#include <shlwapi.h>     // PathCombineA

#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "libturbojpeg.dll.a")
#pragma comment(lib, "Shlwapi.lib")

// ---------------------------------------------------------------------------
//  Helper: console‑friendly error print
// ---------------------------------------------------------------------------
static void PrintError(const char* msg, HRESULT hr = S_OK)
{
    if (hr != S_OK)
        std::cerr << msg << " (HRESULT = 0x" << std::hex << hr << std::dec << ")\n";
    else
        std::cerr << msg << "\n";
}

// ---------------------------------------------------------------------------
//  Helper: load / save last‑used server IP in %TEMP%
// ---------------------------------------------------------------------------
namespace ipcache
{
    std::string makePath()
    {
        char tmp[MAX_PATH] = {};
        DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n == 0 || n > MAX_PATH)
            return "screenshare_last_ip.txt";   // fallback to cwd

        char full[MAX_PATH] = {};
        PathCombineA(full, tmp, "screenshare_last_ip.txt");
        return full;
    }

    std::string load()
    {
        std::ifstream fin(makePath());
        std::string    ip;
        if (fin)
            std::getline(fin, ip);
        return ip;
    }

    void save(const std::string& ip)
    {
        std::ofstream fout(makePath(), std::ios::trunc);
        if (fout)
            fout << ip;
    }
} // namespace ipcache

// ===========================================================================
//  SERVER – namespace server
// ==========================================================================
namespace server
{
    constexpr int SERVER_PORT = 9999;
    constexpr int JPEG_QUALITY = 75;

    // ---------------------------------------------------------------------------
    //  Desktop‑Duplication initialisation
    // ---------------------------------------------------------------------------
    bool InitDesktopDuplication(
        ID3D11Device** dev,
        ID3D11DeviceContext** ctx,
        IDXGIOutputDuplication** dup,
        UINT& outW,
        UINT& outH)
    {
        static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL            obtained{};

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            levels,
            _countof(levels),
            D3D11_SDK_VERSION,
            dev,
            &obtained,
            ctx);

        if (FAILED(hr))
        {
            PrintError("D3D11CreateDevice failed", hr);
            return false;
        }

        IDXGIDevice* dxgiDev = nullptr;
        hr = (*dev)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
        if (FAILED(hr))
        {
            PrintError("QueryInterface(IDXGIDevice) failed", hr);
            return false;
        }

        IDXGIAdapter* adapter = nullptr;
        hr = dxgiDev->GetAdapter(&adapter);
        dxgiDev->Release();
        if (FAILED(hr))
        {
            PrintError("GetAdapter failed", hr);
            return false;
        }

        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(0, &output);
        adapter->Release();
        if (FAILED(hr))
        {
            PrintError("EnumOutputs failed", hr);
            return false;
        }

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        output->Release();
        if (FAILED(hr))
        {
            PrintError("QueryInterface(IDXGIOutput1) failed", hr);
            return false;
        }

        hr = output1->DuplicateOutput(*dev, dup);
        output1->Release();
        if (FAILED(hr))
        {
            PrintError("DuplicateOutput failed – another app already capturing?", hr);
            return false;
        }

        DXGI_OUTDUPL_DESC desc{};
        (*dup)->GetDesc(&desc);
        outW = desc.ModeDesc.Width;
        outH = desc.ModeDesc.Height;

        return true;
    }

    // ---------------------------------------------------------------------------
    //  Run server – listens forever, allowing sequential reconnections
    // ---------------------------------------------------------------------------
    int Run()
    {
        // Winsock initialisation
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            PrintError("WSAStartup failed");
            return -1;
        }

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET)
        {
            PrintError("socket() failed");
            WSACleanup();
            return -1;
        }

        sockaddr_in srvAddr{};
        srvAddr.sin_family = AF_INET;
        srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        srvAddr.sin_port = htons(SERVER_PORT);

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&srvAddr), sizeof(srvAddr)) == SOCKET_ERROR)
        {
            PrintError("bind() failed");
            closesocket(listenSock);
            WSACleanup();
            return -1;
        }

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR)
        {
            PrintError("listen() failed");
            closesocket(listenSock);
            WSACleanup();
            return -1;
        }

        std::cout << "Server: Listening on port " << SERVER_PORT << " …\n";

        // Persistent D3D / JPEG resources
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        IDXGIOutputDuplication* dup = nullptr;
        UINT                     deckW = 0;
        UINT                     deckH = 0;

        if (!InitDesktopDuplication(&dev, &ctx, &dup, deckW, deckH))
        {
            closesocket(listenSock);
            WSACleanup();
            return -1;
        }

        tjhandle tj = tjInitCompress();
        if (!tj)
        {
            PrintError("tjInitCompress() failed");
            dup->Release();
            ctx->Release();
            dev->Release();
            closesocket(listenSock);
            WSACleanup();
            return -1;
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = deckW;
        td.Height = deckH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ID3D11Texture2D* staging = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging)) || !staging)
        {
            PrintError("CreateTexture2D (staging) failed");
            tjDestroy(tj);
            dup->Release();
            ctx->Release();
            dev->Release();
            closesocket(listenSock);
            WSACleanup();
            return -1;
        }

        // Accept loop
        for (;;)
        {
            std::cout << "Server: Waiting for a client …\n";
            SOCKET clientSock = accept(listenSock, nullptr, nullptr);
            if (clientSock == INVALID_SOCKET)
            {
                PrintError("accept() failed");
                continue;
            }

            std::cout << "Server: Client connected.\n";

            // Capture & send loop
            while (true)
            {
                IDXGIResource* desktopRes = nullptr;
                DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

                HRESULT hr = dup->AcquireNextFrame(500, &frameInfo, &desktopRes);
                if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                    continue;
                if (FAILED(hr))
                {
                    PrintError("AcquireNextFrame failed", hr);
                    break;
                }

                ID3D11Texture2D* frameTex = nullptr;
                hr = desktopRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&frameTex));
                desktopRes->Release();
                if (FAILED(hr) || !frameTex)
                {
                    PrintError("QueryInterface(ID3D11Texture2D) failed", hr);
                    dup->ReleaseFrame();
                    break;
                }

                ctx->CopyResource(staging, frameTex);
                frameTex->Release();
                dup->ReleaseFrame();

                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
                if (FAILED(hr))
                {
                    PrintError("Map(staging) failed", hr);
                    break;
                }

                const unsigned char* src = static_cast<const unsigned char*>(mapped.pData);
                const int            pitch = static_cast<int>(mapped.RowPitch);
                const int            width = static_cast<int>(deckW);
                const int            height = static_cast<int>(deckH);

                unsigned char* jpegBuf = nullptr;
                unsigned long  jpegSize = 0;

                if (tjCompress2(
                    tj,
                    src,
                    width,
                    pitch,
                    height,
                    TJPF_BGRA,
                    &jpegBuf,
                    &jpegSize,
                    TJSAMP_420,
                    JPEG_QUALITY,
                    0) < 0)
                {
                    PrintError("tjCompress2 failed");
                    ctx->Unmap(staging, 0);
                    break;
                }
                ctx->Unmap(staging, 0);

                // Send length + payload
                unsigned int netLen = htonl(static_cast<unsigned int>(jpegSize));
                if (send(clientSock, reinterpret_cast<const char*>(&netLen), 4, 0) != 4)
                {
                    PrintError("send(length) failed");
                    tjFree(jpegBuf);
                    break;
                }

                int sentTotal = 0;
                while (sentTotal < static_cast<int>(jpegSize))
                {
                    int s = send(clientSock,
                        reinterpret_cast<const char*>(jpegBuf) + sentTotal,
                        static_cast<int>(jpegSize) - sentTotal,
                        0);
                    if (s <= 0)
                    {
                        PrintError("send(data) failed");
                        break;
                    }
                    sentTotal += s;
                }
                tjFree(jpegBuf);
                if (sentTotal != static_cast<int>(jpegSize))
                    break; // connection lost
            }

            closesocket(clientSock);
            std::cout << "Server: Client disconnected – ready for new connection.\n";
        }

        // Unreachable but included for completeness
        tjDestroy(tj);
        staging->Release();
        dup->Release();
        ctx->Release();
        dev->Release();
        closesocket(listenSock);
        WSACleanup();
        return 0;
    }
} // namespace server

// ===========================================================================
//  CLIENT – namespace client
// ==========================================================================
namespace client
{
    constexpr int SERVER_PORT = 9999;

    // Globals
    unsigned char* g_rgbBuffer = nullptr;
    int                     g_imgWidth = 0;
    int                     g_imgHeight = 0;
    BITMAPINFO              g_bmpInfo = {};
    std::atomic<bool>       g_hasNewFrame = false;
    std::mutex              g_bufMutex;
    tjhandle                g_tjDecompress = nullptr;

#ifndef WM_APP_UPDATEFRAME
#    define WM_APP_UPDATEFRAME (WM_APP + 1)
#endif

    // ---------------------------------------------------------------------------
    //  Window procedure
    // ---------------------------------------------------------------------------
    LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_APP_UPDATEFRAME:
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC          hdc = BeginPaint(hWnd, &ps);
            {
                std::lock_guard<std::mutex> lock(g_bufMutex);
                if (g_hasNewFrame && g_rgbBuffer && g_imgWidth && g_imgHeight)
                {
                    SetDIBitsToDevice(
                        hdc,
                        0,
                        0,
                        g_imgWidth,
                        g_imgHeight,
                        0,
                        0,
                        0,
                        g_imgHeight,
                        g_rgbBuffer,
                        &g_bmpInfo,
                        DIB_RGB_COLORS);
                }
            }
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hWnd, msg, wp, lp);
        }
    }

    // ---------------------------------------------------------------------------
    //  Receiver thread – receives JPEG via TCP and signals repaint
    // ---------------------------------------------------------------------------
    void ReceiverThread(HWND hWnd, const char* serverIp)
    {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            std::cerr << "WSAStartup failed\n";
            return;
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
        {
            std::cerr << "socket() failed\n";
            WSACleanup();
            return;
        }

        sockaddr_in srvAddr{};
        srvAddr.sin_family = AF_INET;
        srvAddr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, serverIp, &srvAddr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&srvAddr), sizeof(srvAddr)) == SOCKET_ERROR)
        {
            std::cerr << "connect() failed\n";
            closesocket(sock);
            WSACleanup();
            return;
        }

        std::cout << "Client: Connected to server\n";

        g_tjDecompress = tjInitDecompress();
        if (!g_tjDecompress)
        {
            std::cerr << "tjInitDecompress() failed\n";
            closesocket(sock);
            WSACleanup();
            return;
        }

        while (true)
        {
            unsigned int netLen = 0;
            int          recvd = recv(sock, reinterpret_cast<char*>(&netLen), 4, MSG_WAITALL);
            if (recvd != 4)
            {
                std::cerr << "recv(length) failed or connection closed\n";
                break;
            }
            unsigned int jpegSize = ntohl(netLen);
            if (jpegSize == 0)
                break;

            std::unique_ptr<unsigned char[]> jpegBuf(new unsigned char[jpegSize]);
            unsigned int total = 0;
            while (total < jpegSize)
            {
                int r = recv(sock, reinterpret_cast<char*>(jpegBuf.get()) + total, jpegSize - total, 0);
                if (r <= 0)
                {
                    std::cerr << "recv(data) failed\n";
                    goto END;
                }
                total += r;
            }

            int width = 0, height = 0, subsamp = 0, colorspace = 0;
            if (tjDecompressHeader3(g_tjDecompress, jpegBuf.get(), jpegSize, &width, &height, &subsamp, &colorspace) < 0)
            {
                std::cerr << "tjDecompressHeader3 failed: " << tjGetErrorStr() << "\n";
                continue;
            }

            const int pitch24 = width * 3;
            const int bufSize = pitch24 * height;

            {
                std::lock_guard<std::mutex> lock(g_bufMutex);
                if (!g_rgbBuffer || width != g_imgWidth || height != g_imgHeight)
                {
                    delete[] g_rgbBuffer;
                    g_rgbBuffer = new unsigned char[bufSize];

                    g_imgWidth = width;
                    g_imgHeight = height;

                    ZeroMemory(&g_bmpInfo, sizeof(g_bmpInfo));
                    g_bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    g_bmpInfo.bmiHeader.biWidth = g_imgWidth;
                    g_bmpInfo.bmiHeader.biHeight = -g_imgHeight; // top‑down DIB
                    g_bmpInfo.bmiHeader.biPlanes = 1;
                    g_bmpInfo.bmiHeader.biBitCount = 24;
                    g_bmpInfo.bmiHeader.biCompression = BI_RGB;
                    g_bmpInfo.bmiHeader.biSizeImage = bufSize;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_bufMutex);
                if (tjDecompress2(g_tjDecompress, jpegBuf.get(), jpegSize, g_rgbBuffer, width, pitch24, height, TJPF_BGR, TJFLAG_FASTDCT) < 0)
                {
                    std::cerr << "tjDecompress2 failed: " << tjGetErrorStr() << "\n";
                    continue;
                }

                constexpr uint8_t TH = 32;
                unsigned char* p = g_rgbBuffer;
                const int         px = width * height;
                for (int i = 0; i < px; ++i, p += 3)
                {
                    if (p[0] < TH && p[1] < TH && p[2] < TH)
                        p[0] = p[1] = p[2] = 0;
                }
            }

            g_hasNewFrame = true;
            PostMessage(hWnd, WM_APP_UPDATEFRAME, 0, 0);
        }
    END:
        if (g_tjDecompress)
        {
            tjDestroy(g_tjDecompress);
            g_tjDecompress = nullptr;
        }
        delete[] g_rgbBuffer;
        g_rgbBuffer = nullptr;
        closesocket(sock);
        WSACleanup();
        std::cout << "Client: Receiver thread exiting\n";
    }

    // ---------------------------------------------------------------------------
    //  Run client – sets up borderless transparent window
    // ---------------------------------------------------------------------------
    int Run(const char* serverIp)
    {
        HINSTANCE hInst = GetModuleHandle(nullptr);

        const wchar_t CLASS_NAME[] = L"ScreenShareClientWindow";

        WNDCLASS wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

        if (!RegisterClass(&wc))
        {
            MessageBox(nullptr, L"RegisterClass failed", L"Error", MB_ICONERROR);
            return 0;
        }

        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);

        HWND hWnd = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            CLASS_NAME,
            L"",
            WS_POPUP,
            0,
            0,
            screenW,
            screenH,
            nullptr,
            nullptr,
            hInst,
            nullptr);

        if (!hWnd)
        {
            MessageBox(nullptr, L"CreateWindowEx failed", L"Error", MB_ICONERROR);
            return 0;
        }

        SetLayeredWindowAttributes(hWnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

        SetWindowDisplayAffinity(hWnd, WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);

        std::thread recvThr(ReceiverThread, hWnd, serverIp);

        MSG msg{};
        while (GetMessage(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (recvThr.joinable())
            recvThr.join();

        return 0;
    }
} // namespace client

// ===========================================================================
//  ENTRY POINT – choose mode at runtime and remember last IP
// ===========================================================================
int main(int argc, char* argv[])
{
    std::string mode;

    if (argc >= 2)
        mode = argv[1];
    else
    {
        std::cout << "Run as (s)erver or (c)lient? ";
        std::getline(std::cin, mode);
    }

    for (char& ch : mode)
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));

    if (mode == "s" || mode == "server")
        return server::Run();

    if (mode == "c" || mode == "client")
    {
        std::string ip;

        if (argc >= 3)
        {
            ip = argv[2];
        }
        else
        {
            std::string last = ipcache::load();
            if (last.empty())
                last = "127.0.0.1";

            std::cout << "Server IP [" << last << "]: ";
            std::getline(std::cin, ip);
            if (ip.empty())
                ip = last;
        }

        ipcache::save(ip);
        return client::Run(ip.c_str());
    }

    std::cerr << "Unknown mode – use 'server' or 'client'.\n";
    return -1;
}
