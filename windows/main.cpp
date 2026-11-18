#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../examples/sample_license.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

constexpr DWORD cameraStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD allStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

struct Preview {
    std::mutex mutex;
    std::condition_variable ready;
    bool closing = false;
    bool received = false;
    bool useMakeup = false;
    bool makeupAvailable = false;
    HRESULT captureStatus = S_OK;
    DWORD captureFlags = 0;
    ComPtr<IMFSample> sample;
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    const wchar_t* message = L"Opening camera...";
};

class ReaderCallback : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFSourceReaderCallback> {
public:
    explicit ReaderCallback(Preview& preview) : preview_(preview) {}

    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override {
        {
            std::lock_guard<std::mutex> lock(preview_.mutex);
            preview_.sample = sample;
            preview_.captureStatus = status;
            preview_.captureFlags = flags;
            preview_.received = true;
        }
        preview_.ready.notify_one();
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    Preview& preview_;
};

static HRESULT OpenCamera(IMFSourceReaderCallback* callback, IMFMediaSource** source, IMFSourceReader** reader) {
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                               MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return hr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
    if (SUCCEEDED(hr)) {
        hr = count ? devices[0]->ActivateObject(IID_PPV_ARGS(source)) : MF_E_NOT_FOUND;
    }
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (FAILED(hr)) return hr;

    attributes.Reset();
    hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(*source, attributes.Get(), reader);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> type;
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = (*reader)->SetStreamSelection(allStreams, FALSE);
    if (SUCCEEDED(hr)) hr = (*reader)->SetStreamSelection(cameraStream, TRUE);
    if (SUCCEEDED(hr)) hr = (*reader)->SetCurrentMediaType(cameraStream, nullptr, type.Get());
    return hr;
}

static bool CopyPixels(IMFSample* sample, int width, int height, LONG defaultStride, std::vector<uint8_t>& pixels) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    ComPtr<IMF2DBuffer> twoDimensional;
    BYTE* top = nullptr;
    LONG pitch = defaultStride;
    bool locked2D = SUCCEEDED(buffer.As(&twoDimensional)) && SUCCEEDED(twoDimensional->Lock2D(&top, &pitch));
    DWORD length = 0;
    if (!locked2D && FAILED(buffer->Lock(&top, nullptr, &length))) return false;

    int64_t absolutePitch = pitch < 0 ? -(int64_t)pitch : pitch;
    int64_t requiredBytes = absolutePitch * (height - 1) + width * 4;
    bool valid = top && absolutePitch >= width * 4 && (locked2D || requiredBytes <= length);
    if (valid) {
        if (!locked2D && pitch < 0) top += absolutePitch * (height - 1);
        pixels.resize((size_t)width * height * 4);
        for (int y = 0; y < height; ++y) {
            uint8_t* row = pixels.data() + (size_t)y * width * 4;
            std::memcpy(row, top + (ptrdiff_t)y * pitch, (size_t)width * 4);
            for (int x = 0; x < width; ++x) row[x * 4 + 3] = 255;
        }
    }
    if (locked2D) twoDimensional->Unlock2D();
    else buffer->Unlock();
    return valid;
}

static bool SetParameters(xm_desktop_engine* engine, bool makeup) {
    float values[XM_PARAM_COUNT] = {};
    values[XM_PARAM_SMOOTHING] = 4.2f;
    values[XM_PARAM_BRIGHTENING] = 0.3f;
    values[XM_PARAM_MAKEUP_INTENSITY] = makeup ? 0.8f : 0.0f;
    xm_params params = {sizeof(params), values, XM_PARAM_COUNT, makeup ? "dawn" : nullptr, nullptr};
    return xm_desktop_engine_set_params(engine, &params) == XM_OK;
}

static void ShowStatus(Preview& preview, const wchar_t* message) {
    std::lock_guard<std::mutex> lock(preview.mutex);
    preview.message = message;
}

static void CaptureFrames(Preview& preview, IMFSourceReader* reader, xm_desktop_engine* engine) {
    bool makeup = false;
    bool sdkReady = engine && SetParameters(engine, makeup);
    ShowStatus(preview, sdkReady ? L"Camera preview" : L"Camera preview | SDK unavailable");
    std::vector<uint8_t> input, output;
    for (;;) {
        if (FAILED(reader->ReadSample(cameraStream, 0, nullptr, nullptr, nullptr, nullptr))) break;
        ComPtr<IMFSample> sample;
        bool requestedMakeup;
        {
            std::unique_lock<std::mutex> lock(preview.mutex);
            preview.ready.wait(lock, [&] { return preview.received || preview.closing; });
            if (preview.closing) return;
            if (FAILED(preview.captureStatus) ||
                (preview.captureFlags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) break;
            sample = std::move(preview.sample);
            preview.received = false;
            requestedMakeup = preview.useMakeup;
        }
        if (!sample) continue;

        ComPtr<IMFMediaType> type;
        UINT32 width = 0, height = 0, stride = 0;
        if (FAILED(reader->GetCurrentMediaType(cameraStream, &type)) ||
            FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
            width < 1 || height < 1 || width > 4096 || height > 4096) break;
        LONG pitch = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) pitch = (LONG)stride;
        else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &pitch))) break;
        if (!CopyPixels(sample.Get(), (int)width, (int)height, pitch, input)) break;

        if (engine && makeup != requestedMakeup) {
            makeup = requestedMakeup;
            sdkReady = SetParameters(engine, makeup);
        }
        output.resize(input.size());
        int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        xm_desktop_frame frame = {sizeof(frame), input.data(), (int)width, (int)height, (int)width * 4,
                                  XM_PIXEL_BGRA8, timestamp, output.data(), (int)width * 4};
        bool processed = sdkReady && xm_desktop_process(engine, &frame, nullptr) == XM_OK;
        {
            std::lock_guard<std::mutex> lock(preview.mutex);
            preview.pixels.swap(processed ? output : input);
            preview.width = (int)width;
            preview.height = (int)height;
        }
    }
    ShowStatus(preview, L"Camera capture stopped.");
}

