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
    m_sort_choice->Append(_L("Name"));
    m_sort_choice->Append(_L("Base Profile"));
    m_sort_choice->Append(_L("Layer Height"));
    m_sort_choice->Append(_L("Compatibility"));
    m_sort_choice->SetSelection(0);
    m_sort_choice->Bind(wxEVT_CHOICE, [this](auto &evt) { on_sort_changed(evt.GetSelection()); });

    wxBoxSizer *top_bar = new wxBoxSizer(wxHORIZONTAL);
    top_bar->Add(m_search, 1, wxALIGN_CENTER | wxRIGHT, FromDIP(10));
    top_bar->Add(new wxStaticText(this, wxID_ANY, _L("Sort:")), 0, wxALIGN_CENTER | wxRIGHT, FromDIP(5));
    top_bar->Add(m_sort_choice, 0, wxALIGN_CENTER);

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

    // Content area: filter | list
    wxBoxSizer *content = new wxBoxSizer(wxHORIZONTAL);
    content->Add(m_filter_panel, 0, wxEXPAND | wxRIGHT, FromDIP(10));
    content->Add(m_list_panel, 1, wxEXPAND);

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
        m_btn_compare->Enable(m_checked_presets.size() >= 2);
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
        m_btn_compare->Refresh();
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

    // Expand button
    auto *expand_btn = new wxStaticText(card, wxID_ANY, "+");
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
    hsizer->Add(expand_btn, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(8));
    hsizer->Add(name_label, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(8));
    hsizer->Add(badge, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(8));
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

    auto *grid = new wxFlexGridSizer(2, FromDIP(5), FromDIP(20));
    grid->AddGrowableCol(1, 1);

    auto add_row = [&](const std::string &label, const std::string &value) {
        if (value.empty()) return;
        auto *lbl = new wxStaticText(panel, wxID_ANY, from_u8(label));
        lbl->SetForegroundColour(text_secondary(dark));
        lbl->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        auto *val = new wxStaticText(panel, wxID_ANY, from_u8(value));
        val->SetForegroundColour(text_primary(dark));
        val->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        grid->Add(lbl, 0, wxALIGN_RIGHT);
        grid->Add(val, 1, wxEXPAND);
    };

    if (m_collection == 2) {
        add_row("Layer Height", data.layer_height);
        add_row("Walls", data.walls);
        add_row("Infill", data.infill);
        add_row("Nozzle", data.nozzle + "mm");
        add_row("Inherits", data.inherits);
        if (data.has_postprocess)
            add_row("Post-process", data.postprocess_script);
    } else if (m_collection == 1) {
        add_row("Material", data.material_type);
        add_row("Nozzle Temp", data.nozzle_temp);
        add_row("Bed Temp", data.bed_temp);
        add_row("Density", data.density);
        add_row("Cost", data.cost);
        add_row("Inherits", data.inherits);
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
            if (is_active && m_filter_nozzles.size() == 1)
                cb->Enable(false);
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
    }

    m_status_text->SetLabel(wxString::Format(_L("%zu presets"), m_visible_indices.size()));
    m_list_panel->Layout();
    m_list_panel->FitInside();
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

        // Search filter
        if (!m_search_text.empty()) {
            std::string lower_search = m_search_text;
            std::transform(lower_search.begin(), lower_search.end(), lower_search.begin(), ::tolower);
            std::string lower_name = d.name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            std::string lower_inherits = d.inherits;
            std::transform(lower_inherits.begin(), lower_inherits.end(), lower_inherits.begin(), ::tolower);
            std::string lower_script = d.postprocess_script;
            std::transform(lower_script.begin(), lower_script.end(), lower_script.begin(), ::tolower);

            if (lower_name.find(lower_search) == std::string::npos &&
                lower_inherits.find(lower_search) == std::string::npos &&
                lower_script.find(lower_search) == std::string::npos)
                continue;
        }

        m_visible_indices.push_back(i);
    }

    // Sort
    auto &all = m_all_data[m_collection];
    if (m_sort_by == "name") {
        std::sort(m_visible_indices.begin(), m_visible_indices.end(),
            [&](size_t a, size_t b) { return all[a].name < all[b].name; });
    } else if (m_sort_by == "base") {
        std::sort(m_visible_indices.begin(), m_visible_indices.end(),
            [&](size_t a, size_t b) { return all[a].inherits < all[b].inherits; });
    } else if (m_sort_by == "layer_height") {
        std::sort(m_visible_indices.begin(), m_visible_indices.end(),
            [&](size_t a, size_t b) { return all[a].layer_height < all[b].layer_height; });
    } else if (m_sort_by == "compatible") {
        std::sort(m_visible_indices.begin(), m_visible_indices.end(),
            [&](size_t a, size_t b) {
                if (all[a].is_compatible != all[b].is_compatible) return all[a].is_compatible;
                return all[a].name < all[b].name;
            });
    }
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
