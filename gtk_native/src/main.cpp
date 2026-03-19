#include <gtk/gtk.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <unordered_map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <json/json.h>

#include "logvcore/log_parser.h"

namespace {

enum Column {
    COL_TS = 0,
    COL_SVC,
    COL_LVL,
    COL_MSG,
    COL_BG,
    COL_FG,
    COL_RAW,
    COL_COUNT
};

constexpr const char* kLegacyRemoteCmd = "journalctl -f --no-pager 2>&1 | cat";
constexpr const char* kDefaultRemoteCmd = "journalctl -f --no-pager -o short-precise 2>&1 | cat";

struct UiState {
    GtkWidget* window{};
    GtkWidget* host_entry{};
    GtkWidget* connect_btn{};
    GtkWidget* clear_btn{};
    GtkWidget* services_btn{};
    GtkWidget* services_popover{};
    GtkWidget* services_box{};
    GtkWidget* filter_entry{};
    GtkWidget* autoscroll_check{};
    GtkWidget* status_label{};
    GtkWidget* count_label{};
    GtkWidget* tree_view{};
    GtkWidget* scroll{};
    GtkTreeViewColumn* ts_column{};

    GtkListStore* store{};

    std::vector<logvcore::LogEntry> entries;
    std::deque<logvcore::LogEntry> pending;
    std::mutex pending_mu;

    std::atomic<bool> running{false};
    std::thread reader_thread;

    pid_t child_pid{-1};
    int child_fd{-1};

    std::unordered_map<std::string, GtkWidget*> service_checks;
    bool newest_at_bottom{true};
    std::atomic<bool> reconnecting{false};
    guint reconnect_timer_id{0};

    struct AppConfig {
        std::vector<std::string> services{
            "cloud-connection-service",
            "storage-service",
            "audio-service",
            "analytics-service",
            "object-detection-service",
            "media-uploader",
            "oclea-webrtc-service",
            "oclea-clip-service",
            "illumination-service",
            "event-service",
            "oclea-service-bridge",
            "truman-supervisor",
            "oclea-continuous-recording-service",
            "oclea-pipeline-service",
        };
        std::vector<std::string> last_service_filter{};
        // Log levels to show; empty = show all.
        std::vector<std::string> level_filter{};
        std::string last_text_filter{};
        bool auto_scroll{true};
        bool newest_at_bottom{true};
        std::string ssh_host{"192.168.130.81"};
        std::string ssh_username{"root"};
        std::string ssh_command{kDefaultRemoteCmd};
        int ssh_port{22};
        int ssh_keepalive{30};   // ServerAliveInterval in seconds; 0 = disabled
        bool ssh_auto_reconnect{true};
        // Discovered services per-host (cleared when host changes).
        std::string last_host{};
        std::vector<std::string> discovered_services{};
        int max_lines{50000};
        int window_width{1400};
        int window_height{900};
        bool window_maximized{false};
    } cfg;
};

const std::filesystem::path& config_path() {
    static const std::filesystem::path p = []() {
        const char* home = std::getenv("HOME");
        if (!home || std::strlen(home) == 0) {
            return std::filesystem::path("./config.json");
        }
        return std::filesystem::path(home) / ".config" / "logv" / "config.json";
    }();
    return p;
}

void split_user_host(const std::string& in, std::string& user, std::string& host) {
    auto at = in.find('@');
    if (at == std::string::npos) {
        host = in;
        if (user.empty()) user = "root";
        return;
    }
    user = in.substr(0, at);
    host = in.substr(at + 1);
    if (user.empty()) user = "root";
}

std::string join_user_host(const std::string& user, const std::string& host) {
    if (host.empty()) return {};
    if (user.empty()) return host;
    return user + "@" + host;
}

template <typename T>
T json_get(const Json::Value& v, const char* key, const T& def) {
    if (!v.isObject() || !v.isMember(key)) return def;
    return def;
}

template <>
inline std::string json_get<std::string>(const Json::Value& v, const char* key, const std::string& def) {
    if (!v.isObject() || !v.isMember(key) || !v[key].isString()) return def;
    return v[key].asString();
}

template <>
inline bool json_get<bool>(const Json::Value& v, const char* key, const bool& def) {
    if (!v.isObject() || !v.isMember(key) || !v[key].isBool()) return def;
    return v[key].asBool();
}

template <>
inline int json_get<int>(const Json::Value& v, const char* key, const int& def) {
    if (!v.isObject() || !v.isMember(key) || !v[key].isInt()) return def;
    return v[key].asInt();
}

std::vector<std::string> json_str_array(const Json::Value& v, const char* key) {
    std::vector<std::string> out;
    if (!v.isObject() || !v.isMember(key) || !v[key].isArray()) return out;
    for (const auto& x : v[key]) {
        if (x.isString()) out.push_back(x.asString());
    }
    return out;
}

UiState::AppConfig load_config() {
    UiState::AppConfig cfg;
    const auto& p = config_path();
    if (!std::filesystem::exists(p)) {
        return cfg;
    }

    std::ifstream in(p);
    if (!in) return cfg;

    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string errs;
    if (!Json::parseFromStream(rb, in, &root, &errs)) {
        return cfg;
    }

    auto services = json_str_array(root, "services");
    if (!services.empty()) cfg.services = std::move(services);

    // Migration guard: only restore transient filters from GTK-native generated configs.
    const std::string backend = json_get<std::string>(root, "ui_backend", "");
    if (backend == "gtk_native") {
        cfg.last_service_filter = json_str_array(root, "last_service_filter");
        cfg.level_filter        = json_str_array(root, "level_filter");
        cfg.last_text_filter = json_get<std::string>(root, "last_text_filter", cfg.last_text_filter);
        cfg.newest_at_bottom = json_get<bool>(root, "newest_at_bottom", cfg.newest_at_bottom);
    }
    cfg.auto_scroll = json_get<bool>(root, "auto_scroll", cfg.auto_scroll);

    if (root.isMember("window") && root["window"].isObject()) {
        const auto& w = root["window"];
        cfg.window_width = json_get<int>(w, "width", cfg.window_width);
        cfg.window_height = json_get<int>(w, "height", cfg.window_height);
        cfg.window_maximized = json_get<bool>(w, "maximized", cfg.window_maximized);
    }

    if (root.isMember("connections") && root["connections"].isObject()) {
        const auto& c = root["connections"];
        if (c.isMember("ssh") && c["ssh"].isObject()) {
            const auto& ssh = c["ssh"];
            cfg.ssh_host = json_get<std::string>(ssh, "host", cfg.ssh_host);
            cfg.ssh_username = json_get<std::string>(ssh, "username", cfg.ssh_username);
            cfg.ssh_command = json_get<std::string>(ssh, "command", cfg.ssh_command);
            cfg.ssh_port     = json_get<int>(ssh, "port", cfg.ssh_port);
            cfg.ssh_keepalive = json_get<int>(ssh, "keepalive", cfg.ssh_keepalive);
            cfg.ssh_auto_reconnect = json_get<bool>(ssh, "auto_reconnect", cfg.ssh_auto_reconnect);
            cfg.last_host = json_get<std::string>(ssh, "last_host", cfg.last_host);
            if (cfg.ssh_command == kLegacyRemoteCmd) {
                cfg.ssh_command = kDefaultRemoteCmd;
            }
        }
    }

    cfg.max_lines           = json_get<int>(root, "max_lines", cfg.max_lines);
    cfg.discovered_services = json_str_array(root, "discovered_services");

    return cfg;
}

void save_config(const UiState& s) {
    Json::Value root(Json::objectValue);
    root["ui_backend"] = "gtk_native";

    Json::Value services(Json::arrayValue);
    for (const auto& svc : s.cfg.services) {
        services.append(svc);
    }
    root["services"] = services;

    Json::Value selected(Json::arrayValue);
    for (const auto& kv : s.service_checks) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(kv.second))) {
            selected.append(kv.first);
        }
    }
    root["last_service_filter"] = selected;

    Json::Value lvl_filter(Json::arrayValue);
    for (const auto& lv : s.cfg.level_filter) lvl_filter.append(lv);
    root["level_filter"] = lvl_filter;

    root["last_text_filter"] = gtk_entry_get_text(GTK_ENTRY(s.filter_entry));
    root["auto_scroll"] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s.autoscroll_check));
    root["newest_at_bottom"] = s.newest_at_bottom;

    Json::Value connections(Json::objectValue);
    Json::Value ssh(Json::objectValue);
    std::string host = gtk_entry_get_text(GTK_ENTRY(s.host_entry));
    while (!host.empty() && (host.back() == '\n' || host.back() == '\r' || host.back() == ' ' || host.back() == '\t')) host.pop_back();
    size_t host_i = 0;
    while (host_i < host.size() && (host[host_i] == ' ' || host[host_i] == '\t')) ++host_i;
    host = host.substr(host_i);
    std::string user = s.cfg.ssh_username;
    std::string host_only = s.cfg.ssh_host;
    split_user_host(host, user, host_only);
    ssh["host"] = host_only;
    ssh["username"] = user;
    ssh["command"] = s.cfg.ssh_command;
    ssh["port"] = s.cfg.ssh_port;
    ssh["keepalive"] = s.cfg.ssh_keepalive;
    ssh["auto_reconnect"] = s.cfg.ssh_auto_reconnect;
    ssh["last_host"]      = s.cfg.last_host;
    ssh["password"] = "";
    ssh["key_path"] = "";
    connections["ssh"] = ssh;
    root["connections"] = connections;

    Json::Value window(Json::objectValue);
    int w = 1400;
    int h = 900;
    gtk_window_get_size(GTK_WINDOW(s.window), &w, &h);
    window["width"] = w;
    window["height"] = h;
    window["maximized"] = gtk_window_is_maximized(GTK_WINDOW(s.window));
    root["window"] = window;

    root["max_lines"] = s.cfg.max_lines;
    Json::Value disc_arr(Json::arrayValue);
    for (const auto& d : s.cfg.discovered_services) disc_arr.append(d);
    root["discovered_services"] = disc_arr;

    const auto& p = config_path();
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    std::ofstream out(p);
    if (!out) return;
    out << Json::writeString(wb, root) << std::endl;
}

