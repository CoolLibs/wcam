#include "Manager.hpp"
#include <mutex>
#include <variant>
#include "ResolutionsManager.hpp"
#include "WebcamRequest.hpp"

namespace wcam::internal {

#ifndef NDEBUG
std::atomic<int> Manager::_managers_alive_count{0};
#endif

Manager::Manager()
    : _thread{&Manager::thread_job, std::ref(*this)}
{
#ifndef NDEBUG
    assert(_managers_alive_count.load() == 0);
    _managers_alive_count.fetch_add(1);
#endif
}

Manager::~Manager()
{
    _wants_to_stop_thread.store(true);
    _thread.join();

#ifndef NDEBUG
    _managers_alive_count.fetch_add(-1);
#endif
}

void Manager::thread_job(Manager& self)
{
    while (!self._wants_to_stop_thread.load())
        self.update();
}

auto grab_all_infos_impl() -> std::vector<Info>;

static auto grab_all_infos() -> std::vector<Info>
{
    auto list_webcams_infos = internal::grab_all_infos_impl();
    for (auto& webcam_info : list_webcams_infos)
    {
        auto& resolutions = webcam_info.resolutions;
        std::sort(resolutions.begin(), resolutions.end(), [](Resolution const& res_a, Resolution const& res_b) {
            return res_a.pixels_count() > res_b.pixels_count()
                   || (res_a.pixels_count() == res_b.pixels_count() && res_a.width() > res_b.width());
        });
        resolutions.erase(std::unique(resolutions.begin(), resolutions.end()), resolutions.end());
    }
    return list_webcams_infos;
}

auto Manager::infos() const -> std::vector<Info>
{
    std::scoped_lock lock{_infos_mutex};
    return _infos;
}

/// Iterates over the map + Might add a new element to the map
auto Manager::open_or_get_webcam(DeviceId const& id) -> SharedWebcam
{
    std::scoped_lock lock{_captures_mutex};

    auto const it = _current_requests.find(id);
    if (it != _current_requests.end())
    {
        std::shared_ptr<WebcamRequest> const request = it->second.lock();
        if (request) // A capture is still alive, we don't want to recreate a new one (we can't capture the same webcam twice anyways)
            return SharedWebcam{request};
    }
    auto const request    = std::make_shared<WebcamRequest>(id);
    _current_requests[id] = request; // Store a weak_ptr in the current requests
    return SharedWebcam{request};
}

/// Iterates over the map + might modify an element of the map
void Manager::request_a_restart_of_the_capture_if_it_exists(DeviceId const& id)
{
    std::scoped_lock lock{_captures_mutex};

    auto const it = _current_requests.find(id);
    if (it == _current_requests.end())
        return;
    auto const request = it->second.lock();
    if (!request)
        return;
    request->maybe_capture() = CaptureNotInitYet{};
}

auto Manager::default_resolution(DeviceId const& id) const -> Resolution
{
    std::scoped_lock lock{_infos_mutex};
    auto const       it = std::find_if(_infos.begin(), _infos.end(), [&](Info const& info) {
        return info.id == id;
    });
    if (it == _infos.end() || it->resolutions.empty())
        return {1, 1};
    return it->resolutions[0]; // We know that resolutions are sorted from largest to smallest, and we want to select the largest one by default
}

auto Manager::is_plugged_in(DeviceId const& id) const -> bool
{
    std::scoped_lock lock{_infos_mutex};
    return _infos.end() != std::find_if(_infos.begin(), _infos.end(), [&](Info const& info) {
               return info.id == id;
           });
}

/// Iterates over the map + might modify an element of the map
void Manager::update()
{
    {
        auto infos = grab_all_infos();

        std::scoped_lock lock{_infos_mutex};
        _infos = std::move(infos);
    }

    {
        auto const current_requests = [&]() { // IIFE
            std::scoped_lock lock{_captures_mutex};
            return _current_requests;
        }();

        for (auto const& [_, request_weak_ptr] : current_requests) // Iterate on a copy of _current_requests, because we might add elements in the latter in parallel, and this would mess up the iteration (and we don't want to lock the map, otherwise it would slow down creating a new SharedWebcam)
        {
            std::shared_ptr<WebcamRequest> const request = request_weak_ptr.lock();
            if (!request) // There is currently no request for that webcam, nothing to do
                continue;
            if (!is_plugged_in(request->id()))
            {
                request->maybe_capture() = Error_WebcamUnplugged{};
                continue;
            }
            if (std::holds_alternative<Capture>(request->maybe_capture()))
                continue; // The capture is valid, nothing to do
            // Otherwise, the webcam is plugged in but the capture is not valid, so we should try to (re)create it
            try
            {
                request->maybe_capture() = Capture{request->id(), resolutions_manager().selected_resolution(request->id())};
            }
            catch (CaptureException const& e)
            {
                request->maybe_capture() = e.capture_error;
            }
        }
    }
}

} // namespace wcam::internal