#if defined(_WIN32)
#include "wcam_windows.hpp"
#include <cstdlib>
#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include "make_device_id.hpp"
#include "wcam/wcam.hpp"

/// NB: we use DirectShow and not MediaFoundation
/// because OBS Virtual Camera only works with DirectShow
/// https://github.com/obsproject/obs-studio/issues/8057
/// Apparently Windows 11 adds this capability (https://medium.com/deelvin-machine-learning/how-does-obs-virtual-camera-plugin-work-on-windows-e92ab8986c4e)
/// So in a very distant future, when Windows 11 is on 99.999% of the machines, and when OBS implements a MediaFoundation backend and a virtual camera for it, then we can switch to MediaFoundation

namespace wcam::internal {
using namespace std; // TODO remove

auto convert_wstr_to_str(BSTR const& wstr) -> std::string
{
    int const wstr_len = static_cast<int>(SysStringLen(wstr));
    // Determine the size of the resulting string
    int const res_len = WideCharToMultiByte(CP_UTF8, 0, wstr, wstr_len, nullptr, 0, nullptr, nullptr);
    if (res_len == 0)
        return "";

    // Allocate the necessary buffer
    auto res = std::string(res_len, 0);

    // Perform the conversion
    WideCharToMultiByte(CP_UTF8, 0, wstr, wstr_len, res.data(), res_len, nullptr, nullptr);
    return res;
}

static auto hr2err(HRESULT hr) -> std::string
{
    char* error_message{nullptr};
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        hr,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<LPSTR>(&error_message), // NOLINT(*reinterpret-cast)
        0,
        nullptr
    );
    auto message = std::string{error_message};
    LocalFree(error_message);
    return message;
}

static void throw_error(HRESULT hr, std::string_view code_that_failed, std::source_location location = std::source_location::current())
{
    throw std::runtime_error{std::format("{}(During `{}`, at {}({}:{}))", hr2err(hr), code_that_failed, location.file_name(), location.line(), location.column())};
}

#define THROW_IF_ERR(exp) /*NOLINT(*macro*)*/ \
    {                                         \
        HRESULT hresult = exp;                \
        if (FAILED(hresult))                  \
            throw_error(hresult, #exp);       \
    }
#define THROW_IF_ERR2(exp, location) /*NOLINT(*macro*)*/ \
    {                                                    \
        HRESULT hresult = exp;                           \
        if (FAILED(hresult))                             \
            throw_error(hresult, #exp, location);        \
    }

template<typename T>
class AutoRelease {
public:
    AutoRelease() = default;
    explicit AutoRelease(REFCLSID class_id, std::source_location location = std::source_location::current())
    {
        THROW_IF_ERR2(CoCreateInstance(class_id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_ptr)), location);
    }

    ~AutoRelease()
    {
        if (_ptr != nullptr)
            _ptr->Release();
    }

    AutoRelease(AutoRelease const&)                = delete;
    AutoRelease& operator=(AutoRelease const&)     = delete;
    AutoRelease(AutoRelease&&) noexcept            = delete;
    AutoRelease& operator=(AutoRelease&&) noexcept = delete;

    auto operator->() -> T* { return _ptr; }
    auto operator->() const -> T const* { return _ptr; }
    auto operator*() -> T& { return *_ptr; }
    auto operator*() const -> T const& { return *_ptr; }
    operator T*() { return _ptr; } // NOLINT(*explicit*)
    T** operator&()                // NOLINT(*runtime-operator)
    {
        return &_ptr;
    }

private:
    T* _ptr{nullptr};
};

static void CoInitializeIFN()
{
    struct Raii { // NOLINT(*special-member-functions)
        Raii() { THROW_IF_ERR(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
        ~Raii() { CoUninitialize(); }
    };
    thread_local Raii instance{}; // Each thread needs to call CoInitializeEx once
}

STDMETHODIMP_(ULONG)
CaptureImpl::AddRef()
{
    return InterlockedIncrement(&_ref_count);
}

STDMETHODIMP_(ULONG)
CaptureImpl::Release()
{
    return InterlockedDecrement(&_ref_count);
}

STDMETHODIMP CaptureImpl::QueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_ISampleGrabberCB || riid == IID_IUnknown)
    {
        *ppv = reinterpret_cast<void*>(this); // NOLINT(*reinterpret-cast*)
        return NOERROR;
    }
    return E_NOINTERFACE;
}

auto find_webcam_name(IMoniker* pMoniker) -> std::string
{
    auto pPropBag = AutoRelease<IPropertyBag>{};
    THROW_IF_ERR(pMoniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&pPropBag))); // TODO should we continue the loop if there is an error here ?

    VARIANT webcam_name_wstr;
    VariantInit(&webcam_name_wstr);
    HRESULT hr = pPropBag->Read(L"FriendlyName", &webcam_name_wstr, nullptr); // TODO what happens if friendly name is missing ?
    // if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
    // {
    //     hr = pPropBag->Read(L"DevicePath", &webcam_name_wstr, nullptr);
    // }
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
    {
        return "Unnamed webcam";
    }
    auto res = convert_wstr_to_str(webcam_name_wstr.bstrVal);
    THROW_IF_ERR(VariantClear(&webcam_name_wstr)); // TODO should we throw here ? // TODO must clear before the early return above
    return res;
}

