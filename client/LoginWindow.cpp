// ===== file: client/LoginWindow.cpp =====
#include "LoginWindow.hpp"
#include "MainWindow.hpp"
#include <utility>

using namespace std;

LoginWindow::LoginWindow()
    : vbox_(Gtk::ORIENTATION_VERTICAL),
      btn_box_(Gtk::ORIENTATION_HORIZONTAL),
      btn_login_("Login"),
      btn_register_("Register") {

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
    btn_box_.set_spacing(5);
    btn_box_.pack_start(btn_login_, Gtk::PACK_EXPAND_WIDGET);
    btn_box_.pack_start(btn_register_, Gtk::PACK_EXPAND_WIDGET);
    vbox_.pack_start(btn_box_, Gtk::PACK_SHRINK);
    vbox_.pack_start(lbl_status_, Gtk::PACK_SHRINK);

    btn_login_.signal_clicked().connect(
        sigc::mem_fun(*this, &LoginWindow::on_btn_login_clicked));
    btn_register_.signal_clicked().connect(
        sigc::mem_fun(*this, &LoginWindow::on_btn_register_clicked));

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

    int port = 0;
    try {
        port = stoi(port_str);
    } catch (...) {
        lbl_status_.set_text("Port must be a number");
        return;
    }

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

    // Đăng ký cửa sổ mới với Gtk::Application để app không thoát khi ẩn Login
    auto app = Glib::RefPtr<Gtk::Application>::cast_dynamic(get_application());
    if (app) app->add_window(*main_win);

    // Giải phóng cửa sổ khi nó đóng
    main_win->signal_hide().connect([main_win]() { delete main_win; });

    main_win->show();
    hide(); // ẩn màn hình login
}

void LoginWindow::on_btn_register_clicked() {
    string host = entry_host_.get_text();
    string port_str = entry_port_.get_text();
    string user = entry_user_.get_text();
    string pass = entry_pass_.get_text();

    if (host.empty() || port_str.empty() || user.empty() || pass.empty()) {
        lbl_status_.set_text("Please fill all fields");
        return;
    }

    int port = 0;
    try {
        port = stoi(port_str);
    } catch (...) {
        lbl_status_.set_text("Port must be a number");
        return;
    }

    if (!client_.connect_to(host, port)) {
        lbl_status_.set_text("Cannot connect to server");
        return;
    }

    string err;
    if (!client_.register_user(user, pass, err)) {
        lbl_status_.set_text("Register failed: " + err);
        client_.close();
        return;
    }

    lbl_status_.set_text("Register success. You can login now.");
    client_.close();
}
