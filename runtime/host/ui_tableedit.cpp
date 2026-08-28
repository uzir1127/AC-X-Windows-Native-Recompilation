/* The parameter tables, edited in flight.
 *
 * Every float in a table's leading block gets a control, not only the ones
 * with a known meaning.  What is known is shown in three tiers -- a traced
 * reader, an inference from the upgrade part that modifies the field, or
 * nothing -- and the second is never presented as the first.
 */

#include "imgui.h"
#include "ui_overlay.h"

extern "C" {
#include "pspreco.h"
#include "../hle/datatables.h"
#include "../hle/hooks.h"
extern u8 *g_ram;
}

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_TABLES 16

static bool  s_open;
static int   s_tab;                       /* which table */
static int   s_row[MAX_TABLES];
static bool  s_follow_live = true;
static bool  s_only_named;
static bool  s_only_changed;
static char  s_mod_name[64] = "mytweaks";
static char  s_saved_to[512];
static float s_speed = 0.05f;
static char  s_filter[64];

void plane_ui_toggle(void)  { s_open = !s_open; }
int  plane_ui_visible(void) { return s_open ? 1 : 0; }

static const char *tier_label(const TableField *f)
{
    if (!f) return "unknown";
    if (f->traced) return "traced from a reader";
    if (f->note && f->note[0]) return "inferred, not traced";
    return "unknown";
}

static ImVec4 tier_colour(const TableField *f)
{
    if (f && f->traced)             return ImVec4(0.55f, 0.85f, 0.55f, 1);
    if (f && f->note && f->note[0]) return ImVec4(0.90f, 0.80f, 0.45f, 1);
    return ImVec4(0.60f, 0.60f, 0.65f, 1);
}

static void draw_field(const AcxTable *t, unsigned off,
                       const TableField *known, const TableField *fb)
{
    const TableField *f = known ? known : fb;
    int row = s_row[s_tab];
    int stock_ok = 0;
    bool is_str = (f->type == PF_STR);
    double now = is_str ? 0.0 : table_get(g_ram, t, row, f);
    double stock = is_str ? 0.0 : table_get_stock(t, row, f, &stock_ok);
    bool differs = stock_ok && fabs(now - stock) > 1e-9;
    char snow[64] = "", sstock[64] = "";

    if (is_str) {
        table_get_str(g_ram, t, row, f, snow, (int)sizeof snow);
        table_get_stock_str(t, row, f, sstock, (int)sizeof sstock);
        stock_ok = sstock[0] != 0;
        differs = strcmp(snow, sstock) != 0;
    }

    if (s_only_named && !(known && known->name[0])) return;
    if (s_only_changed && !differs) return;

    ImGui::PushID((int)off);
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::TextDisabled("+%-3u", off);

    ImGui::TableNextColumn();
    if (known && known->name[0])
        ImGui::TextColored(tier_colour(known), "%s", known->name);
    else if (f->type == PF_F32)
        ImGui::TextColored(tier_colour(known), "float %u", off / 4);
    else
        ImGui::TextColored(tier_colour(known), "byte %u", off);
    if (ImGui::IsItemHovered() && known && known->note && known->note[0])
        ImGui::SetTooltip("%s\n(%s)", known->note, tier_label(known));

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (f->type == PF_STR) {
        char buf[64];
        table_get_str(g_ram, t, row, f, buf, (int)sizeof buf);
        if (ImGui::InputText("##s", buf, f->len < sizeof buf ? f->len : sizeof buf))
            table_set_str(g_ram, t, row, f, buf);
    } else if (f->type == PF_F32) {
        float v = (float)now;
        float mag = fabsf(v) > 1.0f ? fabsf(v) : 1.0f;
        if (ImGui::DragFloat("##v", &v, s_speed * mag * 0.02f, 0, 0, "%.5g"))
            table_set(g_ram, t, row, f, (double)v);
    } else {
        int v = (int)now;
        if (ImGui::DragInt("##v", &v, 0.25f))
            table_set(g_ram, t, row, f, (double)v);
    }

    ImGui::TableNextColumn();
    /* One speed unit is 43.2 km/h and one world unit is 0.2 m; see
     * docs/formats/PRM.md. */
    if (known && known->scale != 0.0)
        ImGui::TextDisabled("%.4g %s", now * known->scale, known->unit);
    else
        ImGui::TextDisabled(" ");

    ImGui::TableNextColumn();
    if (is_str) {
        if (differs) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1), "%s", sstock);
        else         ImGui::TextDisabled("%s", sstock);
    } else if (!stock_ok) ImGui::TextDisabled("-");
    else if (differs) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1), "%.6g", stock);
    else              ImGui::TextDisabled("%.6g", stock);

    ImGui::PopID();
}

