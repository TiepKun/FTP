// ===== file: client/main.cpp =====
#include <gtkmm.h>
#include "LoginWindow.hpp"

using namespace std;

int main(int argc, char *argv[]) {
    auto app = Gtk::Application::create(argc, argv, "com.fileshare.app");
    LoginWindow win;
    return app->run(win);
}
