#pragma once
// Reading a rendered image back to the CPU and writing it out as a PNG.
//
// This exists for more than convenience screenshots. There is no second engineer to look
// at the screen, and "does the renderer draw the right thing" is not a question unit
// tests over data structures can answer — so the build needs to be able to render a frame
// from a known camera and hand the pixels back for checking.

#include <string>

#include "gpu/buffer.hpp"
#include "gpu/image.hpp"

namespace ws {

// Copies an image to host memory and writes it as a PNG. The image must be in
// VK_IMAGE_LAYOUT_GENERAL and the device idle. Returns false and logs on failure.
bool save_image_png(Device& device, const GpuImage& image, const std::string& path);

}  // namespace ws