static bool begin_grid(void)
{
    if (!ImGui::BeginTable("f", 5,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                           | ImGuiTableFlags_ScrollY
                           | ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("off",   ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("real",  ImGuiTableColumnFlags_WidthFixed, 96);
    ImGui::TableSetupColumn("stock", ImGuiTableColumnFlags_WidthFixed, 88);
    ImGui::TableHeadersRow();
    return true;
}

/* The leading float block, every slot of it. */
static void draw_floats(const AcxTable *t)
{
    static const TableField blank = { 0, PF_F32, "", "", 0, 0.0, NULL, 0 };
    TableField fb = blank;
    if (!begin_grid()) return;
    for (unsigned off = 0; off < t->float_block; off += 4) {
        const TableField *k = table_field_at(t, off);
        fb.off = off;
        draw_field(t, off, (k && k->type == PF_F32) ? k : NULL, &fb);
    }
    ImGui::EndTable();
}

/* A one-field string table (1243 menu labels) is unusable as a row picker, so
 * it gets a filtered list with every row on the page.  The clipper builds only
 * the visible rows. */
static void draw_list(const AcxTable *t, int rows)
{
    const TableField *f = &t->fields[0];
    char buf[64], stock[64];

    ImGui::SetNextItemWidth(240);
    ImGui::InputTextWithHint("##filter", "filter", s_filter, sizeof s_filter);
    ImGui::SameLine();
    ImGui::TextDisabled("%d rows", rows);

    if (!ImGui::BeginTable("l", 3, ImGuiTableFlags_RowBg
                           | ImGuiTableFlags_BordersInnerV
                           | ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("row",   ImGuiTableColumnFlags_WidthFixed, 52);
    ImGui::TableSetupColumn("text",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("stock", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableHeadersRow();

    /* Filtering breaks the clipper's uniform-row assumption, so the filtered
     * case walks every row; the common case stays clipped. */
    if (s_filter[0]) {
        for (int i = 0; i < rows; i++) {
            table_get_str(g_ram, t, i, f, buf, (int)sizeof buf);
            if (!strstr(buf, s_filter)) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%d", i);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##s", buf, f->len))
                table_set_str(g_ram, t, i, f, buf);
            ImGui::TableNextColumn();
            table_get_stock_str(t, i, f, stock, (int)sizeof stock);
            if (strcmp(buf, stock) != 0)
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1), "%s", stock);
            else ImGui::TextDisabled("%s", stock);
            ImGui::PopID();
        }
    } else {
        ImGuiListClipper clip;
        clip.Begin(rows);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                table_get_str(g_ram, t, i, f, buf, (int)sizeof buf);
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("%d", i);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##s", buf, f->len))
                    table_set_str(g_ram, t, i, f, buf);
                ImGui::TableNextColumn();
                table_get_stock_str(t, i, f, stock, (int)sizeof stock);
                if (strcmp(buf, stock) != 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1), "%s", stock);
                else ImGui::TextDisabled("%s", stock);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndTable();
}

/* Everything the field list names at or past the float block. */
static void draw_rest(const AcxTable *t)
{
    if (!begin_grid()) return;
    for (int i = 0; i < t->field_count; i++) {
        const TableField *k = &t->fields[i];
        if (k->off < t->float_block) continue;
        draw_field(t, k->off, k, k);
    }
    ImGui::EndTable();
}

void plane_ui_draw(void)
{
    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(700, 580), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(600, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Parameter tables  (F5 closes)", NULL,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* --- which table ------------------------------------------------- */
    for (int i = 0; i < g_acx_table_count && i < MAX_TABLES; i++) {
        if (i % 5) ImGui::SameLine();
        bool on = (s_tab == i);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(g_acx_tables[i].title, ImVec2(96, 0))) s_tab = i;
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", g_acx_tables[i].record);
    }

    const AcxTable *t = &g_acx_tables[s_tab];
    int rows = 0;
    u32 base = table_base(g_ram, t, &rows);
    if (!base) {
        ImGui::TextWrapped("%s is not registered yet. MAIN.bin arrives a few "
                           "seconds into boot.", t->record);
        ImGui::End();
        return;
    }
    if (s_row[s_tab] >= rows) s_row[s_tab] = rows - 1;

    int live_row = plane_live_row();
    u32 live = plane_live();
    if (t->live_kind == LIVE_COPY && s_follow_live && live_row >= 0 && live_row < rows)
        s_row[s_tab] = live_row;

    if (!t->list_view) {
        ImGui::SetNextItemWidth(170);
        ImGui::SliderInt("row", &s_row[s_tab], 0, rows - 1);
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%s  %d rows x %u bytes at %08x",
                        t->record, rows, t->stride, (unsigned)base);

    /* --- how an edit reaches the game -------------------------------- */
    if (t->live_kind == LIVE_COPY) {
        ImGui::Checkbox("follow the aircraft in the air", &s_follow_live);
        if (live && live_row == s_row[s_tab]) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1),
                               "LIVE at %08x -- edits take effect immediately",
                               (unsigned)live);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->live_note);
        } else if (!hook_available()) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1),
                               "Not a moddable build -- edits reach the table "
                               "but not the aircraft in the air.");
        } else if (live_row >= 0) {
            ImGui::TextDisabled("The aircraft in the air is row %d; edits here "
                                "apply on its next spawn.", live_row);
        } else {
            ImGui::TextDisabled("Nothing spawned yet; edits apply on the next "
                                "spawn.");
        }
        if (s_row[s_tab] == 40 || s_row[s_tab] == 41)
            ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1),
                               "Row %d is not an aircraft: rows 40 and 41 are "
                               "the ceiling and floor the parts pass clamps "
                               "against.", s_row[s_tab]);
    } else if (t->live_kind == LIVE_REOPEN) {
        ImGui::TextColored(ImVec4(0.90f, 0.80f, 0.45f, 1),
                           "LIVE on reopen -- close and reopen the menu to see it");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->live_note);
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "LIVE in place");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->live_note);
    }

    ImGui::Separator();
    ImGui::Checkbox("named only", &s_only_named);
    ImGui::SameLine();
    ImGui::Checkbox("changed only", &s_only_changed);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("drag speed", &s_speed, 0.005f, 0.5f, "%.3f");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("green  a reader was traced\n"
                          "amber  inferred from which upgrade part modifies it\n"
                          "grey   nothing known.\n"
                          "       Change one and fly; that is how they get named.");

    if (t->list_view) {
        draw_list(t, rows);
    } else if (t->float_block) {
        if (ImGui::BeginTabBar("tt")) {
            char lbl[64];
            snprintf(lbl, sizeof lbl, "%u floats", t->float_block / 4);
            if (ImGui::BeginTabItem(lbl))  { draw_floats(t); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Rest")) { draw_rest(t); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    } else {
        draw_rest(t);
    }

    ImGui::Separator();
    if (ImGui::Button("Revert this row")) table_revert_row(g_ram, t, s_row[s_tab]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputText("mod", s_mod_name, sizeof s_mod_name);
    ImGui::SameLine();
    if (ImGui::Button("Save this table as a mod")) {
        if (table_save_mod(g_ram, t, s_mod_name, s_saved_to,
                           sizeof s_saved_to) != 0)
            snprintf(s_saved_to, sizeof s_saved_to, "save failed");
    }
    if (s_saved_to[0]) ImGui::TextDisabled("%s", s_saved_to);
    else ImGui::TextDisabled("writes the whole table as a drop-in replacement");

    ImGui::End();
}
