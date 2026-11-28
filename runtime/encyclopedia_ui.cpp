// The in-game MHGU database window: category list, name search and a wrapped detail
// pane. Data comes from runtime/encyclopedia.c; this file owns no game state.
#include "encyclopedia.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "config.h"

// Filled by runtime/overlay.cpp when the encyclopedia icon sheet joins ImGui's font atlas.
extern int g_encyclopedia_icon_id;
extern int g_encyclopedia_icon_w, g_encyclopedia_icon_h;
extern const short *g_encyclopedia_icon_rect;

namespace {
int g_lang = -1;
int g_category;
char g_query[96];
std::vector<int> g_filtered;
int g_filtered_category = -1;
int g_filtered_lang = -1;
char g_filtered_query[sizeof g_query];
int g_selected;
bool g_was_visible;

static bool is_ascii_lower(char c) { return c >= 'A' && c <= 'Z'; }

static bool contains_ci(const char *haystack, const char *needle) {
    if (!needle[0]) return true;
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < n) {
            char a = p[i], b = needle[i];
            if (!a) break;
            if (is_ascii_lower(a)) a = (char)(a - 'A' + 'a');
            if (is_ascii_lower(b)) b = (char)(b - 'A' + 'a');
            if (a != b) break;
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

static int language() {
    if (g_lang < 0) g_lang = config_language() == 2 ? 1 : 0;
    return g_lang;
}

static const char *L(const char *en, const char *zh) { return language() == 1 ? zh : en; }



static void draw_icon(ImDrawList *draw, int category, int index, ImVec2 pos, float size) {
    int cell = encyclopedia_entry_icon(category, index);
    if (cell < 0 || cell == 0xFFFF || g_encyclopedia_icon_id < 0 || !g_encyclopedia_icon_rect) return;
    ImFontAtlasRect atlas;
    if (!ImGui::GetIO().Fonts->GetCustomRect(g_encyclopedia_icon_id, &atlas)) return;
    const short *r = g_encyclopedia_icon_rect + cell * 4;
    if (!g_encyclopedia_icon_w || !g_encyclopedia_icon_h) return;
    float uw = atlas.uv1.x - atlas.uv0.x, vh = atlas.uv1.y - atlas.uv0.y;
    ImVec2 uv0(atlas.uv0.x + r[0] / (float)g_encyclopedia_icon_w * uw,
               atlas.uv0.y + r[1] / (float)g_encyclopedia_icon_h * vh);
    ImVec2 uv1(atlas.uv0.x + (r[0] + r[2]) / (float)g_encyclopedia_icon_w * uw,
               atlas.uv0.y + (r[1] + r[3]) / (float)g_encyclopedia_icon_h * vh);
    uint32_t rgb = encyclopedia_entry_color(category, index);
    draw->AddImage(ImGui::GetIO().Fonts->TexRef, pos, ImVec2(pos.x + size, pos.y + size), uv0, uv1,
                   IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255));
}

static void rebuild_filter() {
    int category = g_category;
    int lang = language();
    if (category == g_filtered_category && lang == g_filtered_lang &&
        std::strcmp(g_query, g_filtered_query) == 0)
        return;
    g_filtered_category = category;
    g_filtered_lang = lang;
    std::snprintf(g_filtered_query, sizeof g_filtered_query, "%s", g_query);
    g_filtered.clear();
    int count = encyclopedia_entries(category);
    for (int i = 0; i < count; ++i) {
        const char *name = encyclopedia_entry_name(category, i, lang);
        const char *other = encyclopedia_entry_name(category, i, 1 - lang);
        if (!g_query[0] || contains_ci(name, g_query) || contains_ci(other, g_query))
            g_filtered.push_back(i);
    }
    if (!g_query[0]) {
        // The generator sorts by the English name; sort the visible list by the selected language.
        std::stable_sort(g_filtered.begin(), g_filtered.end(), [category, lang](int a, int b) {
            return std::strcmp(encyclopedia_entry_name(category, a, lang),
                               encyclopedia_entry_name(category, b, lang)) < 0;
        });
    }
    if (g_selected >= (int)g_filtered.size()) g_selected = (int)g_filtered.size() - 1;
    if (g_selected < 0) g_selected = 0;
}

static void text_lines(const char *text) {
    const char *p = text;
    while (p && *p) {
        const char *end = std::strchr(p, '\n');
        std::string line = end ? std::string(p, (size_t)(end - p)) : std::string(p);
        if (line.empty()) ImGui::Spacing();
        else ImGui::TextWrapped("%s", line.c_str());
        p = end ? end + 1 : nullptr;
    }
}

static void draw_categories(float scale) {
    ImGui::BeginChild("##categories", ImVec2(180 * scale, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("%s", L("CATEGORIES", "分类"));
    int count = encyclopedia_categories();
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        const char *name = encyclopedia_category_name(i, language());
        if (ImGui::Selectable(name, i == g_category)) {
            g_category = i;
            g_selected = 0;
            g_filtered_category = -1;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void draw_list(float scale) {
    ImGui::BeginChild("##entries", ImVec2(285 * scale, 0), ImGuiChildFlags_Borders);
    if (g_filtered.empty()) {
        ImGui::TextDisabled("%s", L("No entries match the search.", "没有匹配的条目。"));
    } else {
        if (g_selected < 0 || g_selected >= (int)g_filtered.size()) g_selected = 0;
        ImGuiListClipper clipper;
        clipper.Begin((int)g_filtered.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                int index = g_filtered[row];
                ImGui::PushID(row);
                float icon_size = ImGui::GetFontSize() + 4;
                float row_height = ImGui::GetTextLineHeight() + 4;
                ImVec2 row_pos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", row == g_selected, 0,
                                      ImVec2(ImGui::GetContentRegionAvail().x, row_height)))
                    g_selected = row;
                ImDrawList *draw = ImGui::GetWindowDrawList();
                draw_icon(draw, g_category, index,
                          ImVec2(row_pos.x + 3, row_pos.y + (row_height - icon_size) * .5f), icon_size);
                float text_y = row_pos.y + (row_height - ImGui::GetTextLineHeight()) * .5f;
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                              ImVec2(row_pos.x + icon_size + 7, text_y),
                              ImGui::GetColorU32(ImGuiCol_Text),
                              encyclopedia_entry_name(g_category, index, language()));
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

static void draw_detail(float scale) {
    (void)scale;
    ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (g_filtered.empty()) {
        ImGui::TextDisabled("%s", L("No entries match the search.", "没有匹配的条目。"));
    } else {
        if (g_selected < 0 || g_selected >= (int)g_filtered.size()) g_selected = 0;
        int index = g_filtered[g_selected];
        float icon_size = ImGui::GetFontSize() * 1.35f;
        ImVec2 name_pos = ImGui::GetCursorScreenPos();
        int icon = encyclopedia_entry_icon(g_category, index);
        if (icon >= 0 && icon != 0xFFFF) {
            draw_icon(ImGui::GetWindowDrawList(), g_category, index, name_pos, icon_size);
            ImGui::SetCursorScreenPos(ImVec2(name_pos.x + icon_size + 6, name_pos.y));
        }
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextWrapped("%s", encyclopedia_entry_name(g_category, index, language()));
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        text_lines(encyclopedia_entry_detail(g_category, index, language()));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}
}  // namespace

extern "C" void encyclopedia_draw(float width, float height) {
    bool visible = encyclopedia_visible();
    if (!visible) {
        g_was_visible = false;
        return;
    }
    if (width <= 0 || height <= 0) return;
    bool first = !g_was_visible;
    g_was_visible = true;
    if (!encyclopedia_available()) {
        ImGui::SetNextWindowSize(ImVec2(560, 190), ImGuiCond_Appearing);
        bool keep = true;
        if (ImGui::Begin("MHGU Database##encyclopedia", &keep, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextWrapped("%s", encyclopedia_error() ? encyclopedia_error() : "No data.");
            ImGui::TextWrapped("%s", L(
                "The built-in encyclopedia data could not be read; reinstall this build.",
                "内置百科数据读取失败，请重新安装此版本。"));
        }
        ImGui::End();
        if (!keep) encyclopedia_hide();
        return;
    }

    float scale = height / 720.0f;
    ImVec2 size(std::min(width * 0.94f, 1220.0f * scale), std::min(height * 0.90f, 690.0f * scale));
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - size.x) * .5f,
                                   vp->WorkPos.y + (vp->WorkSize.y - size.y) * .5f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    if (first) ImGui::SetNextWindowFocus();
    bool keep = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("MHGU Database##encyclopedia", &keep, flags)) {
        ImGui::End();
        if (!keep) encyclopedia_hide();
        return;
    }
    if (!keep || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        encyclopedia_hide();
        ImGui::End();
        return;
    }

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * .95f);
    ImGui::TextUnformatted(L("MHGU Database", "MHGU 百科"));
    ImGui::SameLine();
    if (ImGui::SmallButton(language() == 1 ? "English" : "中文")) {
        g_lang = 1 - language();
        g_filtered_lang = -1;
        g_selected = 0;
    }
    ImGui::PopFont();
    ImGui::Separator();

    rebuild_filter();
    float sidebar_scale = height / 720.0f;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * .92f);
    ImGui::TextDisabled("%s  %d / %d", L("entries", "条目"),
                        (int)g_filtered.size(), encyclopedia_entries(g_category));
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##search", L("Search names (English or Chinese)", "搜索名称（中文或英文）"),
                                 g_query, sizeof g_query))
        g_filtered_category = -1;
    ImGui::TextDisabled("%s", L("Names only", "仅名称"));

    draw_categories(sidebar_scale);
    ImGui::SameLine();
    draw_list(sidebar_scale);
    ImGui::SameLine();
    draw_detail(sidebar_scale);
    ImGui::End();
}
