#include "PresetExplorerDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Tab.hpp"
#include "UnsavedChangesDialog.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TabCtrl.hpp"

#include <wx/statline.h>
#include <wx/checkbox.h>

namespace Slic3r {
namespace GUI {

static wxColour card_bg(bool dark) { return dark ? wxColour(50, 50, 50) : wxColour(248, 248, 248); }
static wxColour card_border(bool dark) { return dark ? wxColour(70, 70, 70) : wxColour(220, 220, 220); }
static wxColour text_primary(bool dark) { return dark ? wxColour(250, 250, 250) : wxColour(38, 46, 48); }
static wxColour text_secondary(bool dark) { return dark ? wxColour(180, 180, 180) : wxColour(107, 107, 107); }
static wxColour badge_bg(bool dark) { return dark ? wxColour(60, 60, 60) : wxColour(230, 230, 230); }

// ============================================================
// Data extraction
// ============================================================

std::string PresetExplorerDialog::parse_nozzle(const std::string &inherits) const
{
    auto pos = inherits.find(" nozzle");
    if (pos != std::string::npos && pos >= 3) {
        auto space = inherits.rfind(' ', pos - 1);
        if (space != std::string::npos)
            return inherits.substr(space + 1, pos - space - 1);
    }
    return "0.4";
}

std::string PresetExplorerDialog::parse_material_type(const Preset &preset) const
{
    auto *opt = preset.config.option<ConfigOptionStrings>("filament_type");
    if (opt && !opt->values.empty())
        return opt->values[0];
    return "";
}

PresetCardData PresetExplorerDialog::extract_card_data(const Preset &preset, PresetCollection *collection)
{
    PresetCardData d;
    d.name = preset.name;
    d.inherits = preset.inherits();
    d.is_compatible = preset.is_compatible;

    if (collection->type() == Preset::TYPE_PRINT) {
        d.nozzle = parse_nozzle(d.inherits);

        auto get_str = [&](const std::string &key) -> std::string {
            auto *opt = preset.config.option<ConfigOptionString>(key);
            return opt ? opt->value : "";
        };
        auto get_float = [&](const std::string &key) -> std::string {
            auto *opt = preset.config.option<ConfigOptionFloat>(key);
            return opt ? std::to_string(opt->value) : "";
        };
        auto get_int = [&](const std::string &key) -> std::string {
            auto *opt = preset.config.option<ConfigOptionInt>(key);
            return opt ? std::to_string(opt->value) : "";
        };
        auto get_percent = [&](const std::string &key) -> std::string {
            auto *opt = preset.config.option<ConfigOptionPercent>(key);
            return opt ? std::to_string((int)opt->value) + "%" : "";
        };

        d.layer_height = get_float("layer_height");
        if (!d.layer_height.empty()) {
            // Trim to 2 decimal places
            auto dot = d.layer_height.find('.');
            if (dot != std::string::npos && d.layer_height.size() > dot + 3)
                d.layer_height = d.layer_height.substr(0, dot + 3);
            d.layer_height += "mm";
        }
        d.walls = get_int("wall_loops");
        d.infill = get_percent("sparse_infill_density");

        auto *post = preset.config.option<ConfigOptionStrings>("post_process");
        if (post && !post->values.empty()) {
            d.has_postprocess = true;
            // Show just the script filename
            for (auto &v : post->values) {
                if (!v.empty()) {
                    auto slash = v.rfind('/');
                    auto space = v.find(' ', slash != std::string::npos ? slash : 0);
                    d.postprocess_script = v.substr(slash != std::string::npos ? slash + 1 : 0,
                        space != std::string::npos ? space - (slash != std::string::npos ? slash + 1 : 0) : std::string::npos);
                    break;
                }
            }
        }
    }
    else if (collection->type() == Preset::TYPE_FILAMENT) {
        d.material_type = parse_material_type(preset);

        auto *temp = preset.config.option<ConfigOptionInts>("nozzle_temperature");
        if (temp && !temp->values.empty())
            d.nozzle_temp = std::to_string(temp->values[0]) + "°C";

        auto *bed = preset.config.option<ConfigOptionInts>("bed_temperature");
        if (bed && !bed->values.empty())
            d.bed_temp = std::to_string(bed->values[0]) + "°C";

        auto *density = preset.config.option<ConfigOptionFloats>("filament_density");
        if (density && !density->values.empty())
            d.density = std::to_string(density->values[0]).substr(0, 4) + " g/cm³";

        auto *cost = preset.config.option<ConfigOptionFloats>("filament_cost");
        if (cost && !cost->values.empty())
            d.cost = "$" + std::to_string(cost->values[0]).substr(0, 5);
    }

    return d;
}

void PresetExplorerDialog::init_preset_data()
{
    auto bundle = wxGetApp().preset_bundle;
    m_all_data.clear();
    m_nozzle_counts.clear();
    m_base_counts.clear();
    m_material_counts.clear();

    for (PresetCollection *collection : {(PresetCollection *)&bundle->printers, &bundle->filaments, &bundle->prints}) {
        std::vector<PresetCardData> cards;
        for (auto &preset : *collection) {
            if (!preset.is_user()) continue;
            auto data = extract_card_data(preset, collection);
            // Update filter counts for process tab
            if (collection->type() == Preset::TYPE_PRINT) {
                m_nozzle_counts[data.nozzle]++;
                m_base_counts[data.inherits]++;
            }
            if (collection->type() == Preset::TYPE_FILAMENT) {
                m_material_counts[data.material_type]++;
            }
            cards.push_back(std::move(data));
        }
        m_all_data.push_back(std::move(cards));
    }
}

// ============================================================
// Constructor & main layout
// ============================================================

PresetExplorerDialog::PresetExplorerDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Preset Explorer"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    bool dark = wxGetApp().dark_mode();
    SetBackgroundColour(dark ? wxColour(30, 30, 30) : *wxWHITE);
    SetMinSize({FromDIP(900), FromDIP(600)});

