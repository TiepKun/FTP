// ===== file: client/MainWindow.hpp =====
#pragma once
#include <gtkmm.h>
#include <string>
#include "NetworkClient.hpp"
#include <atomic>

using namespace std;

class MainWindow : public Gtk::Window {
public:
    MainWindow(NetworkClient &&client, const string &username);
    

protected:
    void on_btn_load_clicked();
    void on_btn_save_clicked();
    void on_btn_upload_clicked();
    void on_btn_download_clicked();
    void on_btn_refresh_clicked();
    void on_btn_pause_upload_clicked();
    void on_btn_resume_upload_clicked();
    void on_btn_pause_download_clicked();
    void on_btn_resume_download_clicked();
    void on_btn_unzip_clicked();
    void on_btn_create_folder_clicked();
    void on_btn_rename_clicked();
    void on_btn_move_clicked();
    void on_btn_delete_clicked();
    void on_btn_restore_clicked();
    void on_btn_list_deleted_clicked();
    void on_btn_share_clicked();
    void on_btn_change_role_clicked();
    void on_btn_logout_clicked();
    std::vector<std::string> collect_folder_paths();
    bool choose_folder_dialog(std::string &out_path);
    void refresh_file_list();
    void on_file_selected();
    

    NetworkClient client_;
    string username_;

    Gtk::Box vbox_;
    Gtk::Entry entry_path_;
    Gtk::Button btn_load_;
    Gtk::Button btn_save_;
    Gtk::Button btn_upload_;
    Gtk::Button btn_refresh_;
    Gtk::Button btn_download_;
    Gtk::Button btn_pause_up_;
    Gtk::Button btn_resume_up_;
    Gtk::Button btn_pause_down_;
    Gtk::Button btn_resume_down_;
    Gtk::Button btn_unzip_;
    Gtk::Entry entry_unzip_target_;

    Gtk::ScrolledWindow scroll_;
    Gtk::TextView text_view_;
    Gtk::Box *actions_box_ = nullptr;
    Gtk::Label lbl_status_;
    Gtk::Entry entry_target_;

    // ===== FILE LIST MODEL =====
    class FileModelColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        FileModelColumns() { add(name); add(size); add(full_path); add(is_folder); add(owner); add(is_shared); add(can_download); add(can_edit); }
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> size;
        Gtk::TreeModelColumn<Glib::ustring> full_path;
        Gtk::TreeModelColumn<bool> is_folder;
        Gtk::TreeModelColumn<Glib::ustring> owner;
        Gtk::TreeModelColumn<bool> is_shared;
        Gtk::TreeModelColumn<bool> can_download;
        Gtk::TreeModelColumn<bool> can_edit;
    };

    FileModelColumns columns_;
    Glib::RefPtr<Gtk::TreeStore> file_list_store_owned_;
    Glib::RefPtr<Gtk::TreeStore> file_list_store_shared_;
    Gtk::TreeView file_list_view_owned_;
    Gtk::TreeView file_list_view_shared_;

    // Helpers
    void add_path_to_tree(const std::string &path,
                          const std::string &size_str,
                          bool is_folder,
                          const std::string &owner,
                          bool is_shared,
                          bool can_download,
                          bool can_edit,
                          Glib::RefPtr<Gtk::TreeStore> store);
    Gtk::TreeModel::iterator find_iter_by_path(const std::string &path, const Gtk::TreeModel::Children &children);
    void expand_and_select(const std::string &path);
    std::vector<std::string> split_path(const std::string &path);
    void upload_folder_recursive(const std::string &local_root,
                                 const std::string &remote_root,
                                 std::error_code &ec_out,
                                 std::string &err_out,
                                 bool &ok_out);
    struct EntryMeta { std::string path; bool is_folder; bool is_shared; bool can_download; bool can_edit; };
    std::vector<EntryMeta> latest_entries_owned_;
    struct RoleInfo { bool view; bool download; bool edit; };
    std::map<std::string, RoleInfo> shared_roles_prev_;

    // ===== Upload progress =====
    Gtk::ProgressBar progress_upload_;

    std::atomic<uint64_t> upload_sent_{0};
    uint64_t upload_total_{0};
    std::atomic<bool> uploading_{false};
    std::atomic<bool> pause_upload_requested_{false};

    sigc::connection upload_timer_;

    bool on_upload_progress_tick();

    // ===== Download progress =====
    Gtk::ProgressBar progress_download_;
    std::atomic<uint64_t> download_received_{0};
    uint64_t download_total_{0};
    std::atomic<bool> downloading_{false};
    std::atomic<bool> pause_download_requested_{false};
    sigc::connection download_timer_;

    // Logout button
    Gtk::Button btn_logout_{"Logout"};

    bool on_download_progress_tick();
    void on_owned_selection_changed();
    void on_shared_selection_changed();
    void update_action_sensitivity(bool is_shared, bool can_download, bool can_edit);
    void reset_selection_state();
    bool current_is_shared_ = false;
    bool current_can_download_ = true;
    bool current_can_edit_ = true;
};
