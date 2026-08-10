#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include <memory>
#include <optional>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <array>
#include <iterator>
#include <type_traits>

#include <imgui/imgui.h>
#include "ACU/basic_types.h"
#include "Common_Plugins/ACUPlugin.h"

struct ImGuiContext;

class ImGuiShared
{
public:
    ImGuiContext& m_ctx;
    void* (*alloc_func)(size_t sz, void* user_data) = nullptr;
    void (*free_func)(void* ptr, void* user_data) = nullptr;
    void* user_data = nullptr;
};
