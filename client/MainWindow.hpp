// ===== file: client/MainWindow.hpp =====
#pragma once
#include <gtkmm.h>
#include <string>
#include "NetworkClient.hpp"

using namespace std;

class MainWindow : public Gtk::Window {
public:
    MainWindow(NetworkClient &&client, const string &username);
    

protected:
    void on_btn_load_clicked();
    void on_btn_save_clicked();
    void on_btn_upload_clicked();
    void refresh_file_list();
    void on_file_selected();
    bool update_online_count();

    NetworkClient client_;
    string username_;

    Gtk::Box vbox_;
    Gtk::Entry entry_path_;
    Gtk::Button btn_load_;
    Gtk::Button btn_save_;
    Gtk::Button btn_upload_;

    Gtk::ScrolledWindow scroll_;
    Gtk::TextView text_view_;
    Gtk::Label lbl_status_;
    Gtk::Label lbl_online_;

    sigc::connection online_timer_;
    // ===== FILE LIST MODEL =====
    class FileModelColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        FileModelColumns() { add(name); add(size); }
        Gtk::TreeModelColumn<Glib::ustring> name;
         Gtk::TreeModelColumn<Glib::ustring> size;
    };

    FileModelColumns columns_;
    Glib::RefPtr<Gtk::ListStore> file_list_store_;
    Gtk::TreeView file_list_view_;
};