auto find_webcam_id(IMoniker* pMoniker) -> DeviceId
{
    auto pPropBag = AutoRelease<IPropertyBag>{};
    THROW_IF_ERR(pMoniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&pPropBag)));

    VARIANT webcam_name_wstr;
    VariantInit(&webcam_name_wstr);
    HRESULT hr = pPropBag->Read(L"DevicePath", &webcam_name_wstr, nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) // It can happen, for example OBS Virtual Camera doesn't have a DevicePath
    {
        return make_device_id(find_webcam_name(pMoniker));
    }
    auto res = convert_wstr_to_str(webcam_name_wstr.bstrVal);
    THROW_IF_ERR(VariantClear(&webcam_name_wstr)); // TODO should we throw here ?
    return make_device_id(res);
}

// Release the format block for a media type.

// Delete a media type structure that was allocated on the heap.
void DeleteMediaType(AM_MEDIA_TYPE* pmt)
{
    if (pmt != NULL)
    {
        if (pmt->cbFormat != 0)
        {
            CoTaskMemFree((PVOID)pmt->pbFormat);
            pmt->cbFormat = 0;
            pmt->pbFormat = NULL;
        }
        if (pmt->pUnk != NULL)
        {
            // pUnk should not be used.
            pmt->pUnk->Release();
            pmt->pUnk = NULL;
        }
        CoTaskMemFree(pmt);
    }
}

// HRESULT videoInput::ShowFilterPropertyPages(IBaseFilter* pFilter)
// {
//     ISpecifyPropertyPages* pProp;

//     HRESULT hr = pFilter->QueryInterface(IID_ISpecifyPropertyPages, (void**)&pProp);
//     if (SUCCEEDED(hr))
//     {
//         // Get the filter's name and IUnknown pointer.
//         FILTER_INFO FilterInfo;
//         hr = pFilter->QueryFilterInfo(&FilterInfo);
//         IUnknown* pFilterUnk;
//         pFilter->QueryInterface(IID_IUnknown, (void**)&pFilterUnk);

//         // Show the page.
//         CAUUID caGUID;
//         pProp->GetPages(&caGUID);
//         pProp->Release();
//         OleCreatePropertyFrame(
//             NULL,               // Parent window
//             0, 0,               // Reserved
//             FilterInfo.achName, // Caption for the dialog box
//             1,                  // Number of objects (just the filter)
//             &pFilterUnk,        // Array of object pointers.
//             caGUID.cElems,      // Number of property pages
//             caGUID.pElems,      // Array of property page CLSIDs
//             0,                  // Locale identifier
//             0, NULL             // Reserved
//         );

