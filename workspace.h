#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>

// Shared pane boundaries are handled before drawing any pane, so all three
// windows use the same geometry even on the frame of a drag.
struct studio_workspace
{
    float column = 0.29f, row = 0.42f;
    float gap = 6.f, min_width = 240.f, min_preview_height = 160.f, min_editor_height = 96.f;
    ImVec2 last_work_pos{};
    bool have_work_pos = false;
    ImVec2 preview_origin{}, preview_bounds{};
    float controls_height = 110.f, controls_used = 110.f;
    ImVec2 preview_chrome{16.f, 120.f};
    bool initialized = false, measured = false, preview_placement_dirty = true, has_editor = false;
    ImVec2 project_pos, project_size, preview_pos, preview_size, editor_pos, editor_size;
    static constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    void measure_preview(ImVec2 window, ImVec2 content)
    {
        if (std::abs(controls_height - controls_used) > 1.f) return;
        preview_chrome = ImVec2(window.x - content.x, window.y - content.y);
        if (!measured)
        {
            measured = true;
            initialized = false; // Fit startup content once; later style changes retain the user's layout.
        }
    }

    void divider(const char *name, ImVec2 pos, ImVec2 size, float &ratio, float extent, bool vertical, bool border = false)
    {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::Begin(name, nullptr, flags | ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::InvisibleButton("##divider", size);
        if (border)
            ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive :
                    ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
            ratio += (vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y) / extent;
        ImGui::End();
        ImGui::PopStyleVar(4);
    }

    void fit_preview()
    {
        preview_size.x = std::clamp(preview_size.x, std::min(min_width, preview_bounds.x), preview_bounds.x);
        preview_size.y = std::clamp(preview_size.y, std::min(min_preview_height, preview_bounds.y), preview_bounds.y);
        preview_pos.x = std::clamp(preview_pos.x, preview_origin.x, preview_origin.x + preview_bounds.x - preview_size.x);
        preview_pos.y = std::clamp(preview_pos.y, preview_origin.y, preview_origin.y + preview_bounds.y - preview_size.y);
    }

    void update_panes()
    {
        const auto *v = ImGui::GetMainViewport();
        const float width = std::max(2.f, v->WorkSize.x - gap);
        const float height = std::max(2.f, v->WorkSize.y - gap);
        project_size.x = std::round(width * column);
        preview_origin = ImVec2(v->WorkPos.x + project_size.x + gap, v->WorkPos.y);
        preview_bounds = ImVec2(width - project_size.x, has_editor ? std::round(height * row) : v->WorkSize.y);
        editor_pos = ImVec2(preview_origin.x, preview_origin.y + std::round(height * row) + gap);
        editor_size = ImVec2(preview_bounds.x, height - std::round(height * row));
    }

    void prepare_preview()
    {
        if (preview_placement_dirty)
        {
            ImGui::SetNextWindowPos(preview_pos);
            ImGui::SetNextWindowSize(preview_size);
            preview_placement_dirty = false;
        }
        const auto screen = ImGui::GetMainViewport()->WorkSize;
        const ImVec2 maximum(std::max(2.f, screen.x - gap - std::min(min_width, (screen.x - gap) * 0.4f)),
            has_editor ? std::max(2.f, screen.y - gap - std::min(min_editor_height, (screen.y - gap) * 0.4f)) : screen.y);
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(min_width, maximum.x), std::min(min_preview_height, maximum.y)), maximum);
    }

    void capture_preview()
    {
        const auto old_pos = preview_pos, old_size = preview_size;
        preview_pos = ImGui::GetWindowPos();
        preview_size = ImGui::GetWindowSize();
        const auto *v = ImGui::GetMainViewport();
        const float width = std::max(2.f, v->WorkSize.x - gap);
        const float height = std::max(2.f, v->WorkSize.y - gap);
        // Only the edge actually being resized drives its adjacent pane.
        // Moving the title bar and resizing the right edge leave neighbors alone.
        // Style/font-driven size adjustments are not pointer resize gestures.
        if (ImGui::IsMouseDown(0) && preview_size.x != old_size.x)
        {
            const float left_delta = preview_pos.x - old_pos.x;
            const float min_column = std::min(min_width / width, 0.4f);
            const float next_column = std::clamp(column + left_delta / width, min_column, 1.f - min_column);
            const float accepted_delta = std::round(width * next_column) - std::round(width * column);
            const float right = std::min(preview_pos.x + preview_size.x, v->WorkPos.x + v->WorkSize.x);
            preview_pos.x = old_pos.x + accepted_delta;
            preview_size.x = right - preview_pos.x;
            column = next_column;
        }
        if (ImGui::IsMouseDown(0) && preview_size.y != old_size.y && has_editor)
        {
            const float bottom_delta = preview_pos.y + preview_size.y - old_pos.y - old_size.y;
            const float next_row = std::clamp(row + bottom_delta / height,
                std::min(min_preview_height / height, 0.4f), 1.f - std::min(min_editor_height / height, 0.4f));
            const float accepted_delta = std::round(height * next_row) - std::round(height * row);
            preview_size.y = old_pos.y + old_size.y + accepted_delta - preview_pos.y;
            row = next_row;
        }
        update_panes();
        fit_preview();
        const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        if (pos.x != preview_pos.x || pos.y != preview_pos.y) ImGui::SetWindowPos(preview_pos);
        if (size.x != preview_size.x || size.y != preview_size.y) ImGui::SetWindowSize(preview_size);
    }

    // Keep short controls on one line when they fit; otherwise start a new row.
    static void next_control(const char *label, bool checkbox = false)
    {
        const auto &style = ImGui::GetStyle();
        const float width = ImGui::CalcTextSize(label).x + (checkbox ?
            ImGui::GetFrameHeight() + style.ItemInnerSpacing.x : 2.f * style.FramePadding.x);
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right)
            ImGui::SameLine();
    }

    void begin_controls()
    {
        controls_used = std::min(controls_height, std::max(32.f, ImGui::GetContentRegionAvail().y - 64.f));
        ImGui::BeginChild("Preview controls", ImVec2(0, controls_used));
    }

    void end_controls()
    {
        controls_height = ImGui::GetCursorPosY();
        ImGui::EndChild();
    }

    void draw(bool editor_visible)
    {
        has_editor = editor_visible;
        const auto *v = ImGui::GetMainViewport();
        const auto &style = ImGui::GetStyle();
        const float scale = std::max(0.5f, ImGui::GetFontSize() / 13.f);
        gap = std::ceil(std::max(6.f * scale, 2.f * style.WindowBorderSize + style.ItemSpacing.x * 0.5f));
        min_width = std::max(240.f * scale, style.WindowMinSize.x);
        min_preview_height = std::max(160.f * scale, style.WindowMinSize.y);
        min_editor_height = std::max(96.f * scale, style.WindowMinSize.y);
        // Window decorations, menu bars and viewport movement change WorkPos.
        // Preserve the preview's local position instead of treating this as a drag.
        if (have_work_pos && (last_work_pos.x != v->WorkPos.x || last_work_pos.y != v->WorkPos.y))
        {
            preview_pos.x += v->WorkPos.x - last_work_pos.x;
            preview_pos.y += v->WorkPos.y - last_work_pos.y;
            preview_placement_dirty = true;
        }
        last_work_pos = v->WorkPos;
        have_work_pos = true;
        const float width = std::max(2.f, v->WorkSize.x - gap);
        const float height = std::max(2.f, v->WorkSize.y - gap);
        const float min_column = std::min(min_width / width, 0.4f);
        const float min_row = std::min(min_preview_height / height, 0.4f);
        const float max_row = 1.f - std::min(min_editor_height / height, 0.4f);
        if (!initialized)
        {
            column = (width - 640.f - preview_chrome.x) / width;
            row = (480.f + preview_chrome.y) / height;
            preview_size = ImVec2(640.f + preview_chrome.x, 480.f + preview_chrome.y);
        }
        column = std::clamp(column, min_column, 1.f - min_column);
        row = std::clamp(row, min_row, max_row);
        divider("##studio_columns", ImVec2(v->WorkPos.x + std::round(width * column), v->WorkPos.y),
            ImVec2(gap, v->WorkSize.y), column, width, true, true);
        column = std::clamp(column, min_column, 1.f - min_column);
        project_pos = v->WorkPos;
        project_size = ImVec2(std::round(width * column), v->WorkSize.y);
        preview_origin = ImVec2(project_pos.x + project_size.x + gap, project_pos.y);
        if (editor_visible)
            divider("##studio_rows", ImVec2(preview_origin.x, preview_origin.y + std::round(height * row)),
                ImVec2(width - project_size.x, gap), row, height, false, true);
        row = std::clamp(row, min_row, max_row);
        preview_bounds = ImVec2(width - project_size.x, editor_visible ? std::round(height * row) : v->WorkSize.y);
        editor_pos = ImVec2(preview_origin.x, preview_origin.y + std::round(height * row) + gap);
        editor_size = ImVec2(preview_bounds.x, height - std::round(height * row));
        if (!initialized)
        {
            preview_pos = preview_origin;
            preview_placement_dirty = true;
        }
        initialized = true;
        const auto pos = preview_pos, size = preview_size;
        fit_preview();
        preview_placement_dirty |= pos.x != preview_pos.x || pos.y != preview_pos.y ||
            size.x != preview_size.x || size.y != preview_size.y;
        // Process native resize edges before drawing the neighboring panes.
        // The later Begin appends the preview controls and image to this window.
        prepare_preview();
        ImGui::Begin("Live preview", nullptr, ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        capture_preview();
        ImGui::End();
    }
};
