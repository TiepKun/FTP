// ===== file: client/LoginWindow.cpp =====
#include "LoginWindow.hpp"
#include "MainWindow.hpp"
#include <utility>

using namespace std;

LoginWindow::LoginWindow()
    : vbox_(Gtk::ORIENTATION_VERTICAL),
      btn_login_("Login") {

    set_title("File Share Login");
    set_default_size(300, 220);

    add(vbox_);
    vbox_.set_spacing(5);
    vbox_.set_margin_top(10);
    vbox_.set_margin_bottom(10);
    vbox_.set_margin_left(10);
    vbox_.set_margin_right(10);

    entry_host_.set_placeholder_text("Server host (e.g. 127.0.0.1)");
    entry_port_.set_placeholder_text("Port (e.g. 5051)");
    entry_user_.set_placeholder_text("Username");
    entry_pass_.set_placeholder_text("Password");
    entry_pass_.set_visibility(false);

    vbox_.pack_start(entry_host_, Gtk::PACK_SHRINK);
    vbox_.pack_start(entry_port_, Gtk::PACK_SHRINK);
    vbox_.pack_start(entry_user_, Gtk::PACK_SHRINK);
    vbox_.pack_start(entry_pass_, Gtk::PACK_SHRINK);
    vbox_.pack_start(btn_login_, Gtk::PACK_SHRINK);
    vbox_.pack_start(lbl_status_, Gtk::PACK_SHRINK);

    btn_login_.signal_clicked().connect(
        sigc::mem_fun(*this, &LoginWindow::on_btn_login_clicked));

    show_all_children();
}

void LoginWindow::on_btn_login_clicked() {
    string host = entry_host_.get_text();
    string port_str = entry_port_.get_text();
    string user = entry_user_.get_text();
    string pass = entry_pass_.get_text();

    if (host.empty() || port_str.empty() || user.empty() || pass.empty()) {
        lbl_status_.set_text("Please fill all fields");
        return;
    }

    int port = stoi(port_str);

    if (!client_.connect_to(host, port)) {
        lbl_status_.set_text("Cannot connect to server");
        return;
    }

    string err;
    if (!client_.auth(user, pass, err)) {
        lbl_status_.set_text("Auth failed: " + err);
        client_.close();
        return;
    }

    // mở main window
    MainWindow *main_win = new MainWindow(std::move(client_), user);
    main_win->show();
    hide(); // ẩn màn hình login
}