//         // Clean up.
//         if (pFilterUnk)
//             pFilterUnk->Release();
//         if (FilterInfo.pGraph)
//             FilterInfo.pGraph->Release();
//         CoTaskMemFree(caGUID.pElems);
//     }
//     return hr;
// }

// auto CreateEventCodeMap(long evCode) -> std::string
// {
//     std::map<long, std::string> evCodeMap;

//     evCodeMap[EC_COMPLETE]                  = "EC_COMPLETE";
//     evCodeMap[EC_USERABORT]                 = "EC_USERABORT";
//     evCodeMap[EC_ERRORABORT]                = "EC_ERRORABORT";
//     evCodeMap[EC_TIME]                      = "EC_TIME";
//     evCodeMap[EC_REPAINT]                   = "EC_REPAINT";
//     evCodeMap[EC_STREAM_ERROR_STOPPED]      = "EC_STREAM_ERROR_STOPPED";
//     evCodeMap[EC_STREAM_ERROR_STILLPLAYING] = "EC_STREAM_ERROR_STILLPLAYING";
//     evCodeMap[EC_ERROR_STILLPLAYING]        = "EC_ERROR_STILLPLAYING";
//     evCodeMap[EC_PALETTE_CHANGED]           = "EC_PALETTE_CHANGED";
//     evCodeMap[EC_VIDEO_SIZE_CHANGED]        = "EC_VIDEO_SIZE_CHANGED";
//     evCodeMap[EC_QUALITY_CHANGE]            = "EC_QUALITY_CHANGE";
//     evCodeMap[EC_SHUTTING_DOWN]             = "EC_SHUTTING_DOWN";
//     evCodeMap[EC_CLOCK_CHANGED]             = "EC_CLOCK_CHANGED";
//     evCodeMap[EC_PAUSED]                    = "EC_PAUSED";
//     evCodeMap[EC_OPENING_FILE]              = "EC_OPENING_FILE";
//     evCodeMap[EC_BUFFERING_DATA]            = "EC_BUFFERING_DATA";
//     evCodeMap[EC_FULLSCREEN_LOST]           = "EC_FULLSCREEN_LOST";
//     evCodeMap[EC_ACTIVATE]                  = "EC_ACTIVATE";
//     evCodeMap[EC_NEED_RESTART]              = "EC_NEED_RESTART";
//     evCodeMap[EC_WINDOW_DESTROYED]          = "EC_WINDOW_DESTROYED";
//     evCodeMap[EC_DISPLAY_CHANGED]           = "EC_DISPLAY_CHANGED";
//     evCodeMap[EC_STARVATION]                = "EC_STARVATION";
//     evCodeMap[EC_OLE_EVENT]                 = "EC_OLE_EVENT";
//     evCodeMap[EC_NOTIFY_WINDOW]             = "EC_NOTIFY_WINDOW";
//     evCodeMap[EC_STREAM_CONTROL_STOPPED]    = "EC_STREAM_CONTROL_STOPPED";
//     evCodeMap[EC_STREAM_CONTROL_STARTED]    = "EC_STREAM_CONTROL_STARTED";
//     evCodeMap[EC_END_OF_SEGMENT]            = "EC_END_OF_SEGMENT";
//     evCodeMap[EC_SEGMENT_STARTED]           = "EC_SEGMENT_STARTED";
//     evCodeMap[EC_LENGTH_CHANGED]            = "EC_LENGTH_CHANGED";
//     evCodeMap[EC_DEVICE_LOST]               = "EC_DEVICE_LOST";

//     auto it = evCodeMap.find(evCode);
//     if (it == evCodeMap.end())
//         throw 0;

//     // Add more event codes as needed

//     return it->second;
// }