const char* level_bg(const std::string& lvl) {
    if (lvl == "WARNING") return "#fff3cd";
    if (lvl == "ERROR") return "#f8d7da";
    if (lvl == "CRITICAL") return "#e2b4ff";
    return nullptr;
}

const char* level_fg(const std::string& lvl) {
    if (lvl == "WARNING") return "#856404";
    if (lvl == "ERROR") return "#721c24";
    if (lvl == "CRITICAL") return "#4a235a";
    if (lvl == "DEBUG") return "#555555";
    return "#222222";
}

// Returns timestamp truncated to seconds (strips fractional part) for display.
// The full timestamp string is kept in LogEntry::timestamp for sort key computation.
std::string ts_display(const char* ts) {
    if (!ts) return {};
    const char* dot = std::strchr(ts, '.');
    return dot ? std::string(ts, dot) : std::string(ts);
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(g_ascii_toupper(c));
    if (s == "WARN") return "WARNING";
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string service_bg_hex(const std::string& service) {
    if (service.empty()) return "#ffffff";

    // FNV-1a hash for deterministic per-service color.
    uint32_t h = 2166136261u;
    for (unsigned char c : service) {
        h ^= c;
        h *= 16777619u;
    }

    // Keep colors light/pastel; use more hash bits for better per-service spread.
    const int r = 210 + static_cast<int>((h >>  0) & 0xff);
    const int g = 210 + static_cast<int>((h >>  8) & 0x1f);
    const int b = 210 + static_cast<int>((h >> 16) & 0x1f);

    char buf[8];  // "#rrggbb\0" = 8 bytes
    std::sprintf(buf, "#%02x%02x%02x", r, g, b);
    return std::string(buf);
}

bool service_allowed(UiState* s, const std::string& service) {
    if (s->service_checks.empty()) return true;
    auto it = s->service_checks.find(service);
    // Services not in the known list are silently dropped.
    if (it == s->service_checks.end()) return false;
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(it->second));
}

void update_services_button_label(UiState* s) {
    if (!s->services_btn) return;
    if (s->service_checks.empty()) {
        gtk_button_set_label(GTK_BUTTON(s->services_btn), "Services: All");
        return;
    }
    int total = 0;
    int checked = 0;
    for (const auto& kv : s->service_checks) {
        ++total;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(kv.second))) {
            ++checked;
        }
    }
    char buf[64];
    if (checked == total) {
        std::snprintf(buf, sizeof(buf), "Services: All");
    } else {
        std::snprintf(buf, sizeof(buf), "Services: %d/%d", checked, total);
    }
    gtk_button_set_label(GTK_BUTTON(s->services_btn), buf);
}

void on_service_toggled(GtkToggleButton*, gpointer data);
void on_timestamp_header_clicked(GtkTreeViewColumn* column, gpointer data);

void ensure_service_checkbox(UiState* s, const std::string& service) {
    if (service.empty()) return;
    if (s->service_checks.find(service) != s->service_checks.end()) return;

    GtkWidget* check = gtk_check_button_new_with_label(service.c_str());
    bool checked = true;
    if (!s->cfg.last_service_filter.empty()) {
        checked = std::find(
            s->cfg.last_service_filter.begin(),
            s->cfg.last_service_filter.end(),
            service
        ) != s->cfg.last_service_filter.end();
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), checked ? TRUE : FALSE);
    g_signal_connect(check, "toggled", G_CALLBACK(on_service_toggled), s);
    gtk_box_pack_start(GTK_BOX(s->services_box), check, FALSE, FALSE, 0);
    gtk_widget_show(check);
    s->service_checks.emplace(service, check);
    update_services_button_label(s);
}

