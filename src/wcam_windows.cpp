#if defined(_WIN32)
#include "wcam_windows.hpp"
#include <dshow.h>
#include <cstdlib>
#include <format>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include "wcam/wcam.hpp"

namespace wcam::internal {
using namespace std; // TODO remove

auto convert_wstr_to_str(BSTR const& wstr) -> std::string
{
    int const wstr_len = static_cast<int>(SysStringLen(wstr));
    // Determine the size of the resulting string
    int const res_len = WideCharToMultiByte(CP_UTF8, 0, wstr, wstr_len, nullptr, 0, nullptr, nullptr);
    if (res_len == 0)
    {
        assert(false);
        return "";
    }

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
        HRESULT hr = exp;                     \
        if (FAILED(hr))                       \
            throw_error(hr, #exp);            \
    }
#define THROW_IF_ERR2(exp, location) /*NOLINT(*macro*)*/ \
    {                                                    \
        HRESULT hr = exp;                                \
        if (FAILED(hr))                                  \
            throw_error(hr, #exp, location);             \
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
    auto ptr_to_ptr() -> T** { return &_ptr; }

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

CaptureImpl::CaptureImpl(UniqueId const& unique_id, img::Size const& requested_resolution)
{
    CoInitializeIFN();

    auto pBuild = AutoRelease<ICaptureGraphBuilder2>{CLSID_CaptureGraphBuilder2};
    auto pGraph = AutoRelease<IGraphBuilder>{CLSID_FilterGraph};
    THROW_IF_ERR(pBuild->SetFiltergraph(pGraph));

    // Obtenir l'objet Moniker correspondant au périphérique sélectionné
    AutoRelease<ICreateDevEnum> pDevEnum{CLSID_SystemDeviceEnum};

    auto pEnum = AutoRelease<IEnumMoniker>{};
    THROW_IF_ERR(pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, pEnum.ptr_to_ptr(), 0));

    auto pMoniker = AutoRelease<IMoniker>{};
    while (pEnum->Next(1, pMoniker.ptr_to_ptr(), NULL) == S_OK)
    {
        auto pPropBag = AutoRelease<IPropertyBag>{};
        THROW_IF_ERR(pMoniker->BindToStorage(0, 0, IID_PPV_ARGS(pPropBag.ptr_to_ptr()))); // TODO should we continue the loop instead of throwing here ?

        VARIANT var;
        VariantInit(&var);

        THROW_IF_ERR(pPropBag->Read(L"FriendlyName", &var, 0)); // TODO can this legitimately fail ?
        if (UniqueId{convert_wstr_to_str(var.bstrVal)} == unique_id)
        {
            break;
        }
        THROW_IF_ERR(VariantClear(&var)); // TODO memory leak : need to do that before we break // TODO should we throw here ?
    }
    // TODO tell the moniker which resolution to use
    // Liaison au filtre de capture du périphérique sélectionné

    IBaseFilter* pCap = nullptr;
    THROW_IF_ERR(pMoniker->BindToObject(0, 0, IID_IBaseFilter, (void**)&pCap));

    // 3. Add the Webcam Filter to the Graph

    THROW_IF_ERR(pGraph->AddFilter(pCap, L"CaptureFilter"));

    // 4. Add and Configure the Sample Grabber

    AutoRelease<IBaseFilter>    pSampleGrabberFilter{CLSID_SampleGrabber};
    AutoRelease<ISampleGrabber> pSampleGrabber{};
    THROW_IF_ERR(pSampleGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)pSampleGrabber.ptr_to_ptr()));

    // Configure the sample grabber
    AM_MEDIA_TYPE mt;
    ZeroMemory(&mt, sizeof(AM_MEDIA_TYPE));
    mt.majortype = MEDIATYPE_Video;
    mt.subtype   = MEDIASUBTYPE_RGB24; // Or any other format you prefer
    THROW_IF_ERR(pSampleGrabber->SetMediaType(&mt));
    THROW_IF_ERR(pSampleGrabber->SetOneShot(FALSE));
    THROW_IF_ERR(pSampleGrabber->SetBufferSamples(TRUE));

    // Add the sample grabber to the graph
    THROW_IF_ERR(pGraph->AddFilter(pSampleGrabberFilter, L"Sample Grabber"));

    // 5. Render the Stream
    AutoRelease<IBaseFilter> pNullRenderer{CLSID_NullRenderer};
    THROW_IF_ERR(pGraph->AddFilter(pNullRenderer, L"Null Renderer"));

    THROW_IF_ERR(pBuild->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCap, pSampleGrabberFilter, pNullRenderer));
    // 6. Retrieve the Video Information Header

