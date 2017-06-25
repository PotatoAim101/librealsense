// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.
#ifdef _MSC_VER
#if (_MSC_VER <= 1800) // constexpr is not supported in MSVC2013
#error( "Librealsense requires MSVC2015 or later to build. Compilation will be aborted" )
#endif
#endif

#include <array>

#include "sr300.h"
#include "ds5/ds5-factory.h"
#include "backend.h"
#include "recorder.h"
#include <chrono>
#include "types.h"
#include "context.h"


template<unsigned... Is> struct seq{};
template<unsigned N, unsigned... Is>
struct gen_seq : gen_seq<N-1, N-1, Is...>{};
template<unsigned... Is>
struct gen_seq<0, Is...> : seq<Is...>{};

template<unsigned N1, unsigned... I1, unsigned N2, unsigned... I2>
constexpr std::array<char const, N1+N2-1> concat(char const (&a1)[N1], char const (&a2)[N2], seq<I1...>, seq<I2...>){
  return {{ a1[I1]..., a2[I2]... }};
}

template<unsigned N1, unsigned N2>
constexpr std::array<char const, N1+N2-1> concat(char const (&a1)[N1], char const (&a2)[N2]){
  return concat(a1, a2, gen_seq<N1-1>{}, gen_seq<N2>{});
}

// The string is used to retrieve the version embedded into .so file on Linux
constexpr auto rs2_api_version = concat("VERSION: ",RS2_API_VERSION_STR);

namespace rsimpl2
{
    context::context(backend_type type,
                     const char* filename,
                     const char* section,
                     rs2_recording_mode mode)
        : _devices_changed_callback(nullptr , [](rs2_devices_changed_callback*){})
    {

        LOG_DEBUG("Librealsense " << std::string(std::begin(rs2_api_version),std::end(rs2_api_version)));

        switch(type)
        {
        case backend_type::standard:
            _backend = uvc::create_backend();
            break;
        case backend_type::record:
            _backend = std::make_shared<uvc::record_backend>(uvc::create_backend(), filename, section, mode);
            break;
        case backend_type::playback:
            _backend = std::make_shared<uvc::playback_backend>(filename, section);

            break;
        default: throw invalid_value_exception(to_string() << "Undefined backend type " << static_cast<int>(type));
        }

       _ts = _backend->create_time_service();

       _device_watcher = _backend->create_device_watcher();
    }

    context::~context()
    {
        _device_watcher->stop(); //ensure that the device watcher will stop before the _devices_changed_callback will be deleted
    }

    std::vector<std::shared_ptr<device_info>> context::query_devices() const
    {

        uvc::devices_data devices(_backend->query_uvc_devices(), _backend->query_usb_devices(), _backend->query_hid_devices());

        return create_devices(devices);
    }

    std::vector<std::shared_ptr<device_info>> context::create_devices(uvc::devices_data devices) const
    {
        std::vector<std::shared_ptr<device_info>> list;

        auto sr300_devices = sr300_info::pick_sr300_devices(_backend, devices._uvc_devices, devices._usb_devices);
        std::copy(begin(sr300_devices), end(sr300_devices), std::back_inserter(list));

        auto ds5_devices = ds5_info::pick_ds5_devices(_backend, devices._uvc_devices, devices._usb_devices, devices._hid_devices);
        std::copy(begin(ds5_devices), end(ds5_devices), std::back_inserter(list));

        auto recovery_devices = recovery_info::pick_recovery_devices(_backend, devices._usb_devices);
        std::copy(begin(recovery_devices), end(recovery_devices), std::back_inserter(list));

        return list;
    }


    void context::on_device_changed(uvc::devices_data old, uvc::devices_data curr)
    {
        auto old_list = create_devices(old);
        auto new_list = create_devices(curr);

        if (rsimpl2::list_changed<std::shared_ptr<device_info>>(old_list, new_list, [](std::shared_ptr<device_info> first, std::shared_ptr<device_info> second) {return *first == *second; }))
        {

            std::vector<rs2_device_info> rs2_devices_info_added;
            std::vector<rs2_device_info> rs2_devices_info_removed;

            auto devices_info_removed = subtract_sets(old_list, new_list);

            for (auto i=0; i<devices_info_removed.size(); i++)
            {
                rs2_devices_info_removed.push_back({ shared_from_this(), devices_info_removed[i], (unsigned int)i });
                LOG_DEBUG("\nDevice disconnected:\n\n" << std::string(devices_info_removed[i]->get_device_data()));
            }

            auto devices_info_added = subtract_sets(new_list, old_list);
            for (auto i = 0; i<devices_info_added.size(); i++)
            {
                rs2_devices_info_added.push_back({ shared_from_this(), devices_info_added[i], (unsigned int)i });
                LOG_DEBUG("\nDevice sconnected:\n\n" << std::string(devices_info_added[i]->get_device_data()));
            }

            _devices_changed_callback->on_devices_changed(  new rs2_device_list({ shared_from_this(), rs2_devices_info_removed }),
                                                            new rs2_device_list({ shared_from_this(), rs2_devices_info_added }));
        }
    }

    double context::get_time()
    {
        return _ts->get_time();
    }

    void context::set_devices_changed_callback(devices_changed_callback_ptr callback)
    {
        _device_watcher->stop();

        _devices_changed_callback = std::move(callback);
        _device_watcher->start([this](uvc::devices_data old, uvc::devices_data curr)
        {
            on_device_changed(old, curr);
        });
    }
}
