#include "mycore/debug_ui/widgets.hpp"

#include <imgui.h>

namespace mycore::debug_ui {

void description(std::string_view text) {
    const auto* begin = text.empty() ? "" : text.data();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(begin, begin + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

} // namespace mycore::debug_ui