    AM_MEDIA_TYPE mtGrabbed;
    THROW_IF_ERR(pSampleGrabber->GetConnectedMediaType(&mtGrabbed));

    VIDEOINFOHEADER* pVih = (VIDEOINFOHEADER*)mtGrabbed.pbFormat;
    _resolution           = img::Size{
        static_cast<img::Size::DataType>(pVih->bmiHeader.biWidth),
        static_cast<img::Size::DataType>(pVih->bmiHeader.biHeight),
    };

    assert(pVih->bmiHeader.biSizeImage == _resolution.width() * _resolution.height() * 3);

    // 7. Start the Graph

    // IMediaControl *pControl = NULL;
    THROW_IF_ERR(pGraph->QueryInterface(IID_IMediaControl, (void**)&_media_control));

    // 8. Implement ISampleGrabberCB Interface

    THROW_IF_ERR(_media_control->Run());

    // Create an instance of the callback
    // _sgCallback = SampleGrabberCallback{resolution};

    THROW_IF_ERR(pSampleGrabber->SetCallback(this, 1));
}

CaptureImpl::~CaptureImpl()
{
    _media_control->Stop();
    _media_control->Release();
}

#if defined(GCC) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wlanguage-extension-token"
#endif

static auto get_video_parameters(IBaseFilter* pCaptureFilter) -> std::vector<img::Size>
{
    // TODO use THROW_IF_ERR and AutoRelease
    std::vector<img::Size> available_resolutions;

    IEnumPins* pEnumPins; // NOLINT(*-init-variables)
    HRESULT    hr = pCaptureFilter->EnumPins(&pEnumPins);
    if (SUCCEEDED(hr))
    {
        IPin* pPin; // NOLINT(*-init-variables)
        while (pEnumPins->Next(1, &pPin, nullptr) == S_OK)
        {
            PIN_DIRECTION pinDirection; // NOLINT(*-init-variables)
            pPin->QueryDirection(&pinDirection);

            if (pinDirection == PINDIR_OUTPUT)
            {
                IAMStreamConfig* pStreamConfig; // NOLINT(*-init-variables)
                hr = pPin->QueryInterface(IID_PPV_ARGS(&pStreamConfig));
                if (SUCCEEDED(hr))
                {
                    int iCount; // NOLINT(*-init-variables)
                    int iSize;  // NOLINT(*-init-variables)
                    hr = pStreamConfig->GetNumberOfCapabilities(&iCount, &iSize);
                    if (SUCCEEDED(hr))
                    {
                        VIDEO_STREAM_CONFIG_CAPS caps;
                        for (int i = 0; i < iCount; i++)
                        {
                            AM_MEDIA_TYPE* pmtConfig;                                                         // NOLINT(*-init-variables)
                            hr = pStreamConfig->GetStreamCaps(i, &pmtConfig, reinterpret_cast<BYTE*>(&caps)); // NOLINT(*-pro-type-reinterpret-cast)
                            if (SUCCEEDED(hr))
                            {
                                if (pmtConfig->formattype == FORMAT_VideoInfo)
                                {
                                    auto* pVih = reinterpret_cast<VIDEOINFOHEADER*>(pmtConfig->pbFormat); // NOLINT(*-pro-type-reinterpret-cast)
                                    available_resolutions.push_back({static_cast<img::Size::DataType>(pVih->bmiHeader.biWidth), static_cast<img::Size::DataType>(pVih->bmiHeader.biHeight)});
                                }
                            }
                        }
                    }
                    pStreamConfig->Release();
                }
            }
            pPin->Release();
        }
        pEnumPins->Release();
    }

    return available_resolutions;
}