auto CaptureImpl::is_disconnected() -> bool
{
    long     evCode;
    LONG_PTR param1, param2;
    bool     disconnected = false;

    while (S_OK == _media_event->GetEvent(&evCode, &param1, &param2, 0))
    {
        // std::cout << CreateEventCodeMap(evCode) << '\n';
        if (evCode == EC_DEVICE_LOST) // TODO distringuish this (CaptureError::WebcamUnplugged) from EC_ERRORABORT (CaptureError::WebcamAlreadyUsedInAnotherApplication)
        {
            disconnected = true;
        }
        if (evCode == EC_ERRORABORT)
        {
            // throw 0;
            disconnected = true;
        }
        _media_event->FreeEventParams(evCode, param1, param2);
    }
    return disconnected;
}

CaptureImpl::CaptureImpl(DeviceId const& device_id, img::Size const& requested_resolution)
{
    CoInitializeIFN();

    auto pBuilder = AutoRelease<ICaptureGraphBuilder2>{CLSID_CaptureGraphBuilder2};
    auto pGraph   = AutoRelease<IGraphBuilder>{CLSID_FilterGraph};
    THROW_IF_ERR(pBuilder->SetFiltergraph(pGraph));

    THROW_IF_ERR(pGraph->QueryInterface(IID_IMediaEventEx, (void**)&_media_event));

    // Obtenir l'objet Moniker correspondant au périphérique sélectionné
    AutoRelease<ICreateDevEnum> pDevEnum{CLSID_SystemDeviceEnum};

    auto pEnum = AutoRelease<IEnumMoniker>{};
    THROW_IF_ERR(pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0));

    auto pMoniker = AutoRelease<IMoniker>{};
    while (pEnum->Next(1, &pMoniker, NULL) == S_OK)
    {
        if (find_webcam_id(pMoniker) == device_id)
            break;
        // TODO error if webcam is not found
    }
    // Liaison au filtre de capture du périphérique sélectionné

    IBaseFilter* pCap = nullptr;
    THROW_IF_ERR(pMoniker->BindToObject(0, 0, IID_IBaseFilter, (void**)&pCap));
    THROW_IF_ERR(pGraph->AddFilter(pCap, L"CaptureFilter"));

    IAMStreamConfig* pConfig = NULL;
    HRESULT          hr      = pBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCap, IID_IAMStreamConfig, (void**)&pConfig);
    AM_MEDIA_TYPE*   pmt;
    hr = pConfig->GetFormat(&pmt); // Get the current format

    if (SUCCEEDED(hr))
    {
        // VIDEOINFOHEADER structure contains the format details
        VIDEOINFOHEADER* pVih = (VIDEOINFOHEADER*)pmt->pbFormat;

        // Set desired width and height
        pVih->bmiHeader.biWidth  = requested_resolution.width();
        pVih->bmiHeader.biHeight = requested_resolution.height();

        // Set the modified format
        hr = pConfig->SetFormat(pmt);

        if (FAILED(hr))
        {
            // Handle error
        }

        DeleteMediaType(pmt);
    }
    else
    {
        // Handle error
    }

    // 4. Add and Configure the Sample Grabber

    AutoRelease<IBaseFilter>    pSampleGrabberFilter{CLSID_SampleGrabber};
    AutoRelease<ISampleGrabber> pSampleGrabber{};
    THROW_IF_ERR(pSampleGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&pSampleGrabber));

    // Configure the sample grabber
    if (device_id.as_string().find("OBS") != std::string::npos
        || device_id.as_string().find("Streamlabs") != std::string::npos)
    {
        // OBS Virtual Camera always returns S_OK on SetFormat(), even if it doesn't support
        // the actual format. So we have to choose a format that it supports manually, e.g. NV12.
        // https://github.com/opencv/opencv/issues/19746#issuecomment-1383056787
        _video_format = MEDIASUBTYPE_NV12;
    }
    else
    {
        _video_format = MEDIASUBTYPE_RGB24;
    }
    AM_MEDIA_TYPE mt;
    ZeroMemory(&mt, sizeof(AM_MEDIA_TYPE));
    mt.majortype = MEDIATYPE_Video;
    mt.subtype   = _video_format;
    THROW_IF_ERR(pSampleGrabber->SetMediaType(&mt));
    THROW_IF_ERR(pSampleGrabber->SetOneShot(false));
    THROW_IF_ERR(pSampleGrabber->SetBufferSamples(false));

    // Add the sample grabber to the graph
    THROW_IF_ERR(pGraph->AddFilter(pSampleGrabberFilter, L"Sample Grabber"));

    // 5. Render the Stream
    AutoRelease<IBaseFilter> pNullRenderer{CLSID_NullRenderer};
    THROW_IF_ERR(pGraph->AddFilter(pNullRenderer, L"Null Renderer"));

    THROW_IF_ERR(pBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, pCap, pSampleGrabberFilter, pNullRenderer)); // Check that PIN_CATEGORY_PREVIEW is indeed more performant than PIN_CATEGORY_CAPTURE
    // 6. Retrieve the Video Information Header

    // TODO check if this does anything //EXP - lets try setting the sync source to null - and make it run as fast as possible
    // {
    //     IMediaFilter* pMediaFilter = 0;
    //     hr                         = VD->pGraph->QueryInterface(IID_IMediaFilter, (void**)&pMediaFilter);
    //     if (FAILED(hr))
    //     {
    //         DebugPrintOut("ERROR: Could not get IID_IMediaFilter interface\n");
    //     }
    //     else
    //     {
    //         pMediaFilter->SetSyncSource(NULL);
    //         pMediaFilter->Release();
    //     }
    // }

    AM_MEDIA_TYPE mtGrabbed;
    THROW_IF_ERR(pSampleGrabber->GetConnectedMediaType(&mtGrabbed));

    VIDEOINFOHEADER* pVih = (VIDEOINFOHEADER*)mtGrabbed.pbFormat;
    _resolution           = img::Size{
        static_cast<img::Size::DataType>(pVih->bmiHeader.biWidth),
        static_cast<img::Size::DataType>(pVih->bmiHeader.biHeight),
    };

    assert(
        (_video_format == MEDIASUBTYPE_RGB24 && pVih->bmiHeader.biSizeImage == _resolution.pixels_count() * 3)
        || (_video_format == MEDIASUBTYPE_NV12 && pVih->bmiHeader.biSizeImage == _resolution.pixels_count() * 3 / 2)
    );

    // 7. Start the Graph

    THROW_IF_ERR(pSampleGrabber->SetCallback(this, 1));
    THROW_IF_ERR(pGraph->QueryInterface(IID_IMediaControl, (void**)&_media_control));
    THROW_IF_ERR(_media_control->Run());

    if (is_disconnected())
        throw CaptureError{Error_WebcamAlreadyUsedInAnotherApplication{}};
}