void refresh_service_checkboxes(UiState* s) {
    // Destroy all existing checkbox widgets and rebuild from cfg.services.
    GList* children = gtk_container_get_children(GTK_CONTAINER(s->services_box));
    for (GList* l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    s->service_checks.clear();

    for (const auto& svc : s->cfg.services)
        ensure_service_checkbox(s, svc);
    for (const auto& svc : s->cfg.discovered_services)
        ensure_service_checkbox(s, svc);

    // If nothing is checked after restoring, enable all.
    bool any_checked = false;
    for (const auto& kv : s->service_checks) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(kv.second))) {
            any_checked = true; break;
        }
    }
    if (!any_checked)
        for (const auto& kv : s->service_checks)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(kv.second), TRUE);

    update_services_button_label(s);
}

void set_status(UiState* s, const char* msg) {
    gtk_label_set_text(GTK_LABEL(s->status_label), msg);
}

void update_count(UiState* s) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%zu lines", s->entries.size());
    gtk_label_set_text(GTK_LABEL(s->count_label), buf);
}

bool row_matches(const logvcore::LogEntry& e, const std::string& filter, const std::vector<std::string>& level_filter) {
    if (!level_filter.empty()) {
        const std::string lvl = to_upper(e.level);
        bool lvl_ok = false;
        for (const auto& lf : level_filter) {
            if (lf == lvl) { lvl_ok = true; break; }
        }
        if (!lvl_ok) return false;
    }
    if (filter.empty()) return true;
    std::string hay = std::string(e.service) + " " + e.message;
    std::string low_hay = hay;
    for (auto& c : low_hay) c = static_cast<char>(g_ascii_tolower(c));
    return low_hay.find(filter) != std::string::npos;
}

struct VisibleAnchor {
    bool valid{false};
    std::string raw;
    int raw_occurrence{0};
    double top_offset{0.0};
};

VisibleAnchor capture_visible_anchor(UiState* s) {
    VisibleAnchor a;
    if (!s || !s->tree_view || !s->store) return a;

    GdkRectangle rect{};
    gtk_tree_view_get_visible_rect(GTK_TREE_VIEW(s->tree_view), &rect);

        const int probe_x = rect.x + 2;
        const int probe_y = rect.y + 2;

        GtkTreePath* path = nullptr;
    GtkTreeViewColumn* col = nullptr;
    int cell_x = 0;
    int cell_y = 0;
    if (!gtk_tree_view_get_path_at_pos(
            GTK_TREE_VIEW(s->tree_view),
            probe_x,
            probe_y,
            &path,
            &col,
            &cell_x,
            &cell_y)) {
        return a;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(s->store), &iter, path)) {
        gtk_tree_path_free(path);
        return a;
    }

    gchar* raw = nullptr;
    gtk_tree_model_get(
        GTK_TREE_MODEL(s->store),
        &iter,
        COL_RAW, &raw,
        -1
    );

    a.raw = raw ? raw : "";

    // Disambiguate duplicates by counting equal-raw rows up to anchor row.
    GtkTreeIter it2;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s->store), &it2);
    int occurrence = 0;
    while (ok) {
        gchar* row_raw = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(s->store), &it2, COL_RAW, &row_raw, -1);
        const bool same = a.raw == (row_raw ? row_raw : "");
        if (row_raw) g_free(row_raw);
        if (same) {
            ++occurrence;
        }
        GtkTreePath* p2 = gtk_tree_model_get_path(GTK_TREE_MODEL(s->store), &it2);
        const bool reached = (p2 != nullptr) ? (gtk_tree_path_compare(p2, path) == 0) : false;
        if (p2) gtk_tree_path_free(p2);
        if (reached) break;
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(s->store), &it2);
    }

    GdkRectangle row_rect{};
    gtk_tree_view_get_background_area(GTK_TREE_VIEW(s->tree_view), path, nullptr, &row_rect);
    a.top_offset = static_cast<double>(probe_y - row_rect.y);
    if (a.top_offset < 0.0) a.top_offset = 0.0;

    a.raw_occurrence = occurrence;
    a.valid = !a.raw.empty();

    if (raw) g_free(raw);
    gtk_tree_path_free(path);
    return a;
}

void restore_visible_anchor(UiState* s, const VisibleAnchor& a) {
    if (!s || !a.valid || !s->tree_view || !s->store) return;

    GtkTreeIter iter;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s->store), &iter);
    int seen = 0;
    while (ok) {
        gchar* raw = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(s->store), &iter, COL_RAW, &raw, -1);
        const bool same = a.raw == (raw ? raw : "");
        if (raw) g_free(raw);
        if (same) {
            ++seen;
        }

        if (same && seen == a.raw_occurrence) {
            GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(s->store), &iter);
            if (path) {
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(s->tree_view), path, nullptr, TRUE, 0.0f, 0.0f);
                GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scroll));
                if (adj) {
                    double v = gtk_adjustment_get_value(adj) + a.top_offset;
                    const double lower = gtk_adjustment_get_lower(adj);
                    const double upper = gtk_adjustment_get_upper(adj);
                    const double page = gtk_adjustment_get_page_size(adj);
                    double max_v = upper - page;
                    if (max_v < lower) max_v = lower;
                    if (v < lower) v = lower;
                    if (v > max_v) v = max_v;
                    gtk_adjustment_set_value(adj, v);
                }
                gtk_tree_path_free(path);
            }
            return;
        }

        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(s->store), &iter);
    }
}

int month_index(const char* mon3) {
    if (std::strcmp(mon3, "Jan") == 0) return 1;
    if (std::strcmp(mon3, "Feb") == 0) return 2;
    if (std::strcmp(mon3, "Mar") == 0) return 3;
    if (std::strcmp(mon3, "Apr") == 0) return 4;
    if (std::strcmp(mon3, "May") == 0) return 5;
    if (std::strcmp(mon3, "Jun") == 0) return 6;
    if (std::strcmp(mon3, "Jul") == 0) return 7;
    if (std::strcmp(mon3, "Aug") == 0) return 8;
    if (std::strcmp(mon3, "Sep") == 0) return 9;
    if (std::strcmp(mon3, "Oct") == 0) return 10;
    if (std::strcmp(mon3, "Nov") == 0) return 11;
    if (std::strcmp(mon3, "Dec") == 0) return 12;
    return 0;
}

uint64_t timestamp_order_key(const logvcore::LogEntry& e) {
    char mon[3]{};   // 3-char month abbreviation — "Jan".."Dec"
    int day = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    int usec = 0;

    int n = std::sscanf(e.timestamp, "%3s %d %d:%d:%d.%d", mon, &day, &hh, &mm, &ss, &usec);
    if (n < 5) {
        n = std::sscanf(e.timestamp, "%3s %d %d:%d:%d", mon, &day, &hh, &mm, &ss);
        usec = 0;
    }
    if (n < 5) {
        return std::numeric_limits<uint64_t>::max();
    }

    const int month = month_index(mon);
    if (month <= 0) {
        return std::numeric_limits<uint64_t>::max();
    }

    if (usec < 0) usec = 0;
    if (usec > 999999) usec = usec % 1000000;

    // Monotonic tuple key: month/day/time/usec. Year is omitted by source format.
    return (((((static_cast<uint64_t>(month) * 32ULL + static_cast<uint64_t>(day)) * 24ULL + static_cast<uint64_t>(hh)) * 60ULL + static_cast<uint64_t>(mm)) * 60ULL + static_cast<uint64_t>(ss)) * 1000000ULL) + static_cast<uint64_t>(usec);
}

