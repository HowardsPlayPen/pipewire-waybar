#include "common/config.h"
#include "common/log.h"
#include "pipewire-waybar/dbus_interface.h"

#include <sdbus-c++/sdbus-c++.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

using SinkRow = sdbus::Struct<std::string, std::string, std::string, bool, bool>;

struct App {
    std::unique_ptr<sdbus::IConnection> conn;
    std::unique_ptr<sdbus::IProxy> proxy;
    phc::Config cfg;
    GtkApplication* gtk{nullptr};
    GtkWindow* window{nullptr};
    GtkWidget* menu{nullptr};
};

std::string icon_for(const phc::Config& cfg, const std::string& type) {
    auto it = cfg.icons.find(type);
    if (it != cfg.icons.end()) return it->second;
    auto fb = cfg.icons.find("unknown");
    return fb != cfg.icons.end() ? fb->second : "";
}

std::vector<SinkRow> list_sinks(sdbus::IProxy& p) {
    std::vector<SinkRow> rows;
    p.callMethod(phc::dbus::kMethodListSinks)
     .onInterface(phc::dbus::kInterface)
     .storeResultsTo(rows);
    return rows;
}

void set_chosen(sdbus::IProxy& p, const std::string& name) {
    p.callMethod(phc::dbus::kMethodSetChosen)
     .onInterface(phc::dbus::kInterface)
     .withArguments(name);
}

extern "C" void on_row_activated(GtkListBox*, GtkListBoxRow* row, gpointer user_data) {
    App* app = static_cast<App*>(user_data);
    const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "phc-name"));
    if (!name) return;
    try {
        set_chosen(*app->proxy, name);
    } catch (const std::exception& e) {
        std::cerr << "picker: SetChosen failed: " << e.what() << "\n";
    }
    gtk_window_close(app->window);
}

extern "C" gboolean on_key_pressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType,
                                   gpointer user_data) {
    App* app = static_cast<App*>(user_data);
    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(app->window);
        return TRUE;
    }
    return FALSE;
}

extern "C" void on_root_click(GtkGestureClick*, int, double x, double y, gpointer user_data) {
    App* app = static_cast<App*>(user_data);
    if (!app->menu) return;
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(app->menu, GTK_WIDGET(app->window), &bounds)) return;
    if (x < bounds.origin.x || x > bounds.origin.x + bounds.size.width ||
        y < bounds.origin.y || y > bounds.origin.y + bounds.size.height) {
        gtk_window_close(app->window);
    }
}

GtkWidget* build_row(const App& app, const SinkRow& r) {
    const auto& [name, type, node_name, available, chosen] = r;

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    GtkWidget* icon = gtk_label_new(icon_for(app.cfg, type).c_str());
    gtk_widget_set_size_request(icon, 24, -1);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget* name_label = gtk_label_new(name.c_str());
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);
    gtk_widget_set_hexpand(name_label, TRUE);
    gtk_box_append(GTK_BOX(box), name_label);

    if (chosen) {
        GtkWidget* mark = gtk_label_new("✓");
        gtk_box_append(GTK_BOX(box), mark);
    }
    if (!available) {
        GtkWidget* tag = gtk_label_new("(unavailable)");
        gtk_widget_add_css_class(tag, "dim-label");
        gtk_box_append(GTK_BOX(box), tag);
    }

    std::string tip = "type: " + type + "\nnode: " + node_name;
    if (chosen) tip += "\n(currently chosen)";
    if (!available) tip += "\n(not currently in graph)";
    gtk_widget_set_tooltip_text(box, tip.c_str());

    return box;
}

void build_ui(App& app) {
    GtkWidget* window = gtk_application_window_new(app.gtk);
    app.window = GTK_WINDOW(window);
    gtk_window_set_title(app.window, "Sink picker");
    gtk_window_set_decorated(app.window, FALSE);

    gtk_layer_init_for_window(app.window);
    gtk_layer_set_layer(app.window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(app.window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(app.window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(app.window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(app.window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_keyboard_mode(app.window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    static GtkCssProvider* provider = nullptr;
    if (!provider) {
        provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider,
            "window.phc-picker, window.phc-picker.background {"
            "  background-color: transparent;"
            "  background-image: none;"
            "  box-shadow: none;"
            "}");
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    gtk_widget_add_css_class(GTK_WIDGET(app.window), "phc-picker");

    GtkWidget* listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_widget_set_halign(listbox, GTK_ALIGN_END);
    gtk_widget_set_valign(listbox, GTK_ALIGN_START);
    gtk_widget_set_margin_top(listbox, 36);
    gtk_widget_set_margin_end(listbox, 8);
    gtk_widget_set_size_request(listbox, 320, -1);
    app.menu = listbox;

    std::vector<SinkRow> rows;
    try { rows = list_sinks(*app.proxy); }
    catch (const std::exception& e) {
        GtkWidget* err = gtk_label_new(("daemon unreachable: " + std::string(e.what())).c_str());
        gtk_list_box_append(GTK_LIST_BOX(listbox), err);
    }

    // Heap-allocate names so the gpointer outlives the call.
    static std::vector<std::unique_ptr<std::string>> name_storage;
    for (auto& r : rows) {
        const auto& [name, type, node_name, available, chosen] = r;
        GtkWidget* widget = build_row(app, r);
        GtkWidget* row_w = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row_w), widget);

        auto stored = std::make_unique<std::string>(name);
        g_object_set_data(G_OBJECT(row_w), "phc-name", const_cast<char*>(stored->c_str()));
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row_w), TRUE);
        if (!available) gtk_widget_set_sensitive(row_w, FALSE);
        gtk_list_box_append(GTK_LIST_BOX(listbox), row_w);
        name_storage.push_back(std::move(stored));
    }

    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_row_activated), &app);

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), &app);
    gtk_widget_add_controller(GTK_WIDGET(app.window), keys);

    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_root_click), &app);
    gtk_widget_add_controller(GTK_WIDGET(app.window), GTK_EVENT_CONTROLLER(click));

    gtk_window_set_child(app.window, listbox);
    gtk_window_present(app.window);
}

extern "C" void on_activate(GApplication*, gpointer user_data) {
    App* app = static_cast<App*>(user_data);
    build_ui(*app);
}

}  // namespace

int main(int argc, char** argv) {
    App app;
    try {
        app.cfg = phc::Config::load(phc::Config::default_path());
    } catch (const std::exception&) {
        // Picker still works without config (just no icons).
    }

    try {
        app.conn = sdbus::createSessionBusConnection();
        app.proxy = sdbus::createProxy(*app.conn,
            sdbus::ServiceName{phc::dbus::kService},
            sdbus::ObjectPath{phc::dbus::kObject});
    } catch (const std::exception& e) {
        std::cerr << "picker: cannot connect to daemon: " << e.what() << "\n";
        return 1;
    }

    app.gtk = gtk_application_new("org.pipewire_waybar.Picker",
                                  G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app.gtk, "activate", G_CALLBACK(on_activate), &app);

    int status = g_application_run(G_APPLICATION(app.gtk), argc, argv);
    g_object_unref(app.gtk);
    return status;
}