    int em = em_unit();

    // Tab control
    m_tab_ctrl = new TabCtrl(this, wxID_ANY);
    m_tab_ctrl->SetFont(Label::Body_14);
    m_tab_ctrl->AppendItem("");
    m_tab_ctrl->AppendItem("");
    m_tab_ctrl->AppendItem("");
    m_tab_ctrl->SetItemText(0, _L("Printer"));
    m_tab_ctrl->SetItemText(1, _L("Filament"));
    m_tab_ctrl->SetItemText(2, _L("Process"));
    m_tab_ctrl->SelectItem(2);  // default to Process
    m_tab_ctrl->SetItemBold(2, true);
    m_tab_ctrl->Bind(wxEVT_TAB_SEL_CHANGED, [this](auto &evt) { on_tab_changed(evt.GetInt()); });

    // Top bar: search + sort
    m_search = new TextInput(this, "", "", "im_text_search");
    m_search->SetSize({FromDIP(400), FromDIP(24)});
    m_search->SetCornerRadius(FromDIP(12));
    m_search->Bind(wxEVT_TEXT, [this](auto &evt) { on_search(evt.GetString()); });

    m_sort_choice = new wxChoice(this, wxID_ANY);
    // Not used anymore — replaced by column headers
    m_sort_choice->Hide();

    wxBoxSizer *top_bar = new wxBoxSizer(wxHORIZONTAL);
    top_bar->Add(m_search, 1, wxALIGN_CENTER);

    // Filter panel (left side)
    m_filter_panel = new wxPanel(this);
    m_filter_panel->SetMinSize({FromDIP(180), -1});
    m_filter_panel->SetBackgroundColour(dark ? wxColour(40, 40, 40) : wxColour(245, 245, 245));
    m_filter_sizer = new wxBoxSizer(wxVERTICAL);
    m_filter_panel->SetSizer(m_filter_sizer);

    // Preset list (right side, scrollable)
    m_list_panel = new wxScrolledWindow(this);
    m_list_panel->SetScrollRate(0, 5);
    m_list_panel->SetBackgroundColour(GetBackgroundColour());
    m_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_list_panel->SetSizer(m_list_sizer);

    // Empty state panel
    m_empty_panel = new wxPanel(this);
    m_empty_panel->SetForegroundColour(wxColor("#A0A0A0"));
    {
        wxSizer *esizer = new wxBoxSizer(wxVERTICAL);
        wxStaticBitmap *bitmap = new wxStaticBitmap(m_empty_panel, wxID_ANY,
            create_scaled_bitmap(dark ? "preset_empty_dark" : "preset_empty", this, 150));
        esizer->Add(bitmap, 0, wxALIGN_CENTER | wxTOP, FromDIP(70));
        auto *elabel = new Label(m_empty_panel, _L("No content"));
        elabel->SetBackgroundColour(GetBackgroundColour());
        elabel->SetForegroundColour(m_empty_panel->GetForegroundColour());
        esizer->Add(elabel, 0, wxALIGN_CENTER);
        m_empty_panel->SetSizer(esizer);
        m_empty_panel->Hide();
    }

    // Content area: filter | list/empty
    // Column header bar
    auto *col_header = new wxPanel(this);
    col_header->SetBackgroundColour(dark ? wxColour(55, 55, 55) : wxColour(235, 235, 235));
    auto *col_sizer = new wxBoxSizer(wxHORIZONTAL);
    int chip_width = FromDIP(55);

