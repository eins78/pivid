#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "image_buffer.h"
#include "unix_system.h"

namespace pivid {

struct DisplayDriverListing {
    std::string dev_file; 
    std::string system_path;
    std::string driver;
    std::string driver_date;
    std::string driver_desc;
    std::string driver_bus_id;
};

struct DisplayMode {
    struct Timings {
        int display, sync_start, sync_end, total;
        int doubling, sync_polarity;
        auto operator<=>(Timings const&) const = default;
    };

    std::string name;
    int pixel_khz;
    int refresh_hz;
    Timings horiz, vert;
    auto operator<=>(DisplayMode const&) const = default;
};

struct DisplayStatus {
    uint32_t connector_id;
    std::string connector_name;
    bool display_detected;
    std::vector<DisplayMode> display_modes;
    DisplayMode active_mode;
};

struct DisplayLayer {
    ImageBuffer image;
    double image_x, image_y, image_width, image_height;
    int screen_x, screen_y, screen_width, screen_height;
};

class DisplayDriver {
  public:
    virtual ~DisplayDriver() {}
    virtual std::vector<DisplayStatus> scan_outputs() = 0;
    virtual ImageBuffer make_buffer(int width, int height, int bpp) = 0;

    virtual bool ready_for_update(uint32_t connector_id) = 0;
    virtual void update_output(
        uint32_t connector_id,
        DisplayMode const& mode,
        std::vector<DisplayLayer> const& layers
    ) = 0;
};

std::vector<DisplayDriverListing> list_display_drivers(UnixSystem* sys);

std::unique_ptr<DisplayDriver> open_display_driver(
    UnixSystem* sys, std::string const& dev_file
);

std::string debug_string(DisplayDriverListing const&);
std::string debug_string(DisplayMode const&);

}  // namespace pivid
