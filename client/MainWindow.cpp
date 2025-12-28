// ===== file: client/MainWindow.cpp =====
#include "MainWindow.hpp"
#include <filesystem>
#include <fstream>

using namespace std;

MainWindow::MainWindow(NetworkClient &&client, const string &username)
    : client_(std::move(client)),
      username_(username),
      vbox_(Gtk::ORIENTATION_VERTICAL),
      btn_load_("Load"),
      btn_save_("Save"),
      btn_upload_("Upload"),
      btn_download_("Download"),
      btn_pause_up_("Pause Up"),
      btn_resume_up_("Resume Up"),
      btn_pause_down_("Pause Down"),
      btn_resume_down_("Resume Down"),
      btn_unzip_("Unzip")
{
    set_title("File Share - " + username_);
    set_default_size(800, 500);

    add(vbox_);
    vbox_.set_spacing(5);
    vbox_.set_margin_top(5);
    vbox_.set_margin_bottom(5);
    vbox_.set_margin_left(5);
    vbox_.set_margin_right(5);

    // ==== Input & buttons ====
    entry_path_.set_placeholder_text("Relative path on server (e.g. notes.txt)");

    Gtk::Box *hbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    hbox->pack_start(entry_path_, Gtk::PACK_EXPAND_WIDGET);
    hbox->pack_start(btn_load_, Gtk::PACK_SHRINK);
    hbox->pack_start(btn_save_, Gtk::PACK_SHRINK);
    hbox->pack_start(btn_upload_, Gtk::PACK_SHRINK);
    hbox->pack_start(btn_download_, Gtk::PACK_SHRINK);
    hbox->pack_start(btn_unzip_, Gtk::PACK_SHRINK);
    vbox_.pack_start(*hbox, Gtk::PACK_SHRINK);

    Gtk::Box *hbox2 = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    entry_unzip_target_.set_placeholder_text("Unzip target folder (optional)");
    hbox2->pack_start(btn_pause_up_, Gtk::PACK_SHRINK);
    hbox2->pack_start(btn_resume_up_, Gtk::PACK_SHRINK);
    hbox2->pack_start(btn_pause_down_, Gtk::PACK_SHRINK);
    hbox2->pack_start(btn_resume_down_, Gtk::PACK_SHRINK);
    hbox2->pack_start(entry_unzip_target_, Gtk::PACK_EXPAND_WIDGET);
    vbox_.pack_start(*hbox2, Gtk::PACK_SHRINK);

    Gtk::Box *hbox3 = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    entry_target_.set_placeholder_text("Target path (for rename/move/copy)");
    hbox3->pack_start(entry_target_, Gtk::PACK_EXPAND_WIDGET);
    Gtk::Button *btn_create_folder = Gtk::manage(new Gtk::Button("Create Folder"));
    Gtk::Button *btn_rename = Gtk::manage(new Gtk::Button("Rename"));
    Gtk::Button *btn_move = Gtk::manage(new Gtk::Button("Move"));
    Gtk::Button *btn_copy = Gtk::manage(new Gtk::Button("Copy"));
    Gtk::Button *btn_delete = Gtk::manage(new Gtk::Button("Delete"));
    Gtk::Button *btn_restore = Gtk::manage(new Gtk::Button("Restore"));
    Gtk::Button *btn_list_deleted = Gtk::manage(new Gtk::Button("Show Deleted"));
    hbox3->pack_start(*btn_create_folder, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_rename, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_move, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_copy, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_delete, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_restore, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_list_deleted, Gtk::PACK_SHRINK);
    vbox_.pack_start(*hbox3, Gtk::PACK_SHRINK);
    vbox_.pack_start(lbl_status_, Gtk::PACK_SHRINK);

    lbl_online_.set_text("Online: ...");
    vbox_.pack_start(lbl_online_, Gtk::PACK_SHRINK);

    // Cập nhật mỗi 10 giây
    online_timer_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &MainWindow::update_online_count),
        1
    );



    // ==== File list + editor side-by-side ====
    file_list_store_ = Gtk::TreeStore::create(columns_);
    file_list_view_.set_model(file_list_store_);
    file_list_view_.append_column("File", columns_.name);
    file_list_view_.append_column("Size (KB)", columns_.size);


    file_list_view_.get_selection()->signal_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_file_selected));

    Gtk::ScrolledWindow *sw_left = Gtk::manage(new Gtk::ScrolledWindow());
    sw_left->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw_left->add(file_list_view_);

    Gtk::ScrolledWindow *sw_right = Gtk::manage(new Gtk::ScrolledWindow());
    sw_right->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw_right->add(text_view_);

    Gtk::Paned *paned = Gtk::manage(new Gtk::Paned(Gtk::ORIENTATION_HORIZONTAL));
    paned->add1(*sw_left);
    paned->add2(*sw_right);

    vbox_.pack_start(*paned, Gtk::PACK_EXPAND_WIDGET);

    // ==== Connect signals ====
    btn_load_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_load_clicked));
    btn_save_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_save_clicked));
    btn_upload_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_upload_clicked));
    btn_download_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_download_clicked));
    btn_pause_up_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_pause_upload_clicked));
    btn_resume_up_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_resume_upload_clicked));
    btn_pause_down_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_pause_download_clicked));
    btn_resume_down_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_resume_download_clicked));
    btn_unzip_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_unzip_clicked));
    btn_create_folder->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_create_folder_clicked));
    btn_rename->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_rename_clicked));
    btn_move->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_move_clicked));
    btn_copy->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_copy_clicked));
    btn_delete->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_delete_clicked));
    btn_restore->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_restore_clicked));
    btn_list_deleted->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_list_deleted_clicked));

    show_all_children();
    refresh_file_list();     // load file list automatically
}

