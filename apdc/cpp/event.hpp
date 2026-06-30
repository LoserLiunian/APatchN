#pragma once
// Boot-stage event handlers. Mirrors ../apd/src/event.rs.
#include "defs.hpp"

namespace apd::event {
void on_post_data_fs(const OptStr &superkey);
void on_services(const OptStr &superkey);
void on_boot_completed(const OptStr &superkey);
void start_uid_listener();
} // namespace apd::event