static void RunCamera(Preview& preview) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        ShowStatus(preview, L"Cannot initialize COM.");
        return;
    }
    if (FAILED(MFStartup(MF_VERSION))) {
        ShowStatus(preview, L"Cannot start Media Foundation.");
        CoUninitialize();
        return;
    }

    wchar_t assetPath[32768], appId[1024];
    DWORD pathLength = GetEnvironmentVariableW(L"SNAPBEAUTY_ASSETS", assetPath, 32768);
    DWORD idLength = GetEnvironmentVariableW(L"SNAPBEAUTY_APP_ID", appId, 1024);
    char assets[32768], identity[1024];
    xm_desktop_engine* engine = nullptr;
    if (pathLength && pathLength < 32768 && idLength < 1024 &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, assetPath, -1, assets, sizeof(assets), nullptr, nullptr) &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            idLength ? appId : L"ai.snapbeauty.samples.windows", -1,
                            identity, sizeof(identity), nullptr, nullptr) && initialize_license(identity)) {
        xm_desktop_options options = {sizeof(options), assets};
        if (xm_desktop_engine_create(&options, &engine) != XM_OK) {
            fprintf(stderr, "Engine creation failed: %s\n", xm_desktop_last_error(nullptr));
        }
    } else {
        fprintf(stderr, "Check SNAPBEAUTY_ASSETS, SNAPBEAUTY_APP_ID and SNAPBEAUTY_LICENSE_FILE.\n");
    }
    {
        std::error_code error;
        std::lock_guard<std::mutex> lock(preview.mutex);
        preview.makeupAvailable = engine && std::filesystem::is_regular_file(
            std::filesystem::path(assetPath) / L"makeup/lip/lip_01.json", error);
    }

    {
        auto callback = Make<ReaderCallback>(preview);
        ComPtr<IMFMediaSource> source;
        ComPtr<IMFSourceReader> reader;
        HRESULT hr = callback ? OpenCamera(callback.Get(), &source, &reader) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) CaptureFrames(preview, reader.Get(), engine);
        else {
            fprintf(stderr, "Camera setup failed (0x%08lx).\n", (unsigned long)hr);
            ShowStatus(preview, hr == MF_E_NOT_FOUND ? L"No camera found." : L"Cannot open camera. Check Windows camera privacy settings.");
        }
        reader.Reset();
        if (source) source->Shutdown();
        std::lock_guard<std::mutex> lock(preview.mutex);
        preview.sample.Reset();
    }
    xm_desktop_engine_destroy(engine);
    xm_desktop_shutdown();
    MFShutdown();
    CoUninitialize();
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM key, LPARAM data) {
    auto* preview = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams);
        return TRUE;
    case WM_CREATE:
        SetTimer(window, 1, 33, nullptr);
        return 0;
    case WM_TIMER:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (key == 'M' && !(data & (1LL << 30))) {
            std::lock_guard<std::mutex> lock(preview->mutex);
            if (preview->makeupAvailable) preview->useMakeup = !preview->useMakeup;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        {
            std::lock_guard<std::mutex> lock(preview->mutex);
            if (!preview->pixels.empty() && client.right > 0 && client.bottom > 30) {
                int width = client.right, height = client.bottom - 30;
                if (width * preview->height > height * preview->width) width = height * preview->width / preview->height;
                else height = width * preview->height / preview->width;
                BITMAPINFO bitmap = {};
                bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
                bitmap.bmiHeader.biWidth = preview->width;
                bitmap.bmiHeader.biHeight = -preview->height;
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                StretchDIBits(dc, (client.right - width) / 2, (client.bottom - 30 - height) / 2, width, height,
                              0, 0, preview->width, preview->height, preview->pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
            }
            TextOutW(dc, 8, client.bottom - 24, preview->message, (int)wcslen(preview->message));
            if (preview->makeupAvailable && !preview->pixels.empty()) {
                const wchar_t* text = preview->useMakeup ? L"M: Dawn on" : L"M: Dawn off";
                TextOutW(dc, 410, client.bottom - 24, text, (int)wcslen(text));
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, 1);
        {
            std::lock_guard<std::mutex> lock(preview->mutex);
            preview->closing = true;
        }
        preview->ready.notify_one();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, key, data);
    }
}

int main() {
    Preview preview;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = L"SnapBeautySampleWindow";
    if (!RegisterClassW(&windowClass)) return 1;
    HWND window = CreateWindowW(windowClass.lpszClassName, L"SnapBeauty Windows Samples", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 840, 540, nullptr, nullptr, instance, &preview);
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    std::thread capture(RunCamera, std::ref(preview));
    MSG message;
    BOOL status;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (status == -1) DestroyWindow(window);
    capture.join();
    return status == -1 ? 1 : 0;
}
