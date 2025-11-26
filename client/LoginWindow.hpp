// ===== file: client/LoginWindow.hpp =====
#pragma once
#include <gtkmm.h>
#include <string>
#include "NetworkClient.hpp"

using namespace std;

class LoginWindow : public Gtk::Window {
public:
    LoginWindow();
    ~LoginWindow() override = default;

protected:
    void on_btn_login_clicked();
    void on_btn_register_clicked();

    Gtk::Box vbox_;
    Gtk::Box btn_box_;
    Gtk::Entry entry_host_;
    Gtk::Entry entry_port_;
    Gtk::Entry entry_user_;
    Gtk::Entry entry_pass_;
    Gtk::Button btn_login_;
    Gtk::Button btn_register_;
    Gtk::Label lbl_status_;

    NetworkClient client_;
};
