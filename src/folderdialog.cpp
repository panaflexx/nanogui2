/*
    src/folderdialog.cpp -- Simple "OK" or "Yes/No"-style modal dialogs

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/folderdialog.h>
#include <nanogui/layout.h>
#include <nanogui/button.h>
#include <nanogui/label.h>
#include <nanogui/screen.h>
#include <nanogui/scrollpanel.h>

#if defined(_WIN32)
#include <filesystem>
#include <windows.h>


NAMESPACE_BEGIN(nanogui)

FolderDialog::FolderDialog(Widget* parent, const std::string& title,
    const std::string& starting_folder) : Window(parent, title, true) {

    set_modal(true);

    if (std::filesystem::exists(starting_folder))
        m_current_folder = starting_folder;
    else m_current_folder = std::filesystem::current_path().string();

    // normalize trailing separator
    if (!m_current_folder.empty()) {
        char sep = std::filesystem::path::preferred_separator;
        if (m_current_folder.back() == sep)
            m_current_folder.pop_back();
    }

    //Widget *top_panel = new Widget(this);
    //top_panel->set_layout(new BoxLayout(Orientation::Vertical,
    //                                 Alignment::Middle, 10, 15));
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Middle, 15));
    ScrollPanel* scroll = new ScrollPanel(this);
    scroll->set_scroll_type(ScrollPanel::ScrollTypes::Both);

    Widget* Container = new Widget(scroll);
    Container->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Middle));

    current_location = new Label(Container, m_current_folder);
    current_location->set_color(Color(0, 204, 0, 255));
    current_location->set_font_size(25);

    explore_treeview = new TreeView(Container);
    explore_treeview->set_fixed_size(Vector2i(300, 400));

    Widget* panel_buttons = new Widget(Container);
    panel_buttons->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 15));

    Button* ok_button = new Button(panel_buttons, "OK");
    ok_button->set_callback([&] { if (m_callback) m_callback(current_location->caption()); dispose(); });
    Button* cancel_button = new Button(panel_buttons, "Cancel");
    cancel_button->set_callback([&] { if (m_callback) m_callback(""); dispose(); });

#if !defined(_WIN32)
    Button* home_button = new Button(panel_buttons, "Home");
    home_button->set_callback([this] {
        const char* home = getenv("HOME");
        std::string homePath = home ? home : "/";
        if (std::filesystem::exists(homePath)) {
            m_current_folder = homePath;
            current_location->set_caption(m_current_folder);
            // rebuild tree rooted at home
            start_items();
        }
    });
#endif

    start_items();

    center();
    request_focus();
}

void FolderDialog::start_items()
{
    std::filesystem::path Curr_Root(m_current_folder);
    NanoTree* CurNanoTree = new NanoTree();
    explore_treeview->set_expand_callback([=](std::string Node)
        {
            for (auto const& CurrChild : explore_treeview->items()->Objects[Node]->Children)
            {
                try
                {
                    FillChildren(CurrChild.first);
                }
                catch (const std::exception& e) {};
            }
        });

    //Put Top Root
#if defined(_WIN32)
    CurNanoTree->set_root("This_PC");
    CurNanoTree->Objects["This_PC"]->Name = "This_PC";
    CurNanoTree->Objects["This_PC"]->Expanded = true;
    explore_treeview->set_items(CurNanoTree);
    // enumerate drives on Windows
    DWORD Drives = GetLogicalDrives();
    for (int Cnt = 0; Cnt < 32; Cnt++)
    {
        if ((Drives >> Cnt) & 1)
        {
            std::string Drive = std::string(1, (char)('A' + Cnt)) + ":\\";
            CurNanoTree->add_node("This_PC", Drive);
            CurNanoTree->Objects[Drive]->Name = Drive;
            CurNanoTree->Objects[Drive]->CallBack = [=] {current_location->set_caption(Drive); explore_treeview->perform_layout(screen()->nvg_context()); };
            FillChildren(Drive);
        }
    }
#else
    // On Unix, start directly from root "/"
    CurNanoTree->set_root("/");
    CurNanoTree->Objects["/"]->Name = "/";
    CurNanoTree->Objects["/"]->Expanded = true;
    explore_treeview->set_items(CurNanoTree);
    FillChildren("/");
#endif

    explore_treeview->set_items(explore_treeview->items());

    // step into the path (portable)
    std::filesystem::path p = Curr_Root;
    std::string accumulated;
    for (const auto& part : p) {
        if (accumulated.empty())
            accumulated = part.string();
        else
            accumulated = (std::filesystem::path(accumulated) / part).string();
        // ensure trailing separator for node names used by the tree
        if (!accumulated.empty() && accumulated.back() != std::filesystem::path::preferred_separator)
            accumulated += std::filesystem::path::preferred_separator;
        explore_treeview->arrow_callback(accumulated);
    }

    explore_treeview->perform_layout(screen()->nvg_context());
}

void FolderDialog::FillChildren(std::string Node)
{
#if defined(_WIN32)
    if (Node == "This_PC") return;
#endif
    if (explore_treeview->items()->Objects[Node]->Children.size() > 0) return;
    char sep = std::filesystem::path::preferred_separator;
    for (auto& CurrObject : std::filesystem::directory_iterator(Node))
    {
        try
        {
            if (std::filesystem::is_directory(CurrObject))
            {
                std::string CompleteName = CurrObject.path().string();
                if (!CompleteName.empty() && CompleteName.back() != sep)
                    CompleteName += sep;
                if (explore_treeview->items()->Objects.find(CompleteName) == explore_treeview->items()->Objects.end())// node does not already exist
                {
                    explore_treeview->items()->add_node(Node, CompleteName);
                    explore_treeview->items()->Objects[CompleteName]->Name = CurrObject.path().filename().string();
                    explore_treeview->items()->Objects[CompleteName]->CallBack = [=] {current_location->set_caption(CompleteName); this->perform_layout(screen()->nvg_context()); };
                }
            }
        }
        catch (const std::exception& e) {};
    }
}
NAMESPACE_END(nanogui)

#endif //WIN32