void append_row(UiState* s, const logvcore::LogEntry& e) {
    GtkTreeIter iter;
    gtk_list_store_append(s->store, &iter);
    const std::string lvl = to_upper(e.level);
    const char* lvl_bg = level_bg(lvl);
    const std::string svc_bg = service_bg_hex(e.service);
    const char* bg = lvl_bg ? lvl_bg : svc_bg.c_str();
    std::string msg = e.message;
    for (char& c : msg) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    gtk_list_store_set(
        s->store,
        &iter,
        COL_TS, ts_display(e.timestamp).c_str(),
        COL_SVC, e.service,
        COL_LVL, lvl.c_str(),
        COL_MSG, msg.c_str(),
        COL_BG, bg,
        COL_FG, level_fg(lvl),
        COL_RAW, e.raw.c_str(),
        -1
    );
}

void rebuild_store(UiState* s) {
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scroll));
    const bool auto_scroll = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->autoscroll_check));
    const double old_val = adj ? gtk_adjustment_get_value(adj) : 0.0;
    const VisibleAnchor anchor = auto_scroll ? VisibleAnchor{} : capture_visible_anchor(s);

    gtk_list_store_clear(s->store);

    const char* ftxt = gtk_entry_get_text(GTK_ENTRY(s->filter_entry));
    std::string filter = ftxt ? ftxt : "";
    for (auto& c : filter) c = static_cast<char>(g_ascii_tolower(c));

    std::vector<std::pair<uint64_t, const logvcore::LogEntry*>> rows;
    rows.reserve(s->entries.size());

    for (const auto& e : s->entries) {
        if (service_allowed(s, e.service) && row_matches(e, filter, s->cfg.level_filter)) {
            rows.emplace_back(timestamp_order_key(e), &e);
        }
    }

    std::stable_sort(
        rows.begin(),
        rows.end(),
        [s](const auto& a, const auto& b) {
            const uint64_t unknown = std::numeric_limits<uint64_t>::max();
            const bool a_unknown = (a.first == unknown);
            const bool b_unknown = (b.first == unknown);
            if (a_unknown != b_unknown) {
                return !a_unknown; // Keep unparsable timestamps at the end.
            }
            if (s->newest_at_bottom) {
                return a.first < b.first;
            }
            return a.first > b.first;
        }
    );

    for (const auto& row : rows) {
        append_row(s, *row.second);
    }

    if (adj != nullptr) {
        const double lower = gtk_adjustment_get_lower(adj);
        const double page = gtk_adjustment_get_page_size(adj);
        const double upper = gtk_adjustment_get_upper(adj);
        double max_val = upper - page;
        if (max_val < lower) max_val = lower;

        if (auto_scroll) {
            gtk_adjustment_set_value(adj, max_val);
        } else {
            double keep = old_val;
            if (keep < lower) keep = lower;
            if (keep > max_val) keep = max_val;
            gtk_adjustment_set_value(adj, keep);
            restore_visible_anchor(s, anchor);
        }
    }

    update_count(s);
}

// Insert a single entry directly into the live store without clearing it.
// position == -1 means append at tail; position == 0 means prepend at head.
void insert_row_live(UiState* s, const logvcore::LogEntry& e, bool at_end) {
    GtkTreeIter iter;
    if (at_end) {
        gtk_list_store_append(s->store, &iter);
    } else {
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s->store), &first)) {
            gtk_list_store_insert_before(s->store, &iter, &first);
        } else {
            gtk_list_store_append(s->store, &iter);
        }
    }

    const std::string lvl = to_upper(e.level);
    const char* lvl_bg = level_bg(lvl);
    const std::string svc_bg = service_bg_hex(e.service);
    const char* bg = lvl_bg ? lvl_bg : svc_bg.c_str();
    std::string msg = e.message;
    for (char& c : msg) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    gtk_list_store_set(
        s->store, &iter,
        COL_TS, ts_display(e.timestamp).c_str(),
        COL_SVC, e.service,
        COL_LVL, lvl.c_str(),
        COL_MSG, msg.c_str(),
        COL_BG, bg,
        COL_FG, level_fg(lvl),
        COL_RAW, e.raw.c_str(),
        -1
    );
}

gboolean drain_pending(gpointer data) {
    auto* s = static_cast<UiState*>(data);

    std::deque<logvcore::LogEntry> local;
    {
        std::lock_guard<std::mutex> lk(s->pending_mu);
        local.swap(s->pending);
    }

    if (local.empty()) return G_SOURCE_CONTINUE;

    const char* ftxt = gtk_entry_get_text(GTK_ENTRY(s->filter_entry));
    std::string filter = ftxt ? ftxt : "";
    for (auto& c : filter) c = static_cast<char>(g_ascii_tolower(c));

    const bool auto_scroll = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->autoscroll_check));

    for (auto& e : local) {
        // Evict oldest visible row if at cap.
        if (s->cfg.max_lines > 0 && s->entries.size() >= static_cast<size_t>(s->cfg.max_lines)) {
            s->entries.erase(s->entries.begin());
            // Remove the corresponding row from the store (first or last depending on sort).
            GtkTreeIter victim;
            if (s->newest_at_bottom) {
                gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s->store), &victim);
            } else {
                gint n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(s->store), nullptr);
                GtkTreePath* lp = gtk_tree_path_new_from_indices(n - 1, -1);
                gtk_tree_model_get_iter(GTK_TREE_MODEL(s->store), &victim, lp);
                gtk_tree_path_free(lp);
            }
            gtk_list_store_remove(s->store, &victim);
        }

        // Auto-discover services not yet in any known list.
        if (e.service[0] != '\0') {
            const std::string svc_str(e.service);
            if (s->service_checks.find(svc_str) == s->service_checks.end()) {
                s->cfg.discovered_services.push_back(svc_str);
                ensure_service_checkbox(s, svc_str);
            }
        }

        s->entries.push_back(e);

        if (service_allowed(s, e.service) && row_matches(e, filter, s->cfg.level_filter)) {
            insert_row_live(s, e, s->newest_at_bottom);
        }
    }

    update_count(s);

    if (auto_scroll) {
        GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s->scroll));
        if (adj) {
            if (s->newest_at_bottom) {
                // Ascending: newest appended to bottom — scroll to end.
                const double upper = gtk_adjustment_get_upper(adj);
                const double page  = gtk_adjustment_get_page_size(adj);
                gtk_adjustment_set_value(adj, upper - page);
            } else {
                // Descending: newest prepended to top — scroll to beginning.
                gtk_adjustment_set_value(adj, gtk_adjustment_get_lower(adj));
            }
        }
    }
    // When auto-scroll is off, do nothing — scroll position is untouched.

    return G_SOURCE_CONTINUE;
}

void on_service_toggled(GtkToggleButton*, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    update_services_button_label(s);
    rebuild_store(s);
}

void on_timestamp_header_clicked(GtkTreeViewColumn* column, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    s->newest_at_bottom = !s->newest_at_bottom;
    gtk_tree_view_column_set_sort_indicator(column, TRUE);
    gtk_tree_view_column_set_sort_order(
        column,
        s->newest_at_bottom ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING
    );
    rebuild_store(s);
}

