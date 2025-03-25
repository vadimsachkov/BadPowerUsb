#include <windows.h>
#include <Cfgmgr32.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <map>
#include <vector>
#include <algorithm>
#include <functional>
#include <cctype>

namespace fs = std::filesystem;
using namespace std;

string getCurrentTimestamp() {
    time_t now = time(0);
    tm local_tm;
    localtime_s(&local_tm, &now);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return string(buffer);
}

string getCurrentLogFileName() {
    time_t now = time(0);
    tm local_tm;
    localtime_s(&local_tm, &now);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "BadPowerUsb_%Y%m.log", &local_tm);
    return string(buffer);
}

void logAndPrint(const string& logPath, const string& message) {
    string fullMessage = getCurrentTimestamp() + " - " + message;
    cout << fullMessage << endl;
    string logFile = (fs::path(logPath) / getCurrentLogFileName()).string();
    ofstream log(logFile, ios::app);
    log << fullMessage << endl;
}

bool isDeviceConnected(const string& deviceInstancePath, const string& logPath) {
    DEVINST devRoot;
    if (CM_Locate_DevNodeA(&devRoot, NULL, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return false;
    }
    string deviceInstancePathUpper = deviceInstancePath;
    transform(deviceInstancePathUpper.begin(), deviceInstancePathUpper.end(),
        deviceInstancePathUpper.begin(), ::toupper);

    std::function<bool(DEVINST)> checkDeviceTree = [&](DEVINST devInst) -> bool {
        char devIdBuffer[1024];
        if (CM_Get_Device_IDA(devInst, devIdBuffer, sizeof(devIdBuffer), 0) == CR_SUCCESS) {
            string currentDevicePath = string(devIdBuffer);
            if (currentDevicePath.find(deviceInstancePathUpper) != string::npos) {
                logAndPrint(logPath, "Found connected device: " + currentDevicePath);
                return true;
            }
        }
        DEVINST child;
        if (CM_Get_Child(&child, devInst, 0) == CR_SUCCESS) {
            if (checkDeviceTree(child)) {
                return true;
            }
        }
        DEVINST sibling;
        if (CM_Get_Sibling(&sibling, devInst, 0) == CR_SUCCESS) {
            if (checkDeviceTree(sibling)) {
                return true;
            }
        }
        return false;
        };

    return checkDeviceTree(devRoot);
}

bool fileExists(const string& filename) {
    return fs::exists(filename);
}

time_t parseTimestamp(const string& timestamp) {
    tm t = {};
    istringstream ss(timestamp);
    ss >> get_time(&t, "%Y-%m-%d %H:%M:%S");
    return mktime(&t);
}

void deleteOldLogs(const string& directory, int maxAgeDays = 365) {
    auto now = chrono::system_clock::now();
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (fs::is_regular_file(entry) && entry.path().filename().string().rfind("BadPowerUsb_", 0) == 0) {
            auto ftime = fs::last_write_time(entry);
            auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(ftime - decltype(ftime)::clock::now() + chrono::system_clock::now());
            auto age = chrono::duration_cast<chrono::hours>(now - sctp).count() / 24;
            if (age > maxAgeDays) {
                fs::remove(entry);
            }
        }
    }
}

void showHelp() {
    cout << "Usage: BadPowerUsb [options]\n"
        << "\nRequired Parameters:\n"
        << "  -uid_usb <uid_usb>        USB device instance path\n"
        << "  -wait_min <minutes>       Max allowed minutes without USB connection\n"
        << "  -uptime_min <minutes>     Min system uptime in minutes before executing the command\n"
        << "  -exec \"<command>\"       Command to execute when conditions are met\n"
        << "  -pathlog <path>           Directory to store logs\n"
        << "\nOptional:\n"
        << "  -? /? ?                   Show this help text\n";
}

template<typename T>
bool parseInteger(const string& s, T& value) {
    try {
        size_t idx;
        value = stoi(s, &idx);
        return idx == s.length() && value > 0;
    }
    catch (...) {
        return false;
    }
}

int main(int argc, char* argv[]) {
    map<string, string> args;
    vector<string> flags(argv + 1, argv + argc);

    if (flags.empty() || find(flags.begin(), flags.end(), "-?") != flags.end() ||
        find(flags.begin(), flags.end(), "/?") != flags.end() ||
        find(flags.begin(), flags.end(), "?") != flags.end()) {
        showHelp();
        return 0;
    }

    for (size_t i = 0; i < flags.size(); i++) {
        if (flags[i].rfind("-", 0) == 0 && i + 1 < flags.size()) {
            args[flags[i]] = flags[i + 1];
            i++;
        }
    }

    if (!args.count("-uid_usb") || args["-uid_usb"].empty() ||
        !args.count("-exec") || args["-exec"].empty() ||
        !args.count("-pathlog") || args["-pathlog"].empty() ||
        !args.count("-wait_min") || !args.count("-uptime_min")) {
        cerr << "Missing required parameters." << endl;
        return 1;
    }

    string deviceInstancePath = args["-uid_usb"];
    string exec_cmd = args["-exec"];
    string log_path = args["-pathlog"];
    int wait_min = 0, uptime_min = 0;

    if (!parseInteger(args["-wait_min"], wait_min) || !parseInteger(args["-uptime_min"], uptime_min)) {
        cerr << "Invalid wait_min or uptime_min value." << endl;
        return 1;
    }

    string timestampFile = (fs::path(log_path) / "BadPowerUsb_last_success.txt").string();
    fs::create_directories(log_path);
    deleteOldLogs(log_path);

    // Separator line for each run
    logAndPrint(log_path, string(60, '-'));

    logAndPrint(log_path, "Starting check for USB device: " + deviceInstancePath);

    if (isDeviceConnected(deviceInstancePath, log_path)) {
        ofstream out(timestampFile);
        out << getCurrentTimestamp();
        out.close();
        logAndPrint(log_path, "USB device found. Timestamp updated.");
        return 0;
    }

    if (!fileExists(timestampFile)) {
        logAndPrint(log_path, "USB device not found. No timestamp file. Exiting.");
        return 0;
    }

    ifstream in(timestampFile);
    string lastTimestamp;
    getline(in, lastTimestamp);
    in.close();

    logAndPrint(log_path, "Last success timestamp read: " + lastTimestamp);

    time_t last = parseTimestamp(lastTimestamp);
    time_t now = time(0);
    double diff_minutes = difftime(now, last) / 60.0;
    ULONGLONG uptime_ms = GetTickCount64();
    ULONGLONG uptime_min_now = uptime_ms / (60 * 1000);

    stringstream ss;
    ss << "USB device not found. Time since last success: " << diff_minutes << " min, Uptime: " << uptime_min_now << " min.";
    logAndPrint(log_path, ss.str());

    stringstream c1;
    c1 << "Time since last success (" << static_cast<int>(diff_minutes) << ") ";
    c1 << (diff_minutes >= wait_min ? ">= " : "< ") << "wait_min (" << wait_min << ")";
    logAndPrint(log_path, c1.str());

    stringstream c2;
    c2 << "System uptime (" << uptime_min_now << ") ";
    c2 << (uptime_min_now >= uptime_min ? ">= " : "< ") << "uptime_min (" << uptime_min << ")";
    logAndPrint(log_path, c2.str());

    if (diff_minutes >= wait_min && uptime_min_now >= uptime_min) {
        logAndPrint(log_path, "Conditions met. Executing command: " + exec_cmd);
        system(exec_cmd.c_str());
    }
    else {
        logAndPrint(log_path, "Conditions NOT met. No action taken.");
    }

    return 0;
}