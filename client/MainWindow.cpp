// ===== file: client/MainWindow.cpp =====
#include "MainWindow.hpp"
#include <filesystem>

using namespace std;

MainWindow::MainWindow(NetworkClient &&client, const string &username)
    : client_(std::move(client)),
      username_(username),
      vbox_(Gtk::ORIENTATION_VERTICAL),
      btn_load_("Load"),
      btn_save_("Save"),
      btn_upload_("Upload")
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

    vbox_.pack_start(*hbox, Gtk::PACK_SHRINK);
    vbox_.pack_start(lbl_status_, Gtk::PACK_SHRINK);

    lbl_online_.set_text("Online: ...");
    vbox_.pack_start(lbl_online_, Gtk::PACK_SHRINK);

    // Cập nhật mỗi 10 giây
    online_timer_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &MainWindow::update_online_count),
        1
    );



    // ==== File list + editor side-by-side ====
    file_list_store_ = Gtk::ListStore::create(columns_);
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
    Gtk::FileChooserDialog dialog("Select a text file", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Open", Gtk::RESPONSE_OK);

    auto filter = Gtk::FileFilter::create();
    filter->set_name("Text files");
    filter->add_pattern("*.txt");
    dialog.add_filter(filter);

    if (dialog.run() != Gtk::RESPONSE_OK) {
        lbl_status_.set_text("Upload canceled");
        return;
    }

    string local_path = dialog.get_filename();
    string remote_path = std::filesystem::path(local_path).filename().string();

    string err;
    if (!client_.upload_file(local_path, remote_path, err)) {
        lbl_status_.set_text("Upload failed: " + err);
        return;
    }

    lbl_status_.set_text("Uploaded " + remote_path + " successfully");
    refresh_file_list();
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
                // Tách name|size
                size_t pos = current.find('|');
                string name = current.substr(0, pos);
                string size = current.substr(pos + 1);

                // Đổi size bytes -> KB đẹp
                double kb = atof(size.c_str()) / 1024.0;
                char buf[32];
                sprintf(buf, "%.2f KB", kb);

                Gtk::TreeModel::Row row = *(file_list_store_->append());
                row[columns_.name] = name;
                row[columns_.size] = buf;

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

    Glib::ustring name_u = (*iter)[columns_.name];
    string name = name_u.raw();

    entry_path_.set_text(name);
    lbl_status_.set_text("Selected: " + name);
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