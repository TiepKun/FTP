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
    void on_btn_download_clicked();
    void on_btn_pause_upload_clicked();
    void on_btn_resume_upload_clicked();
    void on_btn_pause_download_clicked();
    void on_btn_resume_download_clicked();
    void on_btn_unzip_clicked();
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
    Gtk::Button btn_download_;
    Gtk::Button btn_pause_up_;
    Gtk::Button btn_resume_up_;
    Gtk::Button btn_pause_down_;
    Gtk::Button btn_resume_down_;
    Gtk::Button btn_unzip_;
    Gtk::Entry entry_unzip_target_;

    Gtk::ScrolledWindow scroll_;
    Gtk::TextView text_view_;
    Gtk::Label lbl_status_;
    Gtk::Label lbl_online_;

    sigc::connection online_timer_;
    // ===== FILE LIST MODEL =====
    class FileModelColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        FileModelColumns() { add(name); add(size); add(full_path); add(is_folder); }
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> size;
        Gtk::TreeModelColumn<Glib::ustring> full_path;
        Gtk::TreeModelColumn<bool> is_folder;
    };

    FileModelColumns columns_;
    Glib::RefPtr<Gtk::TreeStore> file_list_store_;
    Gtk::TreeView file_list_view_;

    // Helpers
    void add_path_to_tree(const std::string &path, const std::string &size_str, bool is_folder);
    std::vector<std::string> split_path(const std::string &path);
    void upload_folder_recursive(const std::string &local_root,
                                 const std::string &remote_root,
                                 std::error_code &ec_out,
                                 std::string &err_out,
                                 bool &ok_out);
};
