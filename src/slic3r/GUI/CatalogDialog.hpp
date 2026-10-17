///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_CatalogDialog_hpp_
#define slic3r_CatalogDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/Catalog.hpp"

class wxListBox;
class wxChoice;
class wxComboBox;
class wxTextCtrl;
class wxCheckBox;

namespace Slic3r {
namespace GUI {

class CatalogDialog : public DPIDialog
{
public:
    CatalogDialog(wxWindow *parent, CatalogDocument doc);
    ~CatalogDialog() override;
    void replace_document(CatalogDocument doc);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    bool commit_form();
    void load_form(int index);
    void rebuild_list(int select_index);
    void refresh_row(int index);
    bool entry_visible(const CatalogEntry &entry) const;
    wxString entry_label(const CatalogEntry &entry) const;
    void fill_inherits(const CatalogEntry &entry);
    void upsert(CatalogEntry entry);
    bool save_document();

    CatalogDocument m_doc;
    std::vector<int> m_visible;
    int  m_form_index{ -1 };
    int  m_filter_selection{ 0 };
    bool m_lock{ false };
    bool m_dirty{ false };

    wxChoice   *m_filter{ nullptr };
    wxListBox  *m_list{ nullptr };
    wxTextCtrl *m_name{ nullptr };
    wxChoice   *m_kind{ nullptr };
    wxChoice   *m_technology{ nullptr };
    wxComboBox *m_inherits{ nullptr };
    wxTextCtrl *m_brand{ nullptr };
    wxTextCtrl *m_optimized{ nullptr };
    wxCheckBox *m_keep{ nullptr };
    wxTextCtrl *m_settings{ nullptr };
};

void open_printer_material_catalog(wxWindow *parent);
void import_printer_or_material(wxWindow *parent);
void export_printer_or_material(wxWindow *parent);

} // namespace GUI
} // namespace Slic3r

#endif
