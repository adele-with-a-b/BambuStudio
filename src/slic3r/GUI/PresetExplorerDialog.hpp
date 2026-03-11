#pragma once

#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/TextInput.hpp"

#include <vector>
#include <map>
#include <set>
#include <functional>

class TabCtrl;

namespace Slic3r {

class Preset;
class PresetCollection;

namespace GUI {

// Data extracted from a preset for display/filtering
struct PresetCardData {
    std::string name;
    std::string inherits;
    std::string nozzle;         // "0.2", "0.4", "0.6", "0.8"
    bool        is_compatible = true;
    bool        has_postprocess = false;
    std::string material_type;  // filament only: "PLA", "ASA", etc.

    // Key settings for expanded view (process)
    std::string layer_height;
    std::string walls;
    std::string infill;
    std::string speed;
    std::string postprocess_script;

    // Key settings for expanded view (filament)
    std::string nozzle_temp;
    std::string bed_temp;
    std::string density;
    std::string cost;
};

class PresetExplorerDialog : public DPIDialog
{
public:
    PresetExplorerDialog(wxWindow *parent);

private:
    // Data
    void init_preset_data();
    std::string parse_nozzle(const std::string &inherits) const;
    std::string parse_material_type(const Preset &preset) const;
    PresetCardData extract_card_data(const Preset &preset, PresetCollection *collection);

    // UI building
    void build_filter_panel(wxWindow *parent, wxSizer *sizer);
    void build_preset_list(wxWindow *parent, wxSizer *sizer);
    wxPanel *create_preset_card(wxWindow *parent, const PresetCardData &data);
    wxPanel *create_expanded_details(wxWindow *parent, const PresetCardData &data);

    // Filtering & sorting
    void apply_filters();
    void on_search(const wxString &keyword);
    void on_sort_changed(int selection);
    void on_tab_changed(int tab);
    void on_filter_changed();
    void rebuild_visible_list();

    // Actions
    void on_delete_checked();
    void on_compare_checked();
    void on_preset_expand(const std::string &name);
    void on_preset_checked(const std::string &name, bool checked);

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    // Tab: 0=Printer, 1=Filament, 2=Process
    int m_collection = 2;  // default to Process

    // All preset data per collection
    std::vector<std::vector<PresetCardData>> m_all_data;  // [collection][index]

    // Current visible (filtered/sorted) indices into m_all_data[m_collection]
    std::vector<size_t> m_visible_indices;

    // UI state
    std::string m_search_text;
    std::string m_sort_by = "name";  // "name", "modified", "layer_height", "base", "material"
    bool m_sort_ascending = true;
    std::set<std::string> m_checked_presets;
    std::string m_expanded_preset;  // only one expanded at a time

    // Filter state
    std::set<std::string> m_filter_nozzles;      // empty = all
    std::set<std::string> m_filter_bases;         // empty = all
    std::set<std::string> m_filter_materials;     // empty = all
    bool m_filter_compatible_only = false;
    bool m_filter_postprocess_only = false;

    // Filter counts (for display)
    std::map<std::string, int> m_nozzle_counts;
    std::map<std::string, int> m_base_counts;
    std::map<std::string, int> m_material_counts;

    // Widgets
    TabCtrl *m_tab_ctrl = nullptr;
    TextInput *m_search = nullptr;
    wxChoice *m_sort_choice = nullptr;
    wxScrolledWindow *m_list_panel = nullptr;
    wxPanel *m_empty_panel = nullptr;
    wxPanel *m_filter_panel = nullptr;
    wxBoxSizer *m_filter_sizer = nullptr;
    wxBoxSizer *m_list_sizer = nullptr;
    Button *m_btn_delete = nullptr;
    Button *m_btn_compare = nullptr;
    ::CheckBox *m_select_all = nullptr;
    wxStaticText *m_status_text = nullptr;

    // Card widgets keyed by preset name
    std::map<std::string, wxPanel *> m_card_panels;
    std::map<std::string, wxPanel *> m_detail_panels;
    std::map<std::string, ::CheckBox *> m_card_checks;
};

}}