    auto make_col_label = [&](const wxString &label, const std::string &sort_key, int width) -> wxStaticText* {
        wxString display = label;
        if (m_sort_by == sort_key)
            display += m_sort_ascending ? " \u25B2" : " \u25BC";
        auto *lbl = new wxStaticText(col_header, wxID_ANY, display,
            wxDefaultPosition, width > 0 ? wxSize(width, -1) : wxDefaultSize,
            width > 0 ? wxALIGN_RIGHT : 0);
        lbl->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        lbl->SetForegroundColour(text_secondary(dark));
        lbl->SetCursor(wxCursor(wxCURSOR_HAND));
        lbl->Bind(wxEVT_LEFT_UP, [this, sort_key](auto &) {
            if (m_sort_by == sort_key)
                m_sort_ascending = !m_sort_ascending;
            else {
                m_sort_by = sort_key;
                m_sort_ascending = true;
            }
            apply_filters();
            rebuild_visible_list();
            // Rebuild column headers to update arrow
            // (handled by next rebuild cycle)
        });
        return lbl;
    };

    // Spacer for checkbox + expand area
    col_sizer->AddSpacer(FromDIP(50));
    col_sizer->Add(make_col_label(_L("Name"), "name", 0), 1, wxALIGN_CENTER);
    col_sizer->AddStretchSpacer();