bool start_reader_process(UiState* s, const std::string& host, const std::string& remote_cmd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        // Build SSH command with configured keepalive and port.
        std::string ssh_opts = "ssh -o BatchMode=yes";
        if (s->cfg.ssh_keepalive > 0) {
            ssh_opts += " -o ServerAliveInterval=" + std::to_string(s->cfg.ssh_keepalive);
            ssh_opts += " -o ServerAliveCountMax=3";
        }
        if (s->cfg.ssh_port != 22) {
            ssh_opts += " -p " + std::to_string(s->cfg.ssh_port);
        }
        std::string shell_cmd = ssh_opts + " " + host + " \"" + remote_cmd + "\"";
        execl("/bin/sh", "sh", "-c", shell_cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(pipefd[1]);
    s->child_pid = pid;
    s->child_fd = pipefd[0];
    return true;
}

void stop_reader(UiState* s) {
    s->running = false;

    if (s->reconnect_timer_id) {
        g_source_remove(s->reconnect_timer_id);
        s->reconnect_timer_id = 0;
    }

    if (s->child_pid > 0) {
        kill(s->child_pid, SIGTERM);
    }

    if (s->reader_thread.joinable()) {
        s->reader_thread.join();
    }

    if (s->child_fd >= 0) {
        close(s->child_fd);
        s->child_fd = -1;
    }

    if (s->child_pid > 0) {
        int status = 0;
        waitpid(s->child_pid, &status, 0);
        s->child_pid = -1;
    }
}

void try_reconnect(UiState* s);

void reader_loop(UiState* s) {
    FILE* fp = fdopen(s->child_fd, "r");
    if (!fp) {
        s->child_fd = -1;
        if (s->cfg.ssh_auto_reconnect && s->running.load()) {
            g_idle_add([](gpointer d) -> gboolean {
                auto* st = static_cast<UiState*>(d);
                set_status(st, "Connection lost — reconnecting...");
                return G_SOURCE_REMOVE;
            }, s);
            // Schedule reconnect on the GTK main thread after 5 s.
            struct ReconArg { UiState* s; };
            auto* arg = new ReconArg{s};
            s->reconnect_timer_id = g_timeout_add_full(
                G_PRIORITY_DEFAULT, 5000,
                [](gpointer d) -> gboolean {
                    auto* a = static_cast<ReconArg*>(d);
                    try_reconnect(a->s);
                    return G_SOURCE_REMOVE;
                },
                arg,
                [](gpointer d) { delete static_cast<ReconArg*>(d); }
            );
        } else {
            g_idle_add([](gpointer d) -> gboolean {
                auto* st = static_cast<UiState*>(d);
                st->running = false;
                gtk_button_set_label(GTK_BUTTON(st->connect_btn), "Connect");
                set_status(st, "Disconnected");
                return G_SOURCE_REMOVE;
            }, s);
        }
        return;
    }

    char* line = nullptr;
    size_t n = 0;

    while (s->running.load()) {
        ssize_t r = getline(&line, &n, fp);
        if (r <= 0) break;

        std::string raw(line, static_cast<size_t>(r));
        auto entry = logvcore::parse_line(raw);

        if (entry.message.empty() && !entry.valid()) {
            continue;
        }

        std::lock_guard<std::mutex> lk(s->pending_mu);
        s->pending.push_back(std::move(entry));
    }

    if (line) free(line);
    fclose(fp);
    s->child_fd = -1;

    // Connection dropped — schedule reconnect on GTK thread.
    if (s->cfg.ssh_auto_reconnect && s->running.load()) {
        g_idle_add([](gpointer d) -> gboolean {
            auto* st = static_cast<UiState*>(d);
            set_status(st, "Connection lost — reconnecting...");
            return G_SOURCE_REMOVE;
        }, s);
        struct ReconArg { UiState* s; };
        auto* arg = new ReconArg{s};
        s->reconnect_timer_id = g_timeout_add_full(
            G_PRIORITY_DEFAULT, 5000,
            [](gpointer d) -> gboolean {
                auto* a = static_cast<ReconArg*>(d);
                a->s->reconnect_timer_id = 0;
                try_reconnect(a->s);
                return G_SOURCE_REMOVE;
            },
            arg,
            [](gpointer d) { delete static_cast<ReconArg*>(d); }
        );
    } else {
        g_idle_add([](gpointer d) -> gboolean {
            auto* st = static_cast<UiState*>(d);
            if (!st->cfg.ssh_auto_reconnect) {
                st->running = false;
                gtk_button_set_label(GTK_BUTTON(st->connect_btn), "Connect");
                set_status(st, "Disconnected");
            }
            return G_SOURCE_REMOVE;
        }, s);
    }
}

void try_reconnect(UiState* s) {
    if (!s->running.load()) return;

    // Clean up previous child.
    if (s->child_pid > 0) {
        int status = 0;
        waitpid(s->child_pid, &status, WNOHANG);
        s->child_pid = -1;
    }
    if (s->reader_thread.joinable()) s->reader_thread.join();

    std::string host = join_user_host(s->cfg.ssh_username, s->cfg.ssh_host);
    std::string cmd = s->cfg.ssh_command.empty() ? std::string(kDefaultRemoteCmd) : s->cfg.ssh_command;

    if (!start_reader_process(s, host, cmd)) {
        set_status(s, "Reconnect failed — retrying in 5s...");
        struct ReconArg { UiState* s; };
        auto* arg = new ReconArg{s};
        s->reconnect_timer_id = g_timeout_add_full(
            G_PRIORITY_DEFAULT, 5000,
            [](gpointer d) -> gboolean {
                auto* a = static_cast<ReconArg*>(d);
                a->s->reconnect_timer_id = 0;
                try_reconnect(a->s);
                return G_SOURCE_REMOVE;
            },
            arg,
            [](gpointer d) { delete static_cast<ReconArg*>(d); }
        );
        return;
    }

    s->reader_thread = std::thread(reader_loop, s);
    set_status(s, "Reconnected (streaming)");
}

void on_filter_changed(GtkEditable*, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    rebuild_store(s);
}

void on_clear_clicked(GtkButton*, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    s->entries.clear();
    {
        std::lock_guard<std::mutex> lk(s->pending_mu);
        s->pending.clear();
    }
    gtk_list_store_clear(s->store);
    update_count(s);
}

void show_settings_dialog(UiState* s) {
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        "Settings",
        GTK_WINDOW(s->window),
        GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT,
        nullptr
    );
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, 480);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget* nb = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(content), nb, TRUE, TRUE, 0);

    // --- SSH tab ---
    GtkWidget* ssh_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(ssh_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(ssh_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(ssh_grid), 10);

    auto add_row = [&](GtkWidget* grid, int row, const char* label, GtkWidget* widget) {
        GtkWidget* lbl = gtk_label_new(label);
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
    };

    GtkWidget* e_host = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e_host), s->cfg.ssh_host.c_str());
    add_row(ssh_grid, 0, "Host:", e_host);

    GtkWidget* e_user = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e_user), s->cfg.ssh_username.c_str());
    add_row(ssh_grid, 1, "Username:", e_user);

    GtkWidget* e_port = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e_port), s->cfg.ssh_port);
    add_row(ssh_grid, 2, "Port:", e_port);

    GtkWidget* e_keepalive = gtk_spin_button_new_with_range(0, 300, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e_keepalive), s->cfg.ssh_keepalive);
    add_row(ssh_grid, 3, "Keep-alive (s, 0=off):", e_keepalive);

    GtkWidget* chk_reconnect = gtk_check_button_new_with_label("Auto-reconnect on disconnect");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_reconnect), s->cfg.ssh_auto_reconnect);
    gtk_grid_attach(GTK_GRID(ssh_grid), chk_reconnect, 0, 4, 2, 1);

    GtkWidget* e_cmd = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e_cmd), s->cfg.ssh_command.c_str());
    add_row(ssh_grid, 5, "Remote command:", e_cmd);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), ssh_grid, gtk_label_new("SSH"));

    // --- Services tab ---
    GtkWidget* svc_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(svc_outer), 10);
    GtkWidget* svc_lbl = gtk_label_new("One service name per line:");
    gtk_widget_set_halign(svc_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(svc_outer), svc_lbl, FALSE, FALSE, 0);

    GtkWidget* svc_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(svc_scroll, -1, 200);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(svc_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget* svc_text = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(svc_text), TRUE);
    GtkTextBuffer* svc_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(svc_text));
    std::string svc_content;
    for (const auto& svc : s->cfg.services) svc_content += svc + "\n";
    gtk_text_buffer_set_text(svc_buf, svc_content.c_str(), -1);
    gtk_container_add(GTK_CONTAINER(svc_scroll), svc_text);
    gtk_box_pack_start(GTK_BOX(svc_outer), svc_scroll, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), svc_outer, gtk_label_new("Services"));

    // --- Log levels tab ---
    GtkWidget* lvl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(lvl_box), 10);
    GtkWidget* lvl_lbl = gtk_label_new("Show only these levels (uncheck = show all):");
    gtk_widget_set_halign(lvl_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(lvl_box), lvl_lbl, FALSE, FALSE, 0);

    const char* all_levels[] = {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL", nullptr};
    std::map<std::string, GtkWidget*> lvl_checks;
    for (int i = 0; all_levels[i]; ++i) {
        GtkWidget* chk = gtk_check_button_new_with_label(all_levels[i]);
        const bool on = s->cfg.level_filter.empty() ||
            std::find(s->cfg.level_filter.begin(), s->cfg.level_filter.end(),
                      std::string(all_levels[i])) != s->cfg.level_filter.end();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), on);
        gtk_box_pack_start(GTK_BOX(lvl_box), chk, FALSE, FALSE, 0);
        lvl_checks[all_levels[i]] = chk;
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), lvl_box, gtk_label_new("Log Levels"));

    // --- General tab ---
    GtkWidget* gen_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(gen_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(gen_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(gen_grid), 10);

    GtkWidget* e_maxlines = gtk_spin_button_new_with_range(0, 1000000, 1000);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e_maxlines), s->cfg.max_lines);
    add_row(gen_grid, 0, "Max log lines (0 = unlimited):", e_maxlines);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gen_grid, gtk_label_new("General"));

    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        // SSH
        s->cfg.ssh_host = gtk_entry_get_text(GTK_ENTRY(e_host));
        s->cfg.ssh_username = gtk_entry_get_text(GTK_ENTRY(e_user));
        s->cfg.ssh_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e_port));
        s->cfg.ssh_keepalive = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e_keepalive));
        s->cfg.ssh_auto_reconnect = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_reconnect));
        s->cfg.ssh_command = gtk_entry_get_text(GTK_ENTRY(e_cmd));

        // Update host entry in main bar.
        gtk_entry_set_text(GTK_ENTRY(s->host_entry),
            join_user_host(s->cfg.ssh_username, s->cfg.ssh_host).c_str());

        // Services.
        GtkTextIter start_it, end_it;
        gtk_text_buffer_get_bounds(svc_buf, &start_it, &end_it);
        gchar* raw_svcs = gtk_text_buffer_get_text(svc_buf, &start_it, &end_it, FALSE);
        s->cfg.services.clear();
        if (raw_svcs) {
            std::string line;
            for (const char* p = raw_svcs; ; ++p) {
                if (*p == '\n' || *p == '\0') {
                    // Trim whitespace.
                    size_t a = 0, b = line.size();
                    while (a < b && (line[a] == ' ' || line[a] == '\t')) ++a;
                    while (b > a && (line[b-1] == ' ' || line[b-1] == '\t' || line[b-1] == '\r')) --b;
                    if (b > a) s->cfg.services.push_back(line.substr(a, b - a));
                    line.clear();
                    if (*p == '\0') break;
                } else {
                    line += *p;
                }
            }
            g_free(raw_svcs);
        }

        // Log levels: empty level_filter = show all.
        s->cfg.level_filter.clear();
        bool all_on = true;
        for (const auto& kv : lvl_checks) {
            if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(kv.second))) {
                all_on = false;
                break;
            }
        }
        if (!all_on) {
            for (const auto& kv : lvl_checks) {
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(kv.second))) {
                    s->cfg.level_filter.push_back(kv.first);
                }
            }
        }

        // General.
        s->cfg.max_lines = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e_maxlines));

        save_config(*s);
        refresh_service_checkboxes(s);
        rebuild_store(s);
    }

    gtk_widget_destroy(dlg);
}