void MainWindow::on_btn_load_clicked() {
    string path = entry_path_.get_text();
    string content, err;

    if (!client_.get_text(path, content, err)) {
        lbl_status_.set_text("Load failed: " + err);
        return;
    }

    text_view_.get_buffer()->set_text(content);
    lbl_status_.set_text("Loaded " + path);
}

void MainWindow::on_btn_save_clicked() {
    string path = entry_path_.get_text();
    string content = text_view_.get_buffer()->get_text();
    string err;

    if (!client_.put_text(path, content, err)) {
        lbl_status_.set_text("Save failed: " + err);
        return;
    }

    lbl_status_.set_text("Saved " + path);
    refresh_file_list();
}

void MainWindow::on_btn_upload_clicked() {
    // Single button: allow picking file or folder.
    Gtk::FileChooserDialog dialog("Select file or folder to upload", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Select _Folder", 1001);
    dialog.add_button("_Open", Gtk::RESPONSE_OK);

    int resp = dialog.run();
    if (resp == Gtk::RESPONSE_CANCEL) {
        lbl_status_.set_text("Upload canceled");
        return;
    }

    // If user chose folder button, open folder chooser
    if (resp == 1001) {
        Gtk::FileChooserDialog folder_dialog("Select folder to upload", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        folder_dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        folder_dialog.add_button("_Select", Gtk::RESPONSE_OK);
        int fresp = folder_dialog.run();
        if (fresp != Gtk::RESPONSE_OK) {
            lbl_status_.set_text("Upload canceled");
            return;
        }
        string folder_path = folder_dialog.get_filename();
        std::error_code ec;
        std::string err;
        bool ok = true;
        std::filesystem::path folder(folder_path);
        string base_name = folder.filename().string();
        upload_folder_recursive(folder_path, base_name, ec, err, ok);
        if (!ok) {
            lbl_status_.set_text("Upload folder failed: " + err);
            return;
        }
        lbl_status_.set_text("Folder uploaded: " + base_name);
        refresh_file_list();
        return;
    }

    // File path chosen
    string local_path = dialog.get_filename();
    std::filesystem::path p(local_path);
    if (std::filesystem::is_directory(p)) {
        std::error_code ec;
        std::string err;
        bool ok = true;
        string base_name = p.filename().string();
        upload_folder_recursive(local_path, base_name, ec, err, ok);
        if (!ok) {
            lbl_status_.set_text("Upload folder failed: " + err);
            return;
        }
        lbl_status_.set_text("Folder uploaded: " + base_name);
        refresh_file_list();
        return;
    }

    string remote_path = p.filename().string();

    string err;
    if (!client_.upload_file(local_path, remote_path, err)) {
        lbl_status_.set_text("Upload failed: " + err);
        return;
    }

    lbl_status_.set_text("Uploaded " + remote_path + " successfully");
    refresh_file_list();
}

void MainWindow::on_btn_download_clicked() {
    string remote_path = entry_path_.get_text();
    Gtk::FileChooserDialog dialog("Save download as", Gtk::FILE_CHOOSER_ACTION_SAVE);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Save", Gtk::RESPONSE_OK);
    dialog.set_current_name(remote_path);

    if (dialog.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Download canceled");
        return;
    }

    string local_path = dialog.get_filename();
    string err;
    if (!client_.download_file(remote_path, local_path, err)) {
        lbl_status_.set_text("Download failed: " + err);
        return;
    }
    lbl_status_.set_text("Downloaded to " + local_path);
}

void MainWindow::on_btn_pause_upload_clicked() {
    string remote_path = entry_path_.get_text();
    string err;
    if (!client_.pause_upload(remote_path, 0, err)) {
        lbl_status_.set_text("Pause upload failed: " + err);
        return;
    }
    lbl_status_.set_text("Upload paused on server");
}

void MainWindow::on_btn_resume_upload_clicked() {
    string remote_path = entry_path_.get_text();
    Gtk::FileChooserDialog dialog("Select local file to resume upload", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Open", Gtk::RESPONSE_OK);
    if (dialog.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Resume upload canceled");
        return;
    }
    string local_path = dialog.get_filename();
    string err;
    if (!client_.continue_upload(remote_path, local_path, err)) {
        lbl_status_.set_text("Resume upload failed: " + err);
        return;
    }
    lbl_status_.set_text("Upload resumed and completed");
    refresh_file_list();
}

void MainWindow::on_btn_pause_download_clicked() {
    string remote_path = entry_path_.get_text();
    Gtk::FileChooserDialog dialog("Select partial download file", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Open", Gtk::RESPONSE_OK);
    if (dialog.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Pause download canceled");
        return;
    }
    string local_path = dialog.get_filename();
    uint64_t offset = 0;
    std::ifstream ifs(local_path, std::ios::binary | std::ios::ate);
    if (ifs) offset = (uint64_t)ifs.tellg();

    string err;
    if (!client_.pause_download(remote_path, offset, err)) {
        lbl_status_.set_text("Pause download failed: " + err);
        return;
    }
    lbl_status_.set_text("Download paused at offset " + to_string(offset));
}

void MainWindow::on_btn_resume_download_clicked() {
    string remote_path = entry_path_.get_text();
    Gtk::FileChooserDialog dialog("Select local file to append download", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Open", Gtk::RESPONSE_OK);
    if (dialog.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Resume download canceled");
        return;
    }
    string local_path = dialog.get_filename();
    string err;
    if (!client_.continue_download(remote_path, local_path, err)) {
        lbl_status_.set_text("Resume download failed: " + err);
        return;
    }
    lbl_status_.set_text("Download resumed and completed");
}

void MainWindow::on_btn_unzip_clicked() {
    string zip_path = entry_path_.get_text();
    string target = entry_unzip_target_.get_text();
    if (target.empty()) {
        // If user didn't specify, default to folder named after zip (without .zip)
        std::filesystem::path p(zip_path);
        string stem = p.stem().string();
        if (!stem.empty())
            target = stem;
    }
    string err;
    if (!client_.unzip_remote(zip_path, target, err)) {
        lbl_status_.set_text("Unzip failed: " + err);
        return;
    }
    lbl_status_.set_text("Unzipped on server");
    refresh_file_list();
}

void MainWindow::on_btn_create_folder_clicked() {
    // Ask for new folder name
    Gtk::Dialog dlg("Folder name", *this, true);
    Gtk::Box *box = dlg.get_content_area();
    Gtk::Entry entry;
    entry.set_placeholder_text("New folder name");
    box->pack_start(entry, Gtk::PACK_EXPAND_WIDGET);
    dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("_Create", Gtk::RESPONSE_OK);
    dlg.show_all_children();
    if (dlg.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Create folder canceled");
        return;
    }
    string name = entry.get_text();
    if (name.empty()) { lbl_status_.set_text("Folder name required"); return; }

    string base = entry_path_.get_text();
    // If selected path is a folder, create inside it; otherwise use root or parent
    string new_path;
    if (!base.empty() && base.back() != '/') {
        // Treat base as folder if it exists as folder in tree
        // Look up selected node flag
        bool found_folder = false;
        auto sel = file_list_view_.get_selection();
        if (sel) {
            auto it = sel->get_selected();
            if (it && (*it)[columns_.is_folder]) {
                found_folder = true;
            }
        }
        if (found_folder) new_path = base + "/" + name;
        else new_path = name;
    } else {
        new_path = base + name;
    }

    string err;
    if (!client_.create_remote_folder(new_path, err)) {
        lbl_status_.set_text("Create folder failed: " + err);
        return;
    }
    lbl_status_.set_text("Folder created: " + new_path);
    refresh_file_list();
}

void MainWindow::on_btn_rename_clicked() {
    string src = entry_path_.get_text();
    string dst = entry_target_.get_text();
    if (src.empty()) { lbl_status_.set_text("Select source path"); return; }
    if (dst.empty()) {
        Gtk::Dialog dlg("New name", *this, true);
        Gtk::Entry entry;
        entry.set_text(src);
        dlg.get_content_area()->pack_start(entry, Gtk::PACK_EXPAND_WIDGET);
        dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        dlg.add_button("_Rename", Gtk::RESPONSE_OK);
        dlg.show_all_children();
        if (dlg.run() != Gtk::RESPONSE_OK) { lbl_status_.set_text("Rename canceled"); return; }
        dst = entry.get_text();
    }
    string err;
    if (!client_.rename_remote(src, dst, err)) {
        lbl_status_.set_text("Rename failed: " + err);
        return;
    }
    lbl_status_.set_text("Renamed to " + dst);
    refresh_file_list();
}

void MainWindow::on_btn_move_clicked() {
    string src = entry_path_.get_text();
    if (src.empty()) { lbl_status_.set_text("Select source path"); return; }
    std::string dst_folder;
    if (!choose_folder_dialog(dst_folder)) { lbl_status_.set_text("Move canceled"); return; }
    // Place inside chosen folder (keep filename)
    std::filesystem::path p(src);
    std::string dst = dst_folder.empty() ? p.filename().string()
                                         : (std::filesystem::path(dst_folder) / p.filename()).generic_string();
    string err;
    if (!client_.move_remote(src, dst, err)) {
        lbl_status_.set_text("Move failed: " + err);
        return;
    }
    lbl_status_.set_text("Moved to " + dst);
    refresh_file_list();
}

void MainWindow::on_btn_copy_clicked() {
    string src = entry_path_.get_text();
    string dst = entry_target_.get_text();
    if (src.empty() || dst.empty()) { lbl_status_.set_text("Fill source (main input) and target"); return; }
    string err;
    if (!client_.copy_remote(src, dst, err)) {
        lbl_status_.set_text("Copy failed: " + err);
        return;
    }
    lbl_status_.set_text("Copied to " + dst);
    refresh_file_list();
}

void MainWindow::on_btn_delete_clicked() {
    string path = entry_path_.get_text();
    if (path.empty()) { lbl_status_.set_text("Enter path to delete"); return; }
    string err;
    if (!client_.delete_remote(path, err)) {
        lbl_status_.set_text("Delete failed: " + err);
        return;
    }
    lbl_status_.set_text("Deleted " + path);
    refresh_file_list();
}

void MainWindow::on_btn_restore_clicked() {
    string path = entry_path_.get_text();
    if (path.empty()) { lbl_status_.set_text("Enter path to restore"); return; }
    string err;
    if (!client_.restore_remote(path, err)) {
        lbl_status_.set_text("Restore failed: " + err);
        return;
    }
    lbl_status_.set_text("Restored " + path);
    refresh_file_list();
}

void MainWindow::on_btn_list_deleted_clicked() {
    string rows, err;
    if (!client_.list_deleted(rows, err)) {
        lbl_status_.set_text("List deleted failed: " + err);
        return;
    }
    Gtk::MessageDialog dlg(*this, "Deleted items", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(rows.empty() ? "No deleted items" : rows);
    dlg.run();
}

void MainWindow::refresh_file_list() {
    string paths, err;

    if (!client_.list_files_db(paths, err)) {
        lbl_status_.set_text("List error: " + err);
        return;
    }

    file_list_store_->clear();

    string current;
    for (char c : paths) {
        if (c == '\n') {
            if (!current.empty()) {
                // Tách name|size|is_folder
                size_t p1 = current.find('|');
                size_t p2 = current.find('|', p1 + 1);
                string path = current.substr(0, p1);
                string size = current.substr(p1 + 1, p2 - p1 - 1);
                bool is_folder = false;
                if (p2 != string::npos) {
                    string flag = current.substr(p2 + 1);
                    is_folder = (flag == "1");
                }
                add_path_to_tree(path, size, is_folder);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    lbl_status_.set_text("Loaded file list");
}

void MainWindow::on_file_selected() {
    auto sel = file_list_view_.get_selection();
    if (!sel) return;

    auto iter = sel->get_selected();
    if (!iter) return;

    Glib::ustring path_u = (*iter)[columns_.full_path];
    string path = path_u.raw();

    entry_path_.set_text(path);
    lbl_status_.set_text("Selected: " + path);
    // Prefill target with same path for convenience
    entry_target_.set_text(path);
}


bool MainWindow::update_online_count() {
    string response, err;

    if (!client_.send_raw_command("STATS", response, err)) {
        lbl_online_.set_text("Online: ?");
        return true; // vẫn chạy timer lần sau
    }

    size_t pos = response.find("online=");
    if (pos != string::npos) {
        size_t start = pos + 7;
        size_t end = response.find(' ', start);
        string count = response.substr(start, end - start);
        lbl_online_.set_text("Online: " + count);
    }

    return true; // quan trọng: TRUE để timer tiếp tục
}

std::vector<std::string> MainWindow::split_path(const std::string &path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

void MainWindow::add_path_to_tree(const std::string &path, const std::string &size_str, bool is_folder) {
    // size_str is bytes
    double kb = atof(size_str.c_str()) / 1024.0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f KB", kb);

    std::vector<std::string> parts = split_path(path);
    if (parts.empty()) return;

    Gtk::TreeModel::iterator parent_iter;
    Gtk::TreeModel::Children children = file_list_store_->children();
    std::string accumulated;

    for (size_t i = 0; i < parts.size(); ++i) {
        if (!accumulated.empty()) accumulated += "/";
        accumulated += parts[i];

        Gtk::TreeModel::iterator found;
        Gtk::TreeModel::Children current_children = parent_iter ? (*parent_iter).children() : children;
        for (auto it = current_children.begin(); it != current_children.end(); ++it) {
            Glib::ustring fp = (*it)[columns_.full_path];
            if (fp == accumulated) {
                found = it;
                break;
            }
        }

        if (!found) {
            Gtk::TreeModel::Row row = *(file_list_store_->append(current_children));
            row[columns_.name] = parts[i];
            row[columns_.full_path] = accumulated;
            row[columns_.is_folder] = (i + 1 < parts.size()) ? true : is_folder;
            // Only set size on leaf
            if (i + 1 == parts.size() && !is_folder) {
                row[columns_.size] = buf;
            } else {
                row[columns_.size] = "";
            }
            found = row;
        } else {
            // If this is leaf and size provided, update size
            if (i + 1 == parts.size() && !is_folder) {
                (*found)[columns_.size] = buf;
            }
        }

        parent_iter = found;
    }
}

void MainWindow::upload_folder_recursive(const std::string &local_root,
                                         const std::string &remote_root,
                                         std::error_code &ec_out,
                                         std::string &err_out,
                                         bool &ok_out) {
    namespace fs = std::filesystem;
    fs::path root(local_root);
    std::string err;
    if (!client_.create_remote_folder(remote_root, err)) {
        ok_out = false;
        err_out = err;
        return;
    }
    for (auto &entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec_out)) {
        if (ec_out) { ok_out = false; err_out = ec_out.message(); return; }
        auto rel = fs::relative(entry.path(), root, ec_out);
        if (ec_out) { ok_out = false; err_out = ec_out.message(); return; }
        std::string remote_path = (fs::path(remote_root) / rel).generic_string();
        if (entry.is_directory()) {
            if (!client_.create_remote_folder(remote_path, err)) {
                ok_out = false; err_out = err; return;
            }
        } else if (entry.is_regular_file()) {
            if (!client_.upload_file(entry.path().string(), remote_path, err)) {
                ok_out = false; err_out = err; return;
            }
        }
    }
}

std::vector<std::string> MainWindow::collect_folder_paths() {
    std::vector<std::string> folders;
    // Only collect top-level folders (depth 1) that are not hidden/system.
    auto children = file_list_store_->children();
    for (auto iter = children.begin(); iter != children.end(); ++iter) {
        if (!(*iter)[columns_.is_folder]) continue;
        Glib::ustring fp = (*iter)[columns_.full_path];
        std::string path = fp.raw();
        std::string name = path.substr(path.find_last_of('/') + 1);
        if (name.rfind(".", 0) == 0 || name == "__MACOSX") continue;
        folders.push_back(path);
    }
    return folders;
}

bool MainWindow::choose_folder_dialog(std::string &out_path) {
    auto folders = collect_folder_paths();
    Gtk::Dialog dlg("Choose target folder", *this, true);
    dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("_OK", Gtk::RESPONSE_OK);
    Gtk::ComboBoxText combo;
    combo.append(""); // root
    for (auto &f : folders) combo.append(f);
    combo.set_active(0);
    dlg.get_content_area()->pack_start(combo, Gtk::PACK_EXPAND_WIDGET);
    dlg.show_all_children();
    if (dlg.run() != Gtk::RESPONSE_OK) return false;
    out_path = combo.get_active_text();
    // If user picked a file (shouldn't happen, but guard) or empty, keep root.
    if (out_path != "" && out_path.back() == '/') {
        while (!out_path.empty() && out_path.back() == '/') out_path.pop_back();
    }
    return true;
}
