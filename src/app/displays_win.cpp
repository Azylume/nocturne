#include "app/engine_main.hpp"

#include "platform/win_displays.hpp"

namespace azl::app {

std::unique_ptr<Displays> windowDisplays(bool windowed, int virtualDisplays)
{
    return std::make_unique<WinDisplays>(windowed, virtualDisplays);
}

} // namespace azl::app