// --- Service status diagnostic check ---

struct SvcCheckArg {
    UiState* s;
    GtkWidget* check_btn;
    std::string output;
};

gpointer run_service_check(gpointer data) {
    auto* arg = static_cast<SvcCheckArg*>(data);
    UiState* s = arg->s;

    // Build inner shell command: one printf per service.
    std::string inner = "for svc in";
    for (const auto& svc : s->cfg.services)
        inner += " " + svc;
    inner += "; do printf \"%-40s %s\\n\" \"$svc\" \"$(systemctl is-active \"$svc\" 2>/dev/null)\"; done";

    std::string ssh_opts = "ssh -o BatchMode=yes -o ConnectTimeout=5";
    if (s->cfg.ssh_keepalive > 0)
        ssh_opts += " -o ServerAliveInterval=" + std::to_string(s->cfg.ssh_keepalive)
                  + " -o ServerAliveCountMax=1";
    if (s->cfg.ssh_port != 22)
        ssh_opts += " -p " + std::to_string(s->cfg.ssh_port);
    std::string host = join_user_host(s->cfg.ssh_username, s->cfg.ssh_host);
    std::string cmd = ssh_opts + " " + host + " '" + inner + "' 2>&1";

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        arg->output = "Failed to launch SSH process.";
    } else {
        char buf[256];
        while (fgets(buf, sizeof(buf), fp))
            arg->output += buf;
        pclose(fp);
        if (arg->output.empty())
            arg->output = "(no output — SSH may have failed or host unreachable)";
    }

    g_idle_add([](gpointer d) -> gboolean {
        auto* a = static_cast<SvcCheckArg*>(d);
        gtk_widget_set_sensitive(a->check_btn, TRUE);

        GtkWidget* dlg = gtk_dialog_new_with_buttons(
            "Service Status",
            GTK_WINDOW(a->s->window),
            GtkDialogFlags(GTK_DIALOG_DESTROY_WITH_PARENT),
            "_Close", GTK_RESPONSE_CLOSE,
            nullptr
        );
        gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 360);
        GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
        gtk_container_set_border_width(GTK_CONTAINER(content), 12);

        GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        GtkWidget* lbl = gtk_label_new(a->output.c_str());
        gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_yalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_margin_start(lbl, 4);
        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, "label { font-family: monospace; font-size: 10pt; }", -1, nullptr);
        gtk_style_context_add_provider(gtk_widget_get_style_context(lbl),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        gtk_container_add(GTK_CONTAINER(sw), lbl);
        gtk_box_pack_start(GTK_BOX(content), sw, TRUE, TRUE, 0);

        gtk_widget_show_all(dlg);
        g_signal_connect(dlg, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
        delete a;
        return G_SOURCE_REMOVE;
    }, arg);

    return nullptr;
}

