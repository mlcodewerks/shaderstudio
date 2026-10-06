#include "workspace.h"
#include <cstdio>
#include <stdexcept>

static void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

int main()
{
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    studio_workspace layout;
    ImVec2 work_offset{}, work_inset{};
    auto frame = [&](bool editor = true) {
        ImGui::NewFrame();
        auto *viewport = ImGui::GetMainViewport();
        viewport->WorkPos = ImVec2(viewport->Pos.x + work_offset.x, viewport->Pos.y + work_offset.y);
        viewport->WorkSize = ImVec2(viewport->Size.x - work_inset.x, viewport->Size.y - work_inset.y);
        layout.draw(editor);
        require(layout.project_pos.x + layout.project_size.x <= layout.preview_pos.x,
            "Project overlaps preview");
        require(layout.preview_pos.x + layout.preview_size.x <= viewport->WorkPos.x + viewport->WorkSize.x,
            "Preview extends outside workspace");
        require(std::abs(layout.editor_pos.x + layout.editor_size.x - viewport->WorkPos.x - viewport->WorkSize.x) < 1.f,
            "Source editor is not anchored to the right edge");
        if (editor)
            require(layout.preview_pos.y + layout.preview_size.y <= layout.editor_pos.y,
                "Preview overlaps source");
        layout.prepare_preview();
        ImGui::Begin("Live preview", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
        layout.capture_preview();
        layout.begin_controls();
        ImGui::Button("Load image...");
        for (auto label : {"Test card", "Pause", "Step", "Restart", "Original"}) {
            studio_workspace::next_control(label);
            ImGui::Button(label);
            require(ImGui::GetItemRectMax().x <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x + 1.f,
                "Preview control extends outside its window");
        }
        layout.end_controls();
        ImGui::End();
        ImGui::Render();
    };
    try {
        frame(); frame();
        // The app supplies the measured title bar / control height at startup.
        layout.controls_used = layout.controls_height;
        layout.measure_preview(layout.preview_size, ImVec2(layout.preview_size.x - 16.f, layout.preview_size.y - 100.f));
        frame();
        require(layout.preview_size.x - 16.f == 640.f && layout.preview_size.y - 100.f == 480.f,
            "Default preview content is not 640 x 480");
        const float source_width = layout.editor_size.x;
        const float source_y = layout.editor_pos.y;
        layout.preview_placement_dirty = true;
        layout.preview_size = ImVec2(300, 250);
        layout.preview_pos.x += 60;
        layout.preview_pos.y += 20;
        frame(); frame();
        require(layout.preview_size.x == 300 && layout.preview_size.y == 250,
            "Floating preview resize was overwritten");
        require(layout.preview_pos.x > layout.preview_origin.x && layout.preview_pos.y > layout.preview_origin.y,
            "Floating preview cannot move independently");
        require(layout.editor_size.x == source_width && layout.editor_pos.y == source_y,
            "Floating preview resize moved the source editor");
        const auto before_move = layout.preview_pos;
        io.AddMousePosEvent(before_move.x + 80.f, before_move.y + 8.f);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMousePosEvent(before_move.x + 110.f, before_move.y + 28.f);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        require(layout.preview_pos.x >= before_move.x + 25.f && layout.preview_pos.y >= before_move.y + 15.f,
            "Preview title-bar drag does not move the window");
        const auto before_resize = layout.preview_size;
        const auto project_before_resize = layout.project_size;
        const auto editor_before_resize = layout.editor_size;
        const ImVec2 grip(layout.preview_pos.x + before_resize.x - 3.f, layout.preview_pos.y + before_resize.y - 3.f);
        io.AddMousePosEvent(grip.x, grip.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMousePosEvent(grip.x - 30.f, grip.y - 30.f);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        require(layout.preview_size.x < before_resize.x - 20.f && layout.preview_size.y < before_resize.y - 20.f,
            "Preview corner drag does not resize the window");
        require(layout.project_size.x == project_before_resize.x,
            "Bottom-right drag incorrectly resizes the project pane");
        require(layout.editor_size.y > editor_before_resize.y + 20.f,
            "Dragging preview bottom upward did not expand source editor");
        auto drag = [&](ImVec2 start, ImVec2 delta) {
            io.AddMousePosEvent(start.x, start.y);
            frame();
            io.AddMouseButtonEvent(0, true);
            frame();
            io.AddMousePosEvent(start.x + delta.x, start.y + delta.y);
            frame();
            io.AddMouseButtonEvent(0, false);
            frame();
        };
        const auto free_project = layout.project_size, free_editor = layout.editor_size;
        const auto free_preview = layout.preview_size;
        drag(ImVec2(layout.preview_pos.x + layout.preview_size.x - 1.f, layout.preview_pos.y + 80.f), ImVec2(50, 0));
        require(layout.preview_size.x > free_preview.x + 40.f, "Right edge cannot grow freely");
        require(layout.project_size.x == free_project.x && layout.editor_size.x == free_editor.x && layout.editor_size.y == free_editor.y,
            "Right edge resize moved neighboring panes");
        drag(ImVec2(layout.preview_pos.x + layout.preview_size.x - 1.f, layout.preview_pos.y + 80.f), ImVec2(-50, 0));
        require(std::abs(layout.preview_size.x - free_preview.x) < 3.f, "Right edge reverse drag did not restore size");
        const auto diagonal_project = layout.project_size, diagonal_editor = layout.editor_size;
        drag(ImVec2(layout.preview_pos.x + 2.f, layout.preview_pos.y + layout.preview_size.y - 2.f), ImVec2(-40, 30));
        require(layout.project_size.x < diagonal_project.x - 30.f && layout.editor_size.y < diagonal_editor.y - 20.f,
            "Bottom-left diagonal drag did not resize both adjacent panes");
        drag(ImVec2(layout.preview_pos.x + 2.f, layout.preview_pos.y + layout.preview_size.y - 2.f), ImVec2(40, -30));
        require(std::abs(layout.project_size.x - diagonal_project.x) < 3.f && std::abs(layout.editor_size.y - diagonal_editor.y) < 3.f,
            "Reverse diagonal drag did not restore neighboring panes");
        const auto bottom_project = layout.project_size, bottom_editor = layout.editor_size;
        drag(ImVec2(layout.preview_pos.x + 100.f, layout.preview_pos.y + layout.preview_size.y - 1.f), ImVec2(0, 35));
        require(layout.project_size.x == bottom_project.x && layout.editor_size.y < bottom_editor.y - 25.f,
            "Bottom edge does not resize only source editor");
        drag(ImVec2(layout.preview_pos.x + 100.f, layout.preview_pos.y + layout.preview_size.y - 1.f), ImVec2(0, -35));
        require(std::abs(layout.editor_size.y - bottom_editor.y) < 3.f, "Reverse bottom drag did not restore source editor");
        const float original_width = layout.project_size.x;
        io.AddMousePosEvent(original_width + 3.f, 100.f);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMousePosEvent(original_width + 83.f, 100.f);
        frame();
        require(layout.project_size.x > original_width + 70.f, "Column drag did not resize panes");
        io.AddMouseButtonEvent(0, false);
        frame();
        const float original_y = layout.editor_pos.y;
        io.AddMousePosEvent(layout.editor_pos.x + 100.f, original_y - 3.f);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMousePosEvent(layout.editor_pos.x + 100.f, original_y - 63.f);
        frame();
        require(layout.editor_pos.y < original_y - 50.f, "Source boundary did not resize workspace");
        io.AddMouseButtonEvent(0, false);
        frame();
        layout.preview_placement_dirty = true;
        layout.preview_size = ImVec2(240, 160);
        frame(); frame(); // Narrow controls wrap; short controls remain scrollable.
        const auto preview_size = layout.preview_size;
        frame(false); frame();
        require(layout.preview_size.x == preview_size.x && layout.preview_size.y == preview_size.y,
            "Hiding/reopening editor changes preview size");
        for (auto size : {ImVec2(640, 480), ImVec2(1920, 1080), ImVec2(640, 360)}) {
            io.DisplaySize = size;
            frame(); frame();
        }
        io.DisplaySize = ImVec2(1920, 1080);
        frame();
        const auto before_offset = layout.preview_pos;
        const float saved_column = layout.column, saved_row = layout.row;
        work_offset = ImVec2(17, 29);
        frame();
        require(layout.preview_pos.x == before_offset.x + 17 && layout.preview_pos.y == before_offset.y + 29,
            "Viewport movement did not preserve preview position within workspace");
        work_inset = ImVec2(34, 58); // Simulate changed host decorations / reserved menu area.
        frame();
        require(layout.column == saved_column && layout.row == saved_row,
            "Work-area changes reset the user's split proportions");
        const auto old_style = ImGui::GetStyle();
        auto &style = ImGui::GetStyle();
        style.WindowBorderSize = 4;
        style.WindowRounding = 12;
        style.WindowPadding = ImVec2(16, 12);
        style.FramePadding = ImVec2(10, 8);
        style.ItemSpacing = ImVec2(12, 8);
        style.WindowMinSize = ImVec2(280, 100);
        frame(); frame();
        require(layout.gap >= 14 && layout.min_width >= 280, "Style dimensions do not affect resize spacing and limits");
        require(layout.column == saved_column && layout.row == saved_row,
            "Style changes unexpectedly resized neighboring panes");
        io.FontGlobalScale = 1.5f;
        frame(); frame();
        require(layout.min_width >= 360, "Font scaling does not scale minimum pane widths");
        io.FontGlobalScale = 1.f;
        style = old_style;
        work_offset = work_inset = ImVec2(0, 0);
        frame(); frame();
        require(layout.column == saved_column && layout.row == saved_row,
            "Restoring viewport/style settings lost the user's layout");
        ImGui::DestroyContext();
        std::puts("PASS: anchored source, independent preview, shared boundaries, control wrapping, host resize");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        ImGui::DestroyContext();
        return 1;
    }
}