static auto get_devices_info(IEnumMoniker* pEnum) -> std::vector<Info>
{
    thread_local auto resolutions_cache = std::unordered_map<std::string, std::vector<img::Size>>{}; // This cache limits the number of times we will allocate IBaseFilter which seems to leak because of a Windows bug.

    std::vector<Info> list_webcam_info{};

    IMoniker* pMoniker; // NOLINT(*-init-variables)
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK)
    {
        IPropertyBag* pPropBag; // NOLINT(*-init-variables)
        HRESULT       hr = pMoniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&pPropBag));
        if (FAILED(hr))
        {
            pMoniker->Release();
            continue;
        }

        // Get description or friendly name.
        VARIANT webcam_name_wstr;
        VariantInit(&webcam_name_wstr);
        // hr = pPropBag->Read(L"Description", &webcam_name_wstr, nullptr);// ?????
        // if (FAILED(hr))
        // {
        hr = pPropBag->Read(L"FriendlyName", &webcam_name_wstr, nullptr);
        // }
        if (SUCCEEDED(hr))
        {
            auto       available_resolutions = std::vector<img::Size>{};
            auto const webcam_name           = convert_wstr_to_str(webcam_name_wstr.bstrVal);
            auto const it                    = resolutions_cache.find(webcam_name);
            if (it != resolutions_cache.end())
            {
                available_resolutions = it->second;
            }
            else
            {
                IBaseFilter* pCaptureFilter; // NOLINT(*-init-variables)
                hr = pMoniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&pCaptureFilter));
                if (SUCCEEDED(hr))
                {
                    available_resolutions = get_video_parameters(pCaptureFilter);
                    pCaptureFilter->Release();
                }
                resolutions_cache[webcam_name] = available_resolutions;
            }
            if (!available_resolutions.empty())
            {
                // TODO use device path instead of friendly name as the UniqueId
                list_webcam_info.push_back({webcam_name, UniqueId{webcam_name}, available_resolutions});
            }
        }
        VariantClear(&webcam_name_wstr);
        pPropBag->Release();
        pMoniker->Release();
    }

    return list_webcam_info;
}

static auto enumerate_devices(REFGUID category, IEnumMoniker** ppEnum) -> HRESULT
{
    // Create the System Device Enumerator.
    ICreateDevEnum* pDevEnum; // NOLINT(*-init-variables)
    HRESULT         hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDevEnum));

    if (SUCCEEDED(hr))
    {
        // Create an enumerator for the category.
        hr = pDevEnum->CreateClassEnumerator(category, ppEnum, 0);
        if (hr == S_FALSE)
        {
            hr = VFW_E_NOT_FOUND; // The category is empty. Treat as an error.
        }
        pDevEnum->Release();
    }

    return hr;
}

auto grab_all_infos_impl() -> std::vector<Info>
{
    CoInitializeIFN();

    std::vector<Info> list_webcam_info{};
    HRESULT           hr;
    // if (SUCCEEDED(hr))
    {
        IEnumMoniker* pEnum; // NOLINT(*-init-variables)
        hr = enumerate_devices(CLSID_VideoInputDeviceCategory, &pEnum);
        if (SUCCEEDED(hr))
        {
            list_webcam_info = get_devices_info(pEnum);
            pEnum->Release();
        }
    }
    return list_webcam_info;
}

} // namespace wcam::internal

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif