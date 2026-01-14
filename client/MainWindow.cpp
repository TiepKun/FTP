// ===== file: client/MainWindow.cpp =====
#include "MainWindow.hpp"
#include "LoginWindow.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <sstream>

using namespace std;

MainWindow::MainWindow(NetworkClient &&client, const string &username)
    : client_(std::move(client)),
      username_(username),
      vbox_(Gtk::ORIENTATION_VERTICAL),
      btn_load_("Load"),
      btn_save_("Save"),
      btn_upload_("Upload"),
      btn_refresh_("Refresh"),
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
    hbox->pack_start(btn_refresh_, Gtk::PACK_SHRINK);
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
    actions_box_ = hbox3;
    entry_target_.set_placeholder_text("Target path (for rename/move/copy)");
    hbox3->pack_start(entry_target_, Gtk::PACK_EXPAND_WIDGET);
    Gtk::Button *btn_create_folder = Gtk::manage(new Gtk::Button("Create Folder"));
    Gtk::Button *btn_rename = Gtk::manage(new Gtk::Button("Rename"));
    Gtk::Button *btn_move = Gtk::manage(new Gtk::Button("Move"));
    Gtk::Button *btn_delete = Gtk::manage(new Gtk::Button("Delete"));
    Gtk::Button *btn_restore = Gtk::manage(new Gtk::Button("Restore"));
    Gtk::Button *btn_list_deleted = Gtk::manage(new Gtk::Button("Show Deleted"));
    hbox3->pack_start(*btn_create_folder, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_rename, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_move, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_delete, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_restore, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_list_deleted, Gtk::PACK_SHRINK);
    vbox_.pack_start(*hbox3, Gtk::PACK_SHRINK);
    vbox_.pack_start(lbl_status_, Gtk::PACK_SHRINK);
    progress_upload_.set_show_text(true);
    progress_upload_.set_fraction(0.0);
    vbox_.pack_start(progress_upload_, Gtk::PACK_SHRINK);
    progress_download_.set_show_text(true);
    progress_download_.set_fraction(0.0);
    vbox_.pack_start(progress_download_, Gtk::PACK_SHRINK);


    // Stats UI removed (online count)
    upload_timer_ = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &MainWindow::on_upload_progress_tick),
        100
    );
    download_timer_ = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &MainWindow::on_download_progress_tick),
        100
    );

    // Logout button: place at bottom-right and style red
    auto css = Gtk::CssProvider::create();
    try {
        css->load_from_data(
            ".logout { background-image: none; background-color: #d9534f; color: white; }"
        );
        btn_logout_.get_style_context()->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_USER);
        btn_logout_.get_style_context()->add_class("logout");
    } catch (...) {
        // ignore CSS errors
    }

    Gtk::Box *hbot = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    Gtk::Label *spacer = Gtk::manage(new Gtk::Label(""));
    spacer->set_hexpand(true);
    hbot->pack_start(*spacer, Gtk::PACK_EXPAND_WIDGET);
    hbot->pack_start(btn_logout_, Gtk::PACK_SHRINK);
    vbox_.pack_end(*hbot, Gtk::PACK_SHRINK);




    // ==== File list + editor side-by-side ====
    file_list_store_owned_ = Gtk::TreeStore::create(columns_);
    file_list_store_shared_ = Gtk::TreeStore::create(columns_);

    file_list_view_owned_.set_model(file_list_store_owned_);
    file_list_view_owned_.append_column("File", columns_.name);
    file_list_view_owned_.append_column("Size (KB)", columns_.size);

    file_list_view_shared_.set_model(file_list_store_shared_);
    file_list_view_shared_.append_column("File Shared", columns_.name);
    file_list_view_shared_.append_column("Size (KB)", columns_.size);

    file_list_view_owned_.get_selection()->signal_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_owned_selection_changed));
    file_list_view_shared_.get_selection()->signal_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_shared_selection_changed));

    Gtk::ScrolledWindow *sw_owned = Gtk::manage(new Gtk::ScrolledWindow());
    sw_owned->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw_owned->add(file_list_view_owned_);

    Gtk::ScrolledWindow *sw_shared = Gtk::manage(new Gtk::ScrolledWindow());
    sw_shared->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw_shared->add(file_list_view_shared_);

    Gtk::Box *left_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    Gtk::Label *lbl_my = Gtk::manage(new Gtk::Label("My Files"));
    lbl_my->set_margin_bottom(2);
    Gtk::Label *lbl_shared = Gtk::manage(new Gtk::Label("Shared Files"));
    lbl_shared->set_margin_bottom(2);
    left_box->pack_start(*lbl_my, Gtk::PACK_SHRINK);
    left_box->pack_start(*sw_owned, Gtk::PACK_EXPAND_WIDGET);
    left_box->pack_start(*lbl_shared, Gtk::PACK_SHRINK);
    left_box->pack_start(*sw_shared, Gtk::PACK_EXPAND_WIDGET);

    Gtk::ScrolledWindow *sw_right = Gtk::manage(new Gtk::ScrolledWindow());
    sw_right->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw_right->add(text_view_);

    Gtk::Paned *paned = Gtk::manage(new Gtk::Paned(Gtk::ORIENTATION_HORIZONTAL));
    paned->add1(*left_box);
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
    btn_refresh_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_refresh_clicked));
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
    btn_delete->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_delete_clicked));
    btn_restore->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_restore_clicked));
    btn_list_deleted->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_list_deleted_clicked));
    Gtk::Button *btn_share = Gtk::manage(new Gtk::Button("Share"));
    Gtk::Button *btn_change_role = Gtk::manage(new Gtk::Button("Change Role"));
    hbox3->pack_start(*btn_share, Gtk::PACK_SHRINK);
    hbox3->pack_start(*btn_change_role, Gtk::PACK_SHRINK);
    btn_share->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_share_clicked));
    btn_change_role->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_change_role_clicked));
    btn_logout_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_btn_logout_clicked));

    show_all_children();
    refresh_file_list();     // load file list automatically

    
}

void MainWindow::on_btn_load_clicked() {
    string path = entry_path_.get_text();
    string content, err;
    bool want_lock = !current_is_shared_ || current_can_edit_;

    if (!client_.get_text(path, content, err, want_lock)) {
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
    string new_version;

    if (!client_.put_text(path, content, new_version, err)) {
        lbl_status_.set_text("Save failed: " + err);
        return;
    }

    lbl_status_.set_text("Saved " + path);
    refresh_file_list();  // Add this line
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

    upload_total_ = std::filesystem::file_size(local_path);
    upload_sent_ = 0;
    uploading_ = true;

    btn_upload_.set_sensitive(false);
    lbl_status_.set_text("Uploading...");

    std::thread([this, local_path, remote_path]() {
        string err;
        bool ok = client_.upload_file_with_progress(
            local_path,
            remote_path,
            upload_sent_,
            err
        );

        Glib::signal_idle().connect_once([this, ok, err, remote_path]() {
            uploading_ = false;
            btn_upload_.set_sensitive(true);

            if (!ok) {
                lbl_status_.set_text("Upload failed: " + err);
            } else {
                lbl_status_.set_text("Uploaded " + remote_path);
                refresh_file_list();
                expand_and_select(remote_path);
            }
        });
    }).detach();

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
    // run download in background with progress
    download_total_ = 0;
    download_received_ = 0;
    downloading_ = true;

    btn_download_.set_sensitive(false);
    lbl_status_.set_text("Downloading...");

    std::thread([this, local_path, remote_path]() {
        string err;
        bool ok = client_.download_file_with_progress(remote_path, local_path, download_received_, download_total_, err);
        Glib::signal_idle().connect_once([this, ok, err, local_path]() {
            downloading_ = false;
            btn_download_.set_sensitive(true);
            progress_download_.set_fraction(0.0);
            progress_download_.set_text("");
            if (!ok) {
                lbl_status_.set_text("Download failed: " + err);
            } else {
                lbl_status_.set_text("Downloaded to " + local_path);
            }
        });
    }).detach();
}

void MainWindow::on_btn_pause_upload_clicked() {
    string remote_path = entry_path_.get_text();
    string err;
    if (uploading_) {
        // currently uploading: stop local upload thread by closing connection
        uploading_ = false;
        client_.close();
        lbl_status_.set_text("Upload paused (connection closed)");
        return;
    }

    // If not actively uploading, allow manual pause via protocol
    if (!client_.pause_upload(remote_path, err)) {
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
    // Resume in background with progress
    upload_total_ = 0;
    upload_sent_ = 0;
    uploading_ = true;
    btn_upload_.set_sensitive(false);
    lbl_status_.set_text("Resuming upload...");

    std::thread([this, remote_path, local_path]() {
        string err;
        if (!client_.ensure_connected(err)) {
            Glib::signal_idle().connect_once([this, err]() {
                uploading_ = false;
                btn_upload_.set_sensitive(true);
                lbl_status_.set_text(string("Resume upload failed: ") + err);
            });
            return;
        }

        // Ask server for resume offset
        string resp;
        if (!client_.send_raw_command(string("CONTINUE_UPLOAD ") + remote_path, resp, err)) {
            Glib::signal_idle().connect_once([this, err]() {
                uploading_ = false;
                btn_upload_.set_sensitive(true);
                lbl_status_.set_text(string("Resume upload failed: ") + err);
            });
            return;
        }

        // Expect: OK 100 Continue from <offset> size <remaining>
        if (resp.rfind("OK", 0) != 0) {
            Glib::signal_idle().connect_once([this, resp]() {
                uploading_ = false;
                btn_upload_.set_sensitive(true);
                lbl_status_.set_text(string("Resume upload failed: ") + resp);
            });
            return;
        }

        // parse offset and remaining
        uint64_t offset = 0, remaining = 0;
        size_t pos = resp.find("Continue from ");
        if (pos != string::npos) {
            pos += strlen("Continue from ");
            size_t pos2 = resp.find(" size ", pos);
            if (pos2 != string::npos) {
                string offs = resp.substr(pos, pos2 - pos);
                try { offset = stoull(offs); } catch (...) { offset = 0; }
                size_t pos3 = pos2 + strlen(" size ");
                string rem = resp.substr(pos3);
                try { remaining = stoull(rem); } catch (...) { remaining = 0; }
            }
        }

        if (remaining == 0) {
            Glib::signal_idle().connect_once([this, resp]() {
                uploading_ = false;
                btn_upload_.set_sensitive(true);
                lbl_status_.set_text(string("Resume upload failed: invalid server response: ") + resp);
            });
            return;
        }

        // Validate user selected the original file (not .tmp) and size
        if (local_path.size() >= 4 && local_path.substr(local_path.size() - 4) == ".tmp") {
            Glib::signal_idle().connect_once([this]() {
                uploading_ = false;
                btn_upload_.set_sensitive(true);
                lbl_status_.set_text("Please select the original local file (not the .tmp)");
            });
            return;
        }

        uint64_t local_size = 0;
        try { local_size = std::filesystem::file_size(local_path); } catch (...) { local_size = 0; }
        if (local_size < offset) {
            Glib::signal_idle().connect_once([this, offset, local_size]() {
                uploading_ = false;
                btn_upload_.set_sensitive(true);
                lbl_status_.set_text("Local file is smaller than resume offset: " + to_string(local_size) + " < " + to_string(offset));
            });
            return;
        }

        // All good — start streaming remaining bytes
        upload_total_ = offset + remaining;
        upload_sent_ = offset;
        uploading_ = true;

        bool ok = client_.resume_upload_stream(remote_path, local_path, offset, upload_sent_, upload_total_, err);

        Glib::signal_idle().connect_once([this, ok, err, remote_path]() {
            uploading_ = false;
            btn_upload_.set_sensitive(true);
            progress_upload_.set_fraction(0.0);
            progress_upload_.set_text("");
            if (!ok) {
                lbl_status_.set_text("Resume upload failed: " + err);
            } else {
                lbl_status_.set_text("Upload resumed and completed: " + remote_path);
                refresh_file_list();
                expand_and_select(remote_path);
            }
        });
    }).detach();
}

void MainWindow::on_btn_pause_download_clicked() {
    string remote_path = entry_path_.get_text();
    if (downloading_) {
        // stop current download and close connection; server will record offset on disconnect
        downloading_ = false;
        client_.close();
        lbl_status_.set_text("Download paused (connection closed)");
        return;
    }

    // If not currently downloading, allow manual pause by selecting partial file
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
    // run resume in background with progress
    download_received_ = 0;
    downloading_ = true;
    btn_download_.set_sensitive(false);
    lbl_status_.set_text("Resuming download...");

    std::thread([this, local_path, remote_path]() {
        string err;
        // ensure we are connected and authenticated before attempting resume
        if (!client_.ensure_connected(err)) {
            Glib::signal_idle().connect_once([this, err]() {
                downloading_ = false;
                btn_download_.set_sensitive(true);
                progress_download_.set_fraction(0.0);
                progress_download_.set_text("");
                lbl_status_.set_text(string("Resume download failed: ") + err);
            });
            return;
        }

        bool ok = client_.continue_download_with_progress(remote_path, local_path, download_received_, download_total_, err);
        Glib::signal_idle().connect_once([this, ok, err, local_path]() {
            downloading_ = false;
            btn_download_.set_sensitive(true);
            progress_download_.set_fraction(0.0);
            progress_download_.set_text("");
            if (!ok) {
                lbl_status_.set_text("Resume download failed: " + err);
            } else {
                lbl_status_.set_text("Download resumed and completed: " + local_path);
            }
        });
    }).detach();
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
        auto sel = file_list_view_owned_.get_selection();
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
    // Keep inputs/selection in sync with the new name to avoid stale paths.
    entry_path_.set_text(dst);
    entry_target_.set_text(dst);
    refresh_file_list();
    expand_and_select(dst);
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

void MainWindow::on_btn_logout_clicked() {
    string resp, err;
    if (!client_.send_raw_command("LOGOUT", resp, err)) {
        lbl_status_.set_text("Logout failed: " + err);
        return;
    }
    client_.close();

    // Create login window and show it, then close this main window
    LoginWindow *login = new LoginWindow();
    auto app = Glib::RefPtr<Gtk::Application>::cast_dynamic(get_application());
    if (app) app->add_window(*login);
    login->signal_hide().connect([login]() { delete login; });

    // Ensure this MainWindow is deleted when hidden
    // update_online_count removed (no online stats shown)
    this->signal_hide().connect([this]() { delete this; });

    login->show();
    hide();
}

void MainWindow::refresh_file_list() {
    string paths, err;

    if (!client_.list_files_db(paths, err)) {
        lbl_status_.set_text("List error: " + err);
        return;
    }

    file_list_store_owned_->clear();
    file_list_store_shared_->clear();
    latest_entries_owned_.clear();
    std::map<std::string, RoleInfo> shared_roles_new;

    string current;
    for (char c : paths) {
        if (c == '\n') {
            if (!current.empty()) {
                // Parse: path|size|is_folder|owner|can_view|can_download|can_edit
                vector<string> tokens;
                string tmp;
                for (char cc : current) {
                    if (cc == '|') { tokens.push_back(tmp); tmp.clear(); }
                    else tmp.push_back(cc);
                }
                tokens.push_back(tmp);

                if (tokens.size() >= 7) {
                    string path = tokens[0];
                    string size_str = tokens[1];
                    bool is_folder = (tokens[2] == "1");
                    string owner = tokens[3];
                    bool can_download = (tokens[5] == "1");
                    bool can_edit = (tokens[6] == "1");
                    bool is_shared = (owner != username_);
                    if (!is_shared) {
                        latest_entries_owned_.push_back({path, is_folder, is_shared, can_download, can_edit});
                        add_path_to_tree(path, size_str, is_folder, owner, is_shared, can_download, can_edit, file_list_store_owned_);
                    } else {
                        shared_roles_new[path] = {true, can_download, can_edit};
                        add_path_to_tree(path, size_str, is_folder, owner, is_shared, can_download, can_edit, file_list_store_shared_);
                    }
                }
                current.clear();
            }
        } else {
            current += c;
        }
    }
    // Handle last line if no trailing newline
    if (!current.empty()) {
        vector<string> tokens;
        string tmp;
        for (char cc : current) {
            if (cc == '|') { tokens.push_back(tmp); tmp.clear(); }
            else tmp.push_back(cc);
        }
        tokens.push_back(tmp);
        if (tokens.size() >= 7) {
            string path = tokens[0];
            string size_str = tokens[1];
            bool is_folder = (tokens[2] == "1");
            string owner = tokens[3];
            bool can_download = (tokens[5] == "1");
            bool can_edit = (tokens[6] == "1");
            bool is_shared = (owner != username_);
            if (!is_shared) {
                latest_entries_owned_.push_back({path, is_folder, is_shared, can_download, can_edit});
                add_path_to_tree(path, size_str, is_folder, owner, is_shared, can_download, can_edit, file_list_store_owned_);
            } else {
                shared_roles_new[path] = {true, can_download, can_edit};
                add_path_to_tree(path, size_str, is_folder, owner, is_shared, can_download, can_edit, file_list_store_shared_);
            }
        }
    }

    // Detect role changes for shared entries
    for (auto &p : shared_roles_new) {
        auto it = shared_roles_prev_.find(p.first);
        if (it != shared_roles_prev_.end()) {
            if (it->second.download != p.second.download || it->second.edit != p.second.edit || it->second.view != p.second.view) {
                std::string old_role = it->second.edit ? "Edit" : (it->second.download ? "Download" : "View");
                std::string new_role = p.second.edit ? "Edit" : (p.second.download ? "Download" : "View");
                lbl_status_.set_text("Role updated for shared \"" + p.first + "\": " + old_role + " -> " + new_role);
                break;
            }
        }
    }
    shared_roles_prev_ = std::move(shared_roles_new);

    lbl_status_.set_text("Loaded file list");
    reset_selection_state();
}

void MainWindow::on_btn_refresh_clicked() {
    refresh_file_list();
}

void MainWindow::on_file_selected() {
    // Deprecated; use owned/shared handlers instead
}


// online stats removed

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

Gtk::TreeModel::iterator MainWindow::find_iter_by_path(const std::string &path, const Gtk::TreeModel::Children &children) {
    for (auto it = children.begin(); it != children.end(); ++it) {
        Glib::ustring fp = (*it)[columns_.full_path];
        if (fp == path) return it;
        auto child = find_iter_by_path(path, (*it).children());
        if (child) return child;
    }
    return Gtk::TreeModel::iterator();
}

void MainWindow::expand_and_select(const std::string &path) {
    auto it = find_iter_by_path(path, file_list_store_owned_->children());
    if (!it) return;
    Gtk::TreeModel::Path tree_path = file_list_store_owned_->get_path(it);
    // Expand only parents (not the target) so folder contents stay collapsed until user opens.
    if (tree_path.size() > 1) {
        Gtk::TreeModel::Path parent_path = tree_path;
        parent_path.up();
        file_list_view_owned_.expand_to_path(parent_path);
    }
    file_list_view_owned_.get_selection()->select(tree_path);
    file_list_view_owned_.scroll_to_row(tree_path);
}

void MainWindow::add_path_to_tree(const std::string &path,
                                  const std::string &size_str,
                                  bool is_folder,
                                  const std::string &owner,
                                  bool is_shared,
                                  bool can_download,
                                  bool can_edit,
                                  Glib::RefPtr<Gtk::TreeStore> store) {
    
    // Convert bytes to KB
    double size_bytes = 0;
    try {
        size_bytes = std::stoll(size_str);
    } catch (...) {
        size_bytes = 0;
    }
    double kb = size_bytes / 1024.0;
    
    char buf[32];
    if (is_folder) {
        snprintf(buf, sizeof(buf), "-");  // No size for folders
    } else {
        snprintf(buf, sizeof(buf), "%.2f KB", kb);
    }

    std::vector<std::string> parts = split_path(path);
    if (parts.empty()) return;

    Gtk::TreeModel::iterator parent_iter;
    Gtk::TreeModel::Children children = store->children();
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
            Gtk::TreeModel::Row row = *(store->append(current_children));
            row[columns_.name] = parts[i];
            row[columns_.full_path] = accumulated;
            row[columns_.is_folder] = (i + 1 < parts.size()) ? true : is_folder;
            row[columns_.owner] = owner;
            row[columns_.is_shared] = is_shared;
            row[columns_.can_download] = can_download;
            row[columns_.can_edit] = can_edit;
            // Only set size on leaf node
            if (i + 1 == parts.size() && !is_folder) {
                row[columns_.size] = buf;
            } else {
                row[columns_.size] = "";
            }
            found = row;
        } else {
            // Update size if this is leaf
            if (i + 1 == parts.size() && !is_folder) {
                (*found)[columns_.size] = buf;
            }
            (*found)[columns_.owner] = owner;
            (*found)[columns_.is_shared] = is_shared;
            (*found)[columns_.can_download] = can_download;
            (*found)[columns_.can_edit] = can_edit;
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
    auto children = file_list_store_owned_->children();
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

void MainWindow::on_btn_share_clicked() {
    // Build dialog
    Gtk::Dialog dlg("Share", *this, true);
    dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("_Send", Gtk::RESPONSE_OK);

    Gtk::Grid grid;
    grid.set_row_spacing(6);
    grid.set_column_spacing(6);

    Gtk::Label lbl_to("To:");
    Gtk::Entry entry_to;
    Gtk::Label lbl_from("From:");
    Gtk::Entry entry_from;
    entry_from.set_text(username_);
    entry_from.set_editable(false);
    Gtk::Label lbl_desc("Description:");
    Gtk::Entry entry_desc;
    Gtk::Label lbl_path("Folder/File:");
    Gtk::ComboBoxText combo_path;
    combo_path.append(""); // require selection
    for (auto &e : latest_entries_owned_) {
        combo_path.append(e.path);
    }

    Gtk::Label lbl_access("General Access:");
    Gtk::ComboBoxText combo_access;
    combo_access.append("View");
    combo_access.append("Download");
    combo_access.append("Editor");
    combo_access.set_active(0);

    grid.attach(lbl_to, 0, 0, 1, 1);
    grid.attach(entry_to, 1, 0, 2, 1);
    grid.attach(lbl_from, 0, 1, 1, 1);
    grid.attach(entry_from, 1, 1, 2, 1);
    grid.attach(lbl_desc, 0, 2, 1, 1);
    grid.attach(entry_desc, 1, 2, 2, 1);
    grid.attach(lbl_path, 0, 3, 1, 1);
    grid.attach(combo_path, 1, 3, 2, 1);
    grid.attach(lbl_access, 0, 4, 1, 1);
    grid.attach(combo_access, 1, 4, 2, 1);

    dlg.get_content_area()->pack_start(grid);
    dlg.show_all_children();

    if (dlg.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Share canceled");
        return;
    }

    std::string to_user = entry_to.get_text();
    std::string sel_path = combo_path.get_active_text();
    if (to_user.empty() || sel_path.empty()) {
        lbl_status_.set_text("To and Folder/File are required");
        return;
    }

    std::string access = combo_access.get_active_text();
    bool can_view = true;
    bool can_download = (access == "Download" || access == "Editor");
    bool can_edit = (access == "Editor");

    std::string err;
    if (!client_.set_permission(sel_path, to_user, can_view, can_download, can_edit, err)) {
        lbl_status_.set_text("Share failed: " + err);
        return;
    }

    lbl_status_.set_text("Shared " + sel_path + " with " + to_user + " (" + access + ")");
}

void MainWindow::on_btn_change_role_clicked() {
    // Dialog for picking owned path and grantee
    Gtk::Dialog dlg("Change Role", *this, true);
    dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("_Apply", Gtk::RESPONSE_OK);

    Gtk::Grid grid;
    grid.set_row_spacing(6);
    grid.set_column_spacing(6);

    Gtk::Label lbl_path("Folder/File:");
    Gtk::ComboBoxText combo_path;
    combo_path.append("");
    for (auto &e : latest_entries_owned_) combo_path.append(e.path);
    combo_path.set_active(0);

    Gtk::Label lbl_user("User:");
    Gtk::ComboBoxText combo_user;

    Gtk::Label lbl_role("Role:");
    Gtk::ComboBoxText combo_role;
    combo_role.append("View");
    combo_role.append("Download");
    combo_role.append("Edit");
    combo_role.set_active(0);

    grid.attach(lbl_path, 0, 0, 1, 1);
    grid.attach(combo_path, 1, 0, 2, 1);
    grid.attach(lbl_user, 0, 1, 1, 1);
    grid.attach(combo_user, 1, 1, 2, 1);
    grid.attach(lbl_role, 0, 2, 1, 1);
    grid.attach(combo_role, 1, 2, 2, 1);
    dlg.get_content_area()->pack_start(grid);
    dlg.show_all_children();

    // Load ACL when path changes
    combo_path.signal_changed().connect([this, &combo_path, &combo_user, &combo_role]() {
        combo_user.remove_all();
        combo_role.set_active(0);
        std::string path = combo_path.get_active_text();
        if (path.empty()) return;
        std::string rows, err;
        if (!client_.list_acl(path, rows, err)) {
            lbl_status_.set_text("Load ACL failed: " + err);
            return;
        }
        std::istringstream ss(rows);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            combo_user.append(line);
        }
        combo_user.set_active(0);
        if (combo_user.get_active_row_number() >= 0) {
            std::string val = combo_user.get_active_text();
            std::vector<std::string> parts;
            std::string cur;
            for (char c : val) {
                if (c == '|') { parts.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            parts.push_back(cur);
            if (parts.size() >= 4) {
                bool dv = parts[2] == "1";
                bool de = parts[3] == "1";
                if (de) combo_role.set_active_text("Edit");
                else if (dv) combo_role.set_active_text("Download");
                else combo_role.set_active_text("View");
            }
        }
    });

    if (dlg.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Change role canceled");
        return;
    }

    std::string path = combo_path.get_active_text();
    std::string user_row = combo_user.get_active_text();
    if (path.empty() || user_row.empty()) {
        lbl_status_.set_text("Select path and user");
        return;
    }

    // Parse user_row: username|v|d|e
    std::vector<std::string> parts;
    std::string cur;
    for (char c : user_row) {
        if (c == '|') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    if (parts.size() < 4) {
        lbl_status_.set_text("Invalid ACL row");
        return;
    }
    std::string username = parts[0];
    bool can_view = parts[1] == "1";
    bool can_download = parts[2] == "1";
    bool can_edit = parts[3] == "1";

    std::string role = combo_role.get_active_text();
    if (role == "View") { can_view = true; can_download = false; can_edit = false; }
    else if (role == "Download") { can_view = true; can_download = true; can_edit = false; }
    else if (role == "Edit") { can_view = true; can_download = true; can_edit = true; }

    std::string err;
    if (!client_.set_permission(path, username, can_view, can_download, can_edit, err)) {
        lbl_status_.set_text("Change role failed: " + err);
        return;
    }

    std::string new_role = can_edit ? "Edit" : (can_download ? "Download" : "View");
    lbl_status_.set_text("Role updated for " + username + " -> " + new_role);
    refresh_file_list();
}
bool MainWindow::on_upload_progress_tick() {
    if (!uploading_) {
        progress_upload_.set_fraction(0.0);
        progress_upload_.set_text("");
        return true;
    }

    if (upload_total_ == 0) return true;

    double frac = (double)upload_sent_.load() / (double)upload_total_;
    if (frac > 1.0) frac = 1.0;

    int percent = (int)(frac * 100);
    progress_upload_.set_fraction(frac);
    progress_upload_.set_text(std::to_string(percent) + "%");

    return true;
}

bool MainWindow::on_download_progress_tick() {
    if (!downloading_) {
        progress_download_.set_fraction(0.0);
        progress_download_.set_text("");
        return true;
    }

    if (download_total_ == 0) return true;

    double frac = (double)download_received_.load() / (double)download_total_;
    if (frac > 1.0) frac = 1.0;

    int percent = (int)(frac * 100);
    progress_download_.set_fraction(frac);
    progress_download_.set_text(std::to_string(percent) + "%");

    return true;
}

void MainWindow::on_owned_selection_changed() {
    auto sel = file_list_view_owned_.get_selection();
    if (!sel) return;
    auto iter = sel->get_selected();
    if (!iter) return;

    Glib::ustring path_u = (*iter)[columns_.full_path];
    string path = path_u.raw();
    entry_path_.set_text(path);
    entry_target_.set_text(path);
    lbl_status_.set_text("Selected: " + path);
    current_is_shared_ = false;
    current_can_download_ = true;
    current_can_edit_ = true;
    text_view_.set_editable(true);
    update_action_sensitivity(false, true, true);
}

void MainWindow::on_shared_selection_changed() {
    auto sel = file_list_view_shared_.get_selection();
    if (!sel) return;
    auto iter = sel->get_selected();
    if (!iter) return;

    Glib::ustring path_u = (*iter)[columns_.full_path];
    string path = path_u.raw();
    bool can_download = (*iter)[columns_.can_download];
    bool can_edit = (*iter)[columns_.can_edit];
    Glib::ustring owner_u = (*iter)[columns_.owner];
    string owner = owner_u.raw();

    entry_path_.set_text(path);
    entry_target_.set_text(""); // avoid accidental rename/move using stale path
    lbl_status_.set_text("Shared " + path + " from " + owner + (can_edit ? " (Edit)" : (can_download ? " (Download)" : " (View)")));

    current_is_shared_ = true;
    current_can_download_ = can_download;
    current_can_edit_ = can_edit;
    text_view_.set_editable(can_edit);
    update_action_sensitivity(true, can_download, can_edit);
}

void MainWindow::update_action_sensitivity(bool is_shared, bool can_download, bool can_edit) {
    // Top-level actions
    btn_upload_.set_sensitive(!is_shared); // uploads only to own area
    btn_save_.set_sensitive(!is_shared || can_edit);
    btn_load_.set_sensitive(true); // view is allowed if listed
    btn_download_.set_sensitive(!is_shared || can_download);

    // File operations bar
    bool allow_full_ops = !is_shared; // keep destructive ops to owner only
    btn_pause_up_.set_sensitive(!is_shared);
    btn_resume_up_.set_sensitive(!is_shared);
    btn_pause_down_.set_sensitive(!is_shared || can_download);
    btn_resume_down_.set_sensitive(!is_shared || can_download);
    btn_unzip_.set_sensitive(!is_shared);

    // Disable whole action bar for shared items to avoid destructive ops (server only supports owner).
    if (actions_box_) {
        for (auto *child : actions_box_->get_children()) {
            if (auto widget = dynamic_cast<Gtk::Widget*>(child)) widget->set_sensitive(!is_shared);
        }
    }
}

void MainWindow::reset_selection_state() {
    current_is_shared_ = false;
    current_can_download_ = true;
    current_can_edit_ = true;
    text_view_.set_editable(true);
    update_action_sensitivity(false, true, true);
}