auto CaptureImpl::image() -> MaybeImage
{
    std::lock_guard lock{_mutex};

    // if (std::holds_alternative<CaptureError>(_image))
    // {
    //     THROW_IF_ERR(_media_control->Run());
    //     if (is_disconnected())
    //         _image = CaptureError::WebcamAlreadyUsedInAnotherApplication;
    // }

    auto res = std::move(_image);
    if (std::holds_alternative<img::Image>(res))
        _image = NoNewImageAvailableYet{}; // Make sure we know that the current image has been consumed

    return res; // We don't use std::move here because it would prevent copy elision
}

CaptureImpl::~CaptureImpl()
{
    _media_control->Stop();
    _media_control->Release();
    _media_event->Release();
}

// #if defined(GCC) || defined(__clang__)
// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wlanguage-extension-token"
// #endif

static auto get_video_parameters(IBaseFilter* pCaptureFilter) -> std::vector<img::Size>
{
    auto available_resolutions = std::vector<img::Size>{};

    auto pEnumPins = AutoRelease<IEnumPins>{};
    THROW_IF_ERR(pCaptureFilter->EnumPins(&pEnumPins));
    while (true)
    {
        auto pPin = AutoRelease<IPin>{}; // Declare pPin inside the loop so that it is freed at the end, Next doesn't Release the pin that you pass
        if (pEnumPins->Next(1, &pPin, nullptr) != S_OK)
            break;
        PIN_DIRECTION pinDirection; // NOLINT(*-init-variables)
        pPin->QueryDirection(&pinDirection);

        if (pinDirection != PINDIR_OUTPUT)
            continue;
        AutoRelease<IAMStreamConfig> pStreamConfig; // NOLINT(*-init-variables)
        THROW_IF_ERR(pPin->QueryInterface(IID_PPV_ARGS(&pStreamConfig)));
        int iCount; // NOLINT(*-init-variables)
        int iSize;  // NOLINT(*-init-variables)
        THROW_IF_ERR(pStreamConfig->GetNumberOfCapabilities(&iCount, &iSize));
        VIDEO_STREAM_CONFIG_CAPS caps;
        for (int i = 0; i < iCount; i++)
        {
            AM_MEDIA_TYPE* pmtConfig;                                                                  // NOLINT(*-init-variables)
            THROW_IF_ERR(pStreamConfig->GetStreamCaps(i, &pmtConfig, reinterpret_cast<BYTE*>(&caps))); // NOLINT(*-pro-type-reinterpret-cast)
            if (pmtConfig->formattype != FORMAT_VideoInfo)
                continue;
            auto* pVih = reinterpret_cast<VIDEOINFOHEADER*>(pmtConfig->pbFormat); // NOLINT(*-pro-type-reinterpret-cast)
            available_resolutions.push_back({static_cast<img::Size::DataType>(pVih->bmiHeader.biWidth), static_cast<img::Size::DataType>(pVih->bmiHeader.biHeight)});
        }
    }

    return available_resolutions;
}

