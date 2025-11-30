// ===== file: client/MainWindow.cpp =====
#include "MainWindow.hpp"

using namespace std;

MainWindow::MainWindow(NetworkClient &&client, const string &username)
    : client_(std::move(client)),
      username_(username),
      vbox_(Gtk::ORIENTATION_VERTICAL),
      btn_load_("Load"),
      btn_save_("Save") {

    set_title("File Share - " + username_);
    set_default_size(600, 400);

    add(vbox_);
    vbox_.set_spacing(5);
    vbox_.set_margin_top(5);
    vbox_.set_margin_bottom(5);
    vbox_.set_margin_left(5);
    vbox_.set_margin_right(5);

    entry_path_.set_placeholder_text("Relative path (e.g. notes.txt)"); //relative path

    Gtk::Box *hbox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
    hbox->pack_start(entry_path_, Gtk::PACK_EXPAND_WIDGET);
    hbox->pack_start(btn_load_, Gtk::PACK_SHRINK);
    hbox->pack_start(btn_save_, Gtk::PACK_SHRINK);

    vbox_.pack_start(*hbox, Gtk::PACK_SHRINK);
    vbox_.pack_start(scroll_, Gtk::PACK_EXPAND_WIDGET);
    vbox_.pack_start(lbl_status_, Gtk::PACK_SHRINK);

    scroll_.add(text_view_);

    btn_load_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_load_clicked));
    btn_save_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_btn_save_clicked));

    show_all_children();
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
    Glib::RefPtr<Gtk::TextBuffer> buf = text_view_.get_buffer();
    string content = buf->get_text();
    string err;
    if (!client_.put_text(path, content, err)) {
        lbl_status_.set_text("Save failed: " + err);
        return;
    }
    lbl_status_.set_text("Saved " + path);
}