void on_check_services_clicked(GtkButton* btn, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    auto* arg = new SvcCheckArg{s, GTK_WIDGET(btn), {}};
    GThread* t = g_thread_new("svc-check", run_service_check, arg);
    g_thread_unref(t);
}

void on_settings_clicked(GtkButton*, gpointer data) {
    show_settings_dialog(static_cast<UiState*>(data));
}

gboolean on_tree_tooltip(GtkWidget* widget, gint x, gint y, gboolean keyboard_mode,
                         GtkTooltip* tooltip, gpointer /*data*/) {
    GtkTreeView* tv = GTK_TREE_VIEW(widget);
    GtkTreeModel* model;
    GtkTreePath* path;
    GtkTreeIter iter;

    if (!gtk_tree_view_get_tooltip_context(tv, &x, &y, keyboard_mode, &model, &path, &iter))
        return FALSE;

    gchar* msg = nullptr;
    gtk_tree_model_get(model, &iter, COL_MSG, &msg, -1);
    gtk_tree_path_free(path);
    if (!msg) return FALSE;

    GtkWidget* lbl = gtk_label_new(msg);
    g_free(msg);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 120);
    gtk_label_set_selectable(GTK_LABEL(lbl), FALSE);
    gtk_widget_set_margin_start(lbl, 6);
    gtk_widget_set_margin_end(lbl, 6);
    gtk_widget_set_margin_top(lbl, 4);
    gtk_widget_set_margin_bottom(lbl, 4);
    gtk_widget_show(lbl);
    gtk_tooltip_set_custom(tooltip, lbl);
    return TRUE;
}

// Build a tab-separated line from one row's columns.
std::string row_to_text(GtkTreeModel* model, GtkTreeIter* iter) {
    gchar* ts  = nullptr;
    gchar* svc = nullptr;
    gchar* lvl = nullptr;
    gchar* msg = nullptr;
    gtk_tree_model_get(model, iter,
        COL_TS,  &ts,
        COL_SVC, &svc,
        COL_LVL, &lvl,
        COL_MSG, &msg,
        -1);
    std::string line;
    if (ts)  { line += ts;  g_free(ts);  }
    line += '\t';
    if (svc) { line += svc; g_free(svc); }
    line += '\t';
    if (lvl) { line += lvl; g_free(lvl); }
    line += '\t';
    if (msg) { line += msg; g_free(msg); }
    return line;
}

void copy_selected_rows(UiState* s) {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s->tree_view));
    GtkTreeModel* model;
    GList* rows = gtk_tree_selection_get_selected_rows(sel, &model);
    if (!rows) return;

    std::string text;
    for (GList* l = rows; l; l = l->next) {
        GtkTreePath* path = static_cast<GtkTreePath*>(l->data);
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            if (!text.empty()) text += '\n';
            text += row_to_text(model, &iter);
        }
    }
    g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));

    if (text.empty()) return;
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, text.c_str(), static_cast<gint>(text.size()));
}

gboolean on_tree_button_press(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    if (event->type != GDK_BUTTON_PRESS || event->button != 3)
        return FALSE;

    // Ensure the row under the cursor is selected if it wasn't already part of
    // a multi-selection, so right-clicking a single unselected row works.
    GtkTreePath* path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
            static_cast<gint>(event->x), static_cast<gint>(event->y),
            &path, nullptr, nullptr, nullptr)) {
        GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
        }
        gtk_tree_path_free(path);
    }

    GtkWidget* menu = gtk_menu_new();
    GtkWidget* item = gtk_menu_item_new_with_label("Copy");
    g_signal_connect_swapped(item, "activate", G_CALLBACK(copy_selected_rows), s);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(event));
    return TRUE;
}

void on_connect_clicked(GtkButton*, gpointer data) {
    auto* s = static_cast<UiState*>(data);

    if (s->running.load()) {
        stop_reader(s);
        gtk_button_set_label(GTK_BUTTON(s->connect_btn), "Connect");
        set_status(s, "Disconnected");
        return;
    }

    std::string host = gtk_entry_get_text(GTK_ENTRY(s->host_entry));
    host = trim(host);

    if (host.empty()) {
        set_status(s, "Host is empty");
        return;
    }

    // Parse and persist host/user into cfg so auto-reconnect uses them.
    split_user_host(host, s->cfg.ssh_username, s->cfg.ssh_host);

    // Clear per-host discovered services when connecting to a different host.
    if (!s->cfg.last_host.empty() && s->cfg.ssh_host != s->cfg.last_host) {
        s->cfg.discovered_services.clear();
        refresh_service_checkboxes(s);
    }
    s->cfg.last_host = s->cfg.ssh_host;

    std::string cmd = s->cfg.ssh_command.empty() ? std::string(kDefaultRemoteCmd) : s->cfg.ssh_command;

    if (!start_reader_process(s, host, cmd)) {
        set_status(s, "Failed to start SSH reader");
        return;
    }

    s->running = true;
    s->reader_thread = std::thread(reader_loop, s);

    gtk_button_set_label(GTK_BUTTON(s->connect_btn), "Disconnect");
    set_status(s, "Connected (streaming)");
}

void on_destroy(GtkWidget*, gpointer data) {
    auto* s = static_cast<UiState*>(data);
    save_config(*s);
    stop_reader(s);
    gtk_main_quit();
}