auto grab_all_infos_impl() -> std::vector<Info>
{
    CoInitializeIFN();

    auto pDevEnum = AutoRelease<ICreateDevEnum>{CLSID_SystemDeviceEnum};
    auto pEnum    = AutoRelease<IEnumMoniker>{};
    THROW_IF_ERR(pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0));
    if (pEnum == nullptr) // Might still be nullptr after CreateClassEnumerator if the VideoInputDevice category is empty or missing (https://learn.microsoft.com/en-us/previous-versions/ms784969(v=vs.85))
        return {};

    thread_local auto resolutions_cache = std::unordered_map<std::string, std::vector<img::Size>>{}; // This cache limits the number of times we will allocate IBaseFilter which seems to leak because of a Windows bug.

    auto infos = std::vector<Info>{};

    while (true)
    {
        auto pMoniker = AutoRelease<IMoniker>{};
        if (pEnum->Next(1, &pMoniker, nullptr) != S_OK)
            break;

        // }
        // if (SUCCEEDED(hr))
        {
            auto       available_resolutions = std::vector<img::Size>{};
            auto const webcam_name           = find_webcam_name(pMoniker);
            auto const it                    = resolutions_cache.find(webcam_name);
            if (it != resolutions_cache.end())
            {
                available_resolutions = it->second;
            }
            else
            {
                auto pCaptureFilter = AutoRelease<IBaseFilter>{};
                THROW_IF_ERR(pMoniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&pCaptureFilter)));
                available_resolutions          = get_video_parameters(pCaptureFilter);
                resolutions_cache[webcam_name] = available_resolutions;
            }
            if (!available_resolutions.empty())
            {
                infos.push_back({webcam_name, find_webcam_id(pMoniker), available_resolutions});
            }
        }
        // else
        // {
        //     throw 0;
        // }
    }

    return infos;
}

} // namespace wcam::internal

// #if defined(__GNUC__) || defined(__clang__)
// #pragma GCC diagnostic pop
// #endif

#endif