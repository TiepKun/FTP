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

    NetworkClient client_;
    string username_;

    Gtk::Box vbox_;
    Gtk::Entry entry_path_;
    Gtk::Button btn_load_;
    Gtk::Button btn_save_;
    Gtk::ScrolledWindow scroll_;
    Gtk::TextView text_view_;
    Gtk::Label lbl_status_;
};
