#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct PreviewBox {
    std::array<int,3> lo, hi;
    std::string name;
};
// Render sampled original intensities with final boxes in original voxel indices.
// The release callback lets the caller discard consumed memory-mapped pages.
void write_box_preview(const std::filesystem::path& path,
                       const std::array<int,3>& size,
                       const std::vector<PreviewBox>& boxes,
                       const std::function<int(int,int,int)>& read,
                       const std::function<void()>& release, double foreground_threshold);