GtkTreeViewColumn* make_text_column(const char* title, int col_idx) {
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    g_object_set(
        G_OBJECT(renderer),
        "single-paragraph-mode", TRUE,
        "ypad", 2,
        nullptr
    );
    GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(
        title,
        renderer,
        "text", col_idx,
        "background", COL_BG,
        "foreground", COL_FG,
        nullptr
    );
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_expand(col, col_idx == COL_MSG);
    gtk_tree_view_column_set_clickable(col, FALSE);
    gtk_tree_view_column_set_sort_indicator(col, FALSE);
    return col;
}

}  // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);

    UiState state;
    state.cfg = load_config();
    state.newest_at_bottom = state.cfg.newest_at_bottom;

    state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state.window), "logv — GTK Native Log Viewer");
    gtk_window_set_default_size(GTK_WINDOW(state.window), state.cfg.window_width, state.cfg.window_height);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(root), 8);
    gtk_container_add(GTK_CONTAINER(state.window), root);

    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    GtkWidget* host_lbl = gtk_label_new("Host:");
    gtk_box_pack_start(GTK_BOX(top), host_lbl, FALSE, FALSE, 0);

    state.host_entry = gtk_entry_new();
    const std::string host_text = join_user_host(state.cfg.ssh_username, state.cfg.ssh_host);
    gtk_entry_set_text(GTK_ENTRY(state.host_entry), host_text.c_str());
    gtk_widget_set_size_request(state.host_entry, 280, -1);
    gtk_box_pack_start(GTK_BOX(top), state.host_entry, FALSE, FALSE, 0);

    state.connect_btn = gtk_button_new_with_label("Connect");
    gtk_box_pack_start(GTK_BOX(top), state.connect_btn, FALSE, FALSE, 0);

    state.clear_btn = gtk_button_new_with_label("Clear");
    gtk_box_pack_start(GTK_BOX(top), state.clear_btn, FALSE, FALSE, 0);

    GtkWidget* settings_btn = gtk_button_new_with_label("Settings");
    gtk_box_pack_start(GTK_BOX(top), settings_btn, FALSE, FALSE, 0);

    state.autoscroll_check = gtk_check_button_new_with_label("Auto-scroll");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state.autoscroll_check), state.cfg.auto_scroll ? TRUE : FALSE);
    gtk_box_pack_start(GTK_BOX(top), state.autoscroll_check, FALSE, FALSE, 0);

    GtkWidget* filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(root), filter_bar, FALSE, FALSE, 0);

    state.services_btn = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(state.services_btn), "Services: All");
    gtk_box_pack_start(GTK_BOX(filter_bar), state.services_btn, FALSE, FALSE, 0);

    GtkWidget* check_svc_btn = gtk_button_new_with_label("Check Active");
    gtk_box_pack_start(GTK_BOX(filter_bar), check_svc_btn, FALSE, FALSE, 0);

    state.services_popover = gtk_popover_new(state.services_btn);
    GtkWidget* services_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(services_scroll, 280, 300);
    gtk_container_add(GTK_CONTAINER(state.services_popover), services_scroll);

    state.services_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(state.services_box), 8);
    gtk_container_add(GTK_CONTAINER(services_scroll), state.services_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(state.services_btn), state.services_popover);
    // GtkPopover is a separate toplevel -- show_all(window) does not traverse into it.
    // Explicitly show the inner widgets so they are visible when the popover opens.
    gtk_widget_show(state.services_box);
    gtk_widget_show(services_scroll);

    GtkWidget* filter_lbl = gtk_label_new("Text:");
    gtk_box_pack_start(GTK_BOX(filter_bar), filter_lbl, FALSE, FALSE, 0);

    state.filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state.filter_entry), "Filter by service/message");
    gtk_entry_set_text(GTK_ENTRY(state.filter_entry), state.cfg.last_text_filter.c_str());
    gtk_box_pack_start(GTK_BOX(filter_bar), state.filter_entry, TRUE, TRUE, 0);

    for (const auto& svc : state.cfg.services) {
        ensure_service_checkbox(&state, svc);
    }

    // If saved filters don't match current known services, fall back to showing all services.
    bool any_checked = false;
    for (const auto& kv : state.service_checks) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(kv.second))) {
            any_checked = true;
            break;
        }
    }
    if (!any_checked) {
        for (const auto& kv : state.service_checks) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(kv.second), TRUE);
        }
    }

    state.store = gtk_list_store_new(
        COL_COUNT,
        G_TYPE_STRING,
        G_TYPE_STRING,
        G_TYPE_STRING,
        G_TYPE_STRING,
        G_TYPE_STRING,
        G_TYPE_STRING,
        G_TYPE_STRING
    );
    gtk_tree_sortable_set_sort_column_id(
        GTK_TREE_SORTABLE(state.store),
        GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
        GTK_SORT_ASCENDING
    );

    state.tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state.store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(state.tree_view), TRUE);

    state.ts_column = make_text_column("Timestamp", COL_TS);
    gtk_tree_view_column_set_clickable(state.ts_column, TRUE);
    gtk_tree_view_column_set_sort_indicator(state.ts_column, TRUE);
    gtk_tree_view_column_set_sort_order(state.ts_column,
        state.newest_at_bottom ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
    g_signal_connect(state.ts_column, "clicked", G_CALLBACK(on_timestamp_header_clicked), &state);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state.tree_view), state.ts_column);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state.tree_view), make_text_column("Service", COL_SVC));
    gtk_tree_view_append_column(GTK_TREE_VIEW(state.tree_view), make_text_column("Level", COL_LVL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(state.tree_view), make_text_column("Message", COL_MSG));

    gtk_widget_set_has_tooltip(state.tree_view, TRUE);
    g_signal_connect(state.tree_view, "query-tooltip", G_CALLBACK(on_tree_tooltip), nullptr);
    g_signal_connect(state.tree_view, "button-press-event", G_CALLBACK(on_tree_button_press), &state);
    g_signal_connect(state.tree_view, "key-press-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer d) -> gboolean {
            if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_KEY_c || ev->keyval == GDK_KEY_C)) {
                copy_selected_rows(static_cast<UiState*>(d));
                return TRUE;
            }
            return FALSE;
        }), &state);

    GtkTreeSelection* tree_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(state.tree_view));
    gtk_tree_selection_set_mode(tree_sel, GTK_SELECTION_MULTIPLE);

    state.scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(state.scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(state.scroll), state.tree_view);
    gtk_box_pack_start(GTK_BOX(root), state.scroll, TRUE, TRUE, 0);

    GtkWidget* status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(root), status_bar, FALSE, FALSE, 0);

    state.status_label = gtk_label_new("Disconnected");
    gtk_widget_set_halign(state.status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status_bar), state.status_label, TRUE, TRUE, 0);

    state.count_label = gtk_label_new("0 lines");
    gtk_widget_set_halign(state.count_label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(status_bar), state.count_label, FALSE, FALSE, 0);

    g_signal_connect(state.connect_btn, "clicked", G_CALLBACK(on_connect_clicked), &state);
    g_signal_connect(state.clear_btn, "clicked", G_CALLBACK(on_clear_clicked), &state);
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(on_settings_clicked), &state);
    g_signal_connect(check_svc_btn, "clicked", G_CALLBACK(on_check_services_clicked), &state);
    g_signal_connect(state.filter_entry, "changed", G_CALLBACK(on_filter_changed), &state);
    g_signal_connect(state.window, "destroy", G_CALLBACK(on_destroy), &state);

    g_timeout_add(100, drain_pending, &state);

    gtk_widget_show_all(state.window);
    gtk_window_present(GTK_WINDOW(state.window));
    if (state.cfg.window_maximized) {
        gtk_window_maximize(GTK_WINDOW(state.window));
    }

    gtk_main();
    return 0;
}