    if (m_collection == 2) {  // Process columns
        col_sizer->Add(make_col_label(_L("Layer"), "layer_height", chip_width), 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
        col_sizer->Add(make_col_label(_L("Walls"), "walls", chip_width), 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
        col_sizer->Add(make_col_label(_L("Infill"), "infill", chip_width), 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
        col_sizer->Add(make_col_label(_L("Nozzle"), "nozzle", chip_width), 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
        auto *pp_lbl = new wxStaticText(col_header, wxID_ANY, _L("PP"), wxDefaultPosition, wxSize(chip_width, -1), wxALIGN_RIGHT);
        pp_lbl->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        pp_lbl->SetForegroundColour(text_secondary(dark));
        col_sizer->Add(pp_lbl, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(10));
    } else if (m_collection == 1) {  // Filament columns
        col_sizer->Add(make_col_label(_L("Material"), "material", chip_width), 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
        col_sizer->Add(make_col_label(_L("Temp"), "nozzle_temp", chip_width), 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
        auto *cost_lbl = new wxStaticText(col_header, wxID_ANY, _L("Cost"), wxDefaultPosition, wxSize(chip_width, -1), wxALIGN_RIGHT);
        cost_lbl->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        cost_lbl->SetForegroundColour(text_secondary(dark));
        col_sizer->Add(cost_lbl, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(10));
    }

    col_header->SetSizer(col_sizer);

    wxBoxSizer *right_sizer = new wxBoxSizer(wxVERTICAL);
    right_sizer->Add(col_header, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
    right_sizer->Add(m_list_panel, 1, wxEXPAND);
    right_sizer->Add(m_empty_panel, 1, wxEXPAND);

    wxBoxSizer *content = new wxBoxSizer(wxHORIZONTAL);
    content->Add(m_filter_panel, 0, wxEXPAND | wxRIGHT, FromDIP(10));
    content->Add(right_sizer, 1, wxEXPAND);

    // Bottom bar: status + actions
    m_status_text = new wxStaticText(this, wxID_ANY, "");
    m_status_text->SetForegroundColour(text_secondary(dark));

    auto *select_all_check = new ::CheckBox(this);
    m_select_all = select_all_check;
    auto *select_all_label = new wxStaticText(this, wxID_ANY, _L("Select All"));
    select_all_label->SetForegroundColour(text_primary(dark));
    select_all_check->Bind(wxEVT_TOGGLEBUTTON, [this, select_all_check](auto &evt) {
        evt.Skip();
        bool checked = evt.IsChecked();
        for (size_t idx : m_visible_indices) {
            auto &name = m_all_data[m_collection][idx].name;
            if (checked) m_checked_presets.insert(name);
            else         m_checked_presets.erase(name);
            auto it = m_card_checks.find(name);
            if (it != m_card_checks.end()) it->second->SetValue(checked);
        }
        m_btn_delete->Enable(!m_checked_presets.empty());
        m_btn_compare->Enable(m_checked_presets.size() == 2);
        m_status_text->SetLabel(m_checked_presets.empty()
            ? wxString::Format(_L("%zu presets"), m_visible_indices.size())
            : wxString::Format(_L("%zu selected"), m_checked_presets.size()));
    });

    m_btn_compare = new Button(this, _L("Compare"));
    m_btn_compare->SetBorderColorNormal(wxColor("#00AE42"));
    m_btn_compare->SetTextColorNormal(wxColor("#00AE42"));
    m_btn_compare->Enable(false);
    m_btn_compare->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](auto &) {
        on_compare_checked();
        m_btn_compare->SetBackgroundColor(GetBackgroundColour());
        m_btn_delete->SetBackgroundColor(GetBackgroundColour());
        m_btn_compare->Refresh();
        m_btn_delete->Refresh();
    });

    m_btn_delete = new Button(this, _L("Delete"));
    m_btn_delete->SetBorderColorNormal(wxColor("#D01B1B"));
    m_btn_delete->SetTextColorNormal(wxColor("#D01B1B"));
    m_btn_delete->Enable(false);
    m_btn_delete->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](auto &) { on_delete_checked(); });

    wxBoxSizer *bottom = new wxBoxSizer(wxHORIZONTAL);
    bottom->Add(select_all_check, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));
    bottom->Add(select_all_label, 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));
    bottom->Add(m_status_text, 1, wxALIGN_CENTER | wxLEFT, FromDIP(10));
    bottom->Add(m_btn_compare, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(8));
    bottom->Add(m_btn_delete, 0, wxALIGN_CENTER);

    // Main sizer
    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_tab_ctrl, 0, wxALIGN_CENTER | wxALL, FromDIP(15));
    main_sizer->Add(top_bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));
    main_sizer->Add(content, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    main_sizer->Add(bottom, 0, wxEXPAND | wxALL, FromDIP(15));

    SetSizer(main_sizer);

    // Load data and build UI
    init_preset_data();
    on_tab_changed(2);  // start on Process tab

    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    Fit();
    CenterOnParent();
}

// ============================================================
// Card creation
// ============================================================

wxPanel *PresetExplorerDialog::create_preset_card(wxWindow *parent, const PresetCardData &data)
{
    bool dark = wxGetApp().dark_mode();
    auto *card = new wxPanel(parent);
    card->SetBackgroundColour(card_bg(dark));
    card->SetMinSize({-1, FromDIP(52)});

    auto *hsizer = new wxBoxSizer(wxHORIZONTAL);

    // Checkbox
    auto *check = new ::CheckBox(card);
    check->Bind(wxEVT_TOGGLEBUTTON, [this, name = data.name](auto &evt) {
        evt.Skip();
        on_preset_checked(name, evt.IsChecked());
    });
    m_card_checks[data.name] = check;

    // Expand button — shows +/- based on expanded state
    bool is_expanded = (m_expanded_preset == data.name);
    auto *expand_btn = new wxStaticText(card, wxID_ANY, is_expanded ? "-" : "+");
    expand_btn->SetFont(wxFont(14, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
    expand_btn->SetForegroundColour(wxColour("#00AE42"));
    expand_btn->SetCursor(wxCursor(wxCURSOR_HAND));
    expand_btn->Bind(wxEVT_LEFT_UP, [this, name = data.name](auto &) { on_preset_expand(name); });

    // Name
    auto *name_label = new Label(card, from_u8(data.name));
    name_label->SetFont(Label::Body_13);
    name_label->SetForegroundColour(data.is_compatible ? text_primary(dark) : text_secondary(dark));

    // Inherits badge
    auto *badge = new wxStaticText(card, wxID_ANY, from_u8(data.inherits));
    badge->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
    badge->SetForegroundColour(text_secondary(dark));
    badge->SetBackgroundColour(badge_bg(dark));

    // Right side: key info in fixed-width columns
    auto *info_sizer = new wxBoxSizer(wxHORIZONTAL);
    int chip_width = FromDIP(55);
    auto add_chip = [&](const std::string &text) {
        auto *chip = new wxStaticText(card, wxID_ANY, from_u8(text), wxDefaultPosition, wxSize(chip_width, -1), wxALIGN_RIGHT);
        chip->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        chip->SetForegroundColour(text_secondary(dark));
        info_sizer->Add(chip, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
    };

    if (m_collection == 2) {  // Process
        add_chip(data.layer_height);
        add_chip(data.walls.empty() ? "" : data.walls + "w");
        add_chip(data.infill);
        add_chip(data.nozzle + "mm");
        add_chip(data.has_postprocess ? "PP" : "");
    } else if (m_collection == 1) {  // Filament
        add_chip(data.material_type);
        add_chip(data.nozzle_temp);
        add_chip(data.cost);
    }

    hsizer->Add(check, 0, wxALIGN_CENTER | wxLEFT, FromDIP(10));
    hsizer->Add(name_label, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(8));
    hsizer->Add(badge, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(4));
    hsizer->Add(expand_btn, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(8));
    hsizer->AddStretchSpacer();
    hsizer->Add(info_sizer, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(10));

    auto *vsizer = new wxBoxSizer(wxVERTICAL);
    vsizer->Add(hsizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));

    card->SetSizer(vsizer);
    return card;
}

wxPanel *PresetExplorerDialog::create_expanded_details(wxWindow *parent, const PresetCardData &data)
{
    bool dark = wxGetApp().dark_mode();
    auto *panel = new wxPanel(parent);
    panel->SetBackgroundColour(dark ? wxColour(45, 45, 45) : wxColour(240, 240, 240));

    auto *grid = new wxFlexGridSizer(2, FromDIP(4), FromDIP(20));
    grid->AddGrowableCol(1, 1);

    // Find the preset and its parent to compute overrides
    auto bundle = wxGetApp().preset_bundle;
    Preset::Type types[] = {Preset::TYPE_PRINTER, Preset::TYPE_FILAMENT, Preset::TYPE_PRINT};
    PresetCollection *collection = nullptr;
    if (m_collection == 0) collection = &bundle->printers;
    else if (m_collection == 1) collection = &bundle->filaments;
    else collection = &bundle->prints;

    auto *preset = collection->find_preset(data.name, false);
    if (preset) {
        const Preset *parent_preset = collection->get_preset_base(*preset);

        if (parent_preset && parent_preset != preset) {
            auto diff_keys = preset->config.diff(parent_preset->config);

            // Skip internal/meta keys
            static const std::set<std::string> skip_keys = {
                "inherits", "from", "name", "print_settings_id", "filament_settings_id",
                "printer_settings_id", "version", "print_extruder_id", "print_extruder_variant",
                "filament_extruder_variant", "compatible_printers", "compatible_printers_condition",
                "compatible_prints", "compatible_prints_condition"
            };

            auto add_row = [&](const std::string &key, const std::string &value) {
                auto *lbl = new wxStaticText(panel, wxID_ANY, from_u8(key));
                lbl->SetForegroundColour(text_secondary(dark));
                lbl->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
                auto *val = new wxStaticText(panel, wxID_ANY, from_u8(value));
                val->SetForegroundColour(text_primary(dark));
                val->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
                grid->Add(lbl, 0, wxALIGN_RIGHT);
                grid->Add(val, 1, wxEXPAND);
            };

            int shown = 0;
            for (auto &key : diff_keys) {
                if (skip_keys.count(key)) continue;
                std::string val = preset->config.opt_serialize(key);
                if (val.length() > 80) val = val.substr(0, 77) + "...";
                add_row(key, val);
                shown++;
            }

            if (shown == 0) {
                add_row("", "(no overrides — identical to base)");
            }
        } else {
            auto *lbl = new wxStaticText(panel, wxID_ANY, _L("Root preset (no parent)"));
            lbl->SetForegroundColour(text_secondary(dark));
            grid->Add(lbl, 0);
            grid->AddSpacer(0);
        }
    }

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(12));
    panel->SetSizer(sizer);
    return panel;
}

// ============================================================
// Filter panel
// ============================================================

void PresetExplorerDialog::build_filter_panel(wxWindow *parent, wxSizer *sizer)
{
    bool dark = wxGetApp().dark_mode();
    sizer->Clear(true);

    auto add_section = [&](const wxString &title) {
        auto *label = new wxStaticText(parent, wxID_ANY, title);
        label->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
        label->SetForegroundColour(text_primary(dark));
        sizer->Add(label, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
    };

    // Compatible only
    auto *compat_cb = new wxCheckBox(parent, wxID_ANY, _L("Compatible only"));
    compat_cb->SetValue(m_filter_compatible_only);
    compat_cb->SetForegroundColour(text_primary(dark));
    compat_cb->Bind(wxEVT_CHECKBOX, [this](auto &evt) {
        m_filter_compatible_only = evt.IsChecked();
        on_filter_changed();
    });
    sizer->Add(compat_cb, 0, wxALL, FromDIP(8));

    if (m_collection == 2) {  // Process
        // Nozzle filter — checked = included, unchecked = excluded
        // All checked by default (m_filter_nozzles empty = show all)
        add_section(_L("Nozzle"));
        std::set<std::string> all_nozzles;
        for (auto &kv : m_nozzle_counts) all_nozzles.insert(kv.first);

        for (auto &kv : m_nozzle_counts) {
            wxString text = from_u8(kv.first + "mm") + wxString::Format(" (%d)", kv.second);
            auto *cb = new wxCheckBox(parent, wxID_ANY, text);
            bool is_active = m_filter_nozzles.empty() || m_filter_nozzles.count(kv.first) > 0;
            cb->SetValue(is_active);
            cb->SetForegroundColour(text_primary(dark));
            // Disable if this is the only one selected — grayed out, can't uncheck
            if (is_active && m_filter_nozzles.size() == 1) {
                cb->Enable(false);
                cb->SetForegroundColour(text_primary(dark));  // keep text readable
            }
            cb->Bind(wxEVT_CHECKBOX, [this, nozzle = kv.first, all_nozzles](auto &evt) {
                if (m_filter_nozzles.empty()) {
                    m_filter_nozzles = all_nozzles;
                }
                if (evt.IsChecked())
                    m_filter_nozzles.insert(nozzle);
                else
                    m_filter_nozzles.erase(nozzle);
                if (m_filter_nozzles == all_nozzles)
                    m_filter_nozzles.clear();
                m_filter_bases.clear();
                on_filter_changed();
            });
            sizer->Add(cb, 0, wxLEFT, FromDIP(12));
        }

        // Base profile filter — dynamic based on nozzle selection
        std::map<std::string, int> dynamic_base_counts;
        for (auto &d : m_all_data[m_collection]) {
            bool nozzle_ok = m_filter_nozzles.empty() || m_filter_nozzles.count(d.nozzle) > 0;
            if (nozzle_ok)
                dynamic_base_counts[d.inherits]++;
        }

        if (!dynamic_base_counts.empty()) {
            add_section(_L("Base Profile"));
            std::set<std::string> all_bases;
            for (auto &kv : dynamic_base_counts) all_bases.insert(kv.first);

            for (auto &kv : dynamic_base_counts) {
                wxString text = from_u8(kv.first) + wxString::Format(" (%d)", kv.second);
                auto *cb = new wxCheckBox(parent, wxID_ANY, text);
                bool is_active = m_filter_bases.empty() || m_filter_bases.count(kv.first) > 0;
                cb->SetValue(is_active);
                cb->SetForegroundColour(text_primary(dark));
                if (is_active && m_filter_bases.size() == 1)
                    cb->Enable(false);
                // Also disable if there's only one base profile available
                if (dynamic_base_counts.size() == 1) {
                    cb->Enable(false);
                }
                cb->SetForegroundColour(text_primary(dark));  // keep text readable even when disabled
                cb->Bind(wxEVT_CHECKBOX, [this, base = kv.first, all_bases](auto &evt) {
                    if (m_filter_bases.empty()) {
                        m_filter_bases = all_bases;
                    }
                    if (evt.IsChecked())
                        m_filter_bases.insert(base);
                    else
                        m_filter_bases.erase(base);
                    if (m_filter_bases == all_bases)
                        m_filter_bases.clear();
                    on_filter_changed();
                });
                sizer->Add(cb, 0, wxLEFT, FromDIP(12));
            }
        }

        // Post-process filter
        auto *pp_cb = new wxCheckBox(parent, wxID_ANY, _L("Has post-processing"));
        pp_cb->SetValue(m_filter_postprocess_only);
        pp_cb->SetForegroundColour(text_primary(dark));
        pp_cb->Bind(wxEVT_CHECKBOX, [this](auto &evt) {
            m_filter_postprocess_only = evt.IsChecked();
            on_filter_changed();
        });
        sizer->Add(pp_cb, 0, wxLEFT | wxTOP, FromDIP(8));
    }
    else if (m_collection == 1) {  // Filament
        if (m_material_counts.size() > 1) {
            add_section(_L("Material"));
            std::set<std::string> all_mats;
            for (auto &kv : m_material_counts) all_mats.insert(kv.first);

            for (auto &kv : m_material_counts) {
                wxString text = from_u8(kv.first) + wxString::Format(" (%d)", kv.second);
                auto *cb = new wxCheckBox(parent, wxID_ANY, text);
                bool is_active = m_filter_materials.empty() || m_filter_materials.count(kv.first) > 0;
                cb->SetValue(is_active);
                cb->SetForegroundColour(text_primary(dark));
                cb->Bind(wxEVT_CHECKBOX, [this, mat = kv.first, all_mats](auto &evt) {
                    if (m_filter_materials.empty()) {
                        m_filter_materials = all_mats;
                    }
                    if (evt.IsChecked())
                        m_filter_materials.insert(mat);
                    else
                        m_filter_materials.erase(mat);
                    if (m_filter_materials == all_mats)
                        m_filter_materials.clear();
                    on_filter_changed();
                });
                sizer->Add(cb, 0, wxLEFT, FromDIP(12));
            }
        }
    }

    parent->Layout();
}

// ============================================================
// Filtering, sorting, rebuilding
// ============================================================

void PresetExplorerDialog::rebuild_visible_list()
{
    m_list_sizer->Clear(true);
    m_card_panels.clear();
    m_detail_panels.clear();
    m_card_checks.clear();

    auto &data = m_all_data[m_collection];
    for (size_t idx : m_visible_indices) {
        auto &d = data[idx];
        auto *card = create_preset_card(m_list_panel, d);
        m_card_panels[d.name] = card;
        m_list_sizer->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(2));

        // Restore expanded state
        if (m_expanded_preset == d.name) {
            auto *details = create_expanded_details(m_list_panel, d);
            m_detail_panels[d.name] = details;
            m_list_sizer->Add(details, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(2));
        }

        // Restore checked state
        if (m_checked_presets.count(d.name)) {
            auto it = m_card_checks.find(d.name);
            if (it != m_card_checks.end()) it->second->SetValue(true);
        }
    }

    m_status_text->SetLabel(wxString::Format(_L("%zu presets"), m_visible_indices.size()));
    m_list_panel->Show(!m_visible_indices.empty());
    m_empty_panel->Show(m_visible_indices.empty());
    m_list_panel->Layout();
    m_list_panel->FitInside();
    m_list_panel->GetParent()->Layout();
}

void PresetExplorerDialog::apply_filters()
{
    m_visible_indices.clear();
    auto &data = m_all_data[m_collection];

    for (size_t i = 0; i < data.size(); i++) {
        auto &d = data[i];

        if (m_filter_compatible_only && !d.is_compatible) continue;

        if (m_collection == 2) {
            if (!m_filter_nozzles.empty() && m_filter_nozzles.count(d.nozzle) == 0) continue;
            if (!m_filter_bases.empty() && m_filter_bases.count(d.inherits) == 0) continue;
            if (m_filter_postprocess_only && !d.has_postprocess) continue;
        }
        if (m_collection == 1) {
            if (!m_filter_materials.empty() && m_filter_materials.count(d.material_type) == 0) continue;
        }

        // Search filter — name only
        if (!m_search_text.empty()) {
            std::string lower_search = m_search_text;
            std::transform(lower_search.begin(), lower_search.end(), lower_search.begin(), ::tolower);
            std::string lower_name = d.name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

            if (lower_name.find(lower_search) == std::string::npos)
                continue;
        }

        m_visible_indices.push_back(i);
    }

    // Sort
    auto &all = m_all_data[m_collection];
    auto cmp = [&](size_t a, size_t b) -> bool {
        bool result;
        if (m_sort_by == "name")
            result = all[a].name < all[b].name;
        else if (m_sort_by == "base")
            result = all[a].inherits < all[b].inherits;
        else if (m_sort_by == "layer_height")
            result = all[a].layer_height < all[b].layer_height;
        else if (m_sort_by == "walls")
            result = all[a].walls < all[b].walls;
        else if (m_sort_by == "infill")
            result = all[a].infill < all[b].infill;
        else if (m_sort_by == "nozzle")
            result = all[a].nozzle < all[b].nozzle;
        else if (m_sort_by == "material")
            result = all[a].material_type < all[b].material_type;
        else if (m_sort_by == "nozzle_temp")
            result = all[a].nozzle_temp < all[b].nozzle_temp;
        else if (m_sort_by == "compatible")
            result = all[a].is_compatible > all[b].is_compatible;
        else
            result = all[a].name < all[b].name;
        return m_sort_ascending ? result : !result;
    };
    std::sort(m_visible_indices.begin(), m_visible_indices.end(), cmp);
}

void PresetExplorerDialog::on_search(const wxString &keyword)
{
    m_search_text = into_u8(keyword);
    apply_filters();
    rebuild_visible_list();
}

void PresetExplorerDialog::on_sort_changed(int selection)
{
    const char *sorts[] = {"name", "base", "layer_height", "compatible"};
    m_sort_by = sorts[selection];
    apply_filters();
    rebuild_visible_list();
}

void PresetExplorerDialog::on_tab_changed(int tab)
{
    m_collection = tab;
    m_checked_presets.clear();
    m_expanded_preset.clear();
    m_filter_nozzles.clear();
    m_filter_bases.clear();
    m_filter_materials.clear();
    m_filter_compatible_only = false;
    m_filter_postprocess_only = false;
    m_search_text.clear();
    if (m_search) m_search->GetTextCtrl()->ChangeValue("");

    m_tab_ctrl->SetItemBold(0, tab == 0);
    m_tab_ctrl->SetItemBold(1, tab == 1);
    m_tab_ctrl->SetItemBold(2, tab == 2);

    build_filter_panel(m_filter_panel, m_filter_sizer);
    apply_filters();
    rebuild_visible_list();
    Layout();
}

void PresetExplorerDialog::on_filter_changed()
{
    // Clear selection when filters change
    m_checked_presets.clear();
    if (m_select_all) m_select_all->SetValue(false);
    if (m_btn_delete) m_btn_delete->Enable(false);
    if (m_btn_compare) m_btn_compare->Enable(false);

    // Rebuild filter sidebar (base profiles depend on nozzle selection)
    build_filter_panel(m_filter_panel, m_filter_sizer);
    apply_filters();
    rebuild_visible_list();
}

// ============================================================
// Actions
// ============================================================

void PresetExplorerDialog::on_preset_expand(const std::string &name)
{
    // Toggle expand
    if (m_expanded_preset == name) {
        // Collapse
        auto it = m_detail_panels.find(name);
        if (it != m_detail_panels.end()) {
            it->second->Destroy();
            m_detail_panels.erase(it);
        }
        m_expanded_preset.clear();
    } else {
        // Collapse previous
        if (!m_expanded_preset.empty()) {
            auto it = m_detail_panels.find(m_expanded_preset);
            if (it != m_detail_panels.end()) {
                it->second->Destroy();
                m_detail_panels.erase(it);
            }
        }
        // Expand new
        auto &data = m_all_data[m_collection];
        for (auto idx : m_visible_indices) {
            if (data[idx].name == name) {
                auto *card = m_card_panels[name];
                auto *details = create_expanded_details(m_list_panel, data[idx]);
                m_detail_panels[name] = details;
                // Insert after the card in the sizer
                size_t pos = 0;
                for (size_t i = 0; i < m_list_sizer->GetItemCount(); i++) {
                    if (m_list_sizer->GetItem(i)->GetWindow() == card) {
                        pos = i + 1;
                        break;
                    }
                }
                m_list_sizer->Insert(pos, details, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(2));
                break;
            }
        }
        m_expanded_preset = name;
    }
    m_list_panel->Layout();
    m_list_panel->FitInside();
}

void PresetExplorerDialog::on_preset_checked(const std::string &name, bool checked)
{
    if (checked)
        m_checked_presets.insert(name);
    else
        m_checked_presets.erase(name);

    m_btn_delete->Enable(!m_checked_presets.empty());
    m_btn_compare->Enable(m_checked_presets.size() == 2);

    // Sync select all checkbox
    if (m_select_all) {
        if (m_checked_presets.size() == m_visible_indices.size())
            m_select_all->SetValue(true);
        else
            m_select_all->SetValue(false);
    }

    m_status_text->SetLabel(m_checked_presets.empty()
        ? wxString::Format(_L("%zu presets"), m_visible_indices.size())
        : wxString::Format(_L("%zu selected"), m_checked_presets.size()));
}

void PresetExplorerDialog::on_delete_checked()
{
    if (m_checked_presets.empty()) return;

    // Confirm
    wxString msg = wxString::Format(_L("Delete %zu selected preset(s)?"), m_checked_presets.size());
    wxMessageDialog dlg(this, msg, _L("Confirm Delete"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES) return;

    Preset::Type types[] = {Preset::TYPE_PRINTER, Preset::TYPE_FILAMENT, Preset::TYPE_PRINT};
    Preset::Type type = types[m_collection];
    Tab *tab = wxGetApp().get_tab(type);
    if (!tab) return;

    auto *collection = tab->get_presets();
    std::string current_name = collection->get_edited_preset().name;
    bool deleted_current = false;

    for (auto &name : m_checked_presets) {
        if (name == current_name) {
            deleted_current = true;
            continue;  // delete current last
        }
        auto *preset = collection->find_preset(name, false);
        if (preset && !preset->is_default && !preset->is_system) {
            if (!preset->setting_id.empty()) {
                collection->set_sync_info_and_save(name, preset->setting_id, "delete", 0);
                wxGetApp().delete_preset_from_cloud(preset->setting_id);
            }
            collection->delete_preset(name);
        }
    }

    if (deleted_current)
        tab->select_preset("", true);
    else
        wxGetApp().plater()->sidebar().update_presets(type);

    // Refresh data and rebuild
    m_checked_presets.clear();
    if (m_select_all) m_select_all->SetValue(false);
    m_btn_delete->Enable(false);
    m_btn_compare->Enable(false);
    m_all_data.clear();
    m_nozzle_counts.clear();
    m_base_counts.clear();
    m_material_counts.clear();
    init_preset_data();
    build_filter_panel(m_filter_panel, m_filter_sizer);
    apply_filters();
    rebuild_visible_list();
}

void PresetExplorerDialog::on_compare_checked()
{
    if (m_checked_presets.size() < 2) return;
    auto it = m_checked_presets.begin();
    std::string left = *it++;
    std::string right = *it;

    // Check if either preset is incompatible
    bool needs_show_all = false;
    for (auto &d : m_all_data[m_collection]) {
        if ((d.name == left || d.name == right) && !d.is_compatible)
            needs_show_all = true;
    }

    Preset::Type types[] = {Preset::TYPE_PRINTER, Preset::TYPE_FILAMENT, Preset::TYPE_PRINT};
    DiffPresetDialog dlg(this, types[m_collection], left, right, needs_show_all);
    dlg.ShowModal();
}

void PresetExplorerDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    SetMinSize({FromDIP(900), FromDIP(600)});
    Layout();
    Fit();
}

}}
