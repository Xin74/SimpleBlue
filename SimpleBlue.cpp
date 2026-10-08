// ============================================================================
// SimpleBlue.cpp
// Minimal BLE scanner + connect + service/characteristic decode + read/write/notify
// ============================================================================

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <windows.h>

#include "BleEnums.h"
#include "SBHelper1.h"

// WinRT includes
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace SimpleBlue;

// ============================================================================
// Prompt state
// ============================================================================
static std::string g_promptDevice = "";
static int g_promptService = 0;
static int g_promptChar = 0;

// ============================================================================
// GUID → normalized string
// ============================================================================
std::string GuidNorm(const winrt::guid& g)
{
    wchar_t buffer[64];
    int len = StringFromGUID2(g, buffer, 64);
    if (len <= 0)
        return {};

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    std::string s(sizeNeeded - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, &s[0], sizeNeeded, nullptr, nullptr);

    if (!s.empty() && s.front() == '{')
        s = s.substr(1, s.size() - 2);

    return s;
}

// ============================================================================
// Decode characteristic properties
// ============================================================================
std::string DecodeProps(GattCharacteristicProperties p)
{
    std::string out;

    if ((p & GattCharacteristicProperties::Read) == GattCharacteristicProperties::Read)
        out += "Re ";
    if ((p & GattCharacteristicProperties::Write) == GattCharacteristicProperties::Write)
        out += "Wr ";
    if ((p & GattCharacteristicProperties::WriteWithoutResponse) == GattCharacteristicProperties::WriteWithoutResponse)
        out += "WnR ";
    if ((p & GattCharacteristicProperties::Notify) == GattCharacteristicProperties::Notify)
        out += "Nt ";
    if ((p & GattCharacteristicProperties::Indicate) == GattCharacteristicProperties::Indicate)
        out += "In ";

    if (out.empty())
        return "(none)";
    if (out.back() == ' ')
        out.pop_back();

    return out;
}

// ============================================================================
// Device storage
// ============================================================================
struct DeviceInfo
{
    uint64_t address;
    std::string name;
    int rssi;
};

std::vector<DeviceInfo> devices;
BluetoothLEDevice connectedDevice{ nullptr };

// ============================================================================
// Find device by partial MAC or name
// ============================================================================
uint64_t parseMac(const std::string& s)
{
    if (s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        return std::stoull(s, nullptr, 16);
    return 0;
}

DeviceInfo* findDevice(const std::string& key)
{
    uint64_t mac = parseMac(key);
    if (mac != 0)
    {
        for (auto& d : devices)
        {
            char buf[20];
            sprintf_s(buf, "%012llx", d.address);
            std::string macHex = buf;
            if (macHex.find(key) != std::string::npos)
                return &d;
        }
    }

    std::string keyLower = key;
    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);

    for (auto& d : devices)
    {
        std::string nameLower = d.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        if (nameLower.find(keyLower) != std::string::npos)
            return &d;
    }

    return nullptr;
}

// ============================================================================
// READ
// ============================================================================
void CmdRead(int svcIndex, int charIndex)
{
    if (!connectedDevice)
    {
        std::cout << "No device connected.\n";
        return;
    }

    auto svcs = connectedDevice.GetGattServicesAsync().get().Services();
    if (svcIndex < 1 || svcIndex >(int)svcs.Size())
    {
        std::cout << "Invalid service index.\n";
        return;
    }

    auto svc = svcs.GetAt(svcIndex - 1);
    auto chars = svc.GetCharacteristicsAsync().get().Characteristics();

    if (charIndex < 1 || charIndex >(int)chars.Size())
    {
        std::cout << "Invalid characteristic index.\n";
        return;
    }

    auto ch = chars.GetAt(charIndex - 1);

    auto result = ch.ReadValueAsync().get();
    if (result.Status() != GattCommunicationStatus::Success)
    {
        std::cout << "Read failed.\n";
        return;
    }

    auto reader = DataReader::FromBuffer(result.Value());
    std::vector<uint8_t> data(reader.UnconsumedBufferLength());
    reader.ReadBytes(data);

    std::cout << "Read: ";
    std::cout << std::hex << std::setfill('0');
    for (auto b : data)
        std::cout << std::setw(2) << (int)b << " ";
    std::cout << std::dec << "\n";
}

// ============================================================================
// WRITE
// ============================================================================
void CmdWrite(int svcIndex, int charIndex, const std::string& hex)
{
    if (!connectedDevice)
    {
        std::cout << "No device connected.\n";
        return;
    }

    auto svcs = connectedDevice.GetGattServicesAsync().get().Services();
    if (svcIndex < 1 || svcIndex >(int)svcs.Size())
    {
        std::cout << "Invalid service index.\n";
        return;
    }

    auto svc = svcs.GetAt(svcIndex - 1);
    auto chars = svc.GetCharacteristicsAsync().get().Characteristics();

    if (charIndex < 1 || charIndex >(int)chars.Size())
    {
        std::cout << "Invalid characteristic index.\n";
        return;
    }

    auto ch = chars.GetAt(charIndex - 1);

    std::stringstream ss(hex);
    std::string token;
    std::vector<uint8_t> bytes;

    while (ss >> token)
        bytes.push_back((uint8_t)std::stoul(token, nullptr, 16));

    DataWriter writer;
    for (auto b : bytes)
        writer.WriteByte(b);

    auto buffer = writer.DetachBuffer();
    auto status = ch.WriteValueAsync(buffer).get();

    if (status != GattCommunicationStatus::Success)
        std::cout << "Write failed.\n";
    else
        std::cout << "Write OK.\n";
}

// ============================================================================
// NOTIFY
// ============================================================================
void CmdNotify(int svcIndex, int charIndex)
{
    if (!connectedDevice)
    {
        std::cout << "No device connected.\n";
        return;
    }

    auto svcs = connectedDevice.GetGattServicesAsync().get().Services();
    if (svcIndex < 1 || svcIndex >(int)svcs.Size())
    {
        std::cout << "Invalid service index.\n";
        return;
    }

    auto svc = svcs.GetAt(svcIndex - 1);
    auto chars = svc.GetCharacteristicsAsync().get().Characteristics();

    if (charIndex < 1 || charIndex >(int)chars.Size())
    {
        std::cout << "Invalid characteristic index.\n";
        return;
    }

    auto ch = chars.GetAt(charIndex - 1);

    auto status = ch.WriteClientCharacteristicConfigurationDescriptorAsync(
        GattClientCharacteristicConfigurationDescriptorValue::Notify).get();

    if (status != GattCommunicationStatus::Success)
    {
        std::cout << "Notify failed.\n";
        return;
    }

    std::string svcUuidStr = GuidNorm(svc.Uuid());
    std::string name = DecodeUuid(ch.Uuid(), svcUuidStr);

    ch.ValueChanged([name](auto const&, GattValueChangedEventArgs const& args)
        {
            auto reader = DataReader::FromBuffer(args.CharacteristicValue());
            std::vector<uint8_t> data(reader.UnconsumedBufferLength());
            reader.ReadBytes(data);

            std::cout << "[NOTIFY] " << name << ": ";
            std::cout << std::hex << std::setfill('0');
            for (auto b : data)
                std::cout << std::setw(2) << (int)b << " ";
            std::cout << std::dec << "\n";
        });

    std::cout << "Notifications enabled for " << name << "\n";
}

// ============================================================================
// Prompt builder
// ============================================================================
std::string BuildPrompt()
{
    std::stringstream ss;

    if (!g_promptDevice.empty())
        ss << g_promptDevice;

    if (g_promptService > 0)
        ss << "/" << g_promptService;

    if (g_promptChar > 0)
        ss << "/" << g_promptChar;

    ss << "> ";
    return ss.str();
}

// ============================================================================
// MAIN
// ============================================================================
int main()
{
    init_apartment();

    std::cout << "SimpleBlue ready.\n";

    BluetoothLEAdvertisementWatcher watcher;

    watcher.Received([&](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& args)
        {
            uint64_t addr = args.BluetoothAddress();
            auto name = winrt::to_string(args.Advertisement().LocalName());
            int rssi = args.RawSignalStrengthInDBm();

            auto it = std::find_if(devices.begin(), devices.end(),
                [&](const DeviceInfo& d) { return d.address == addr; });

            if (it == devices.end())
            {
                devices.push_back({ addr, name, rssi });

                std::cout << "Device: " << std::hex << addr
                    << " RSSI: " << std::dec << rssi;

                if (!name.empty())
                    std::cout << " Name: " << name;

                std::cout << "\n";
            }
            else
            {
                it->rssi = rssi;
                if (!name.empty())
                    it->name = name;
            }
        });

    std::string cmd;
    while (true)
    {
        std::cout << BuildPrompt();
        if (!std::getline(std::cin, cmd))
            break;

        // SCAN
        if (cmd == "scan")
        {
            std::cout << "Scanning for 10 seconds...\n";
            devices.clear();

            watcher.Start();

            ULONGLONG start = GetTickCount64();
            const ULONGLONG duration = 10000;

            while (GetTickCount64() - start < duration)
            {
                MSG msg{};
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                Sleep(10);
            }

            watcher.Stop();
            std::cout << "Scan complete.\n";
            continue;
        }

        // ====================================================================
        // scanD — scan for specific device and auto-connect immediately
        // ====================================================================
        if (cmd.rfind("scanD ", 0) == 0)
        {
            std::string key = cmd.substr(6);

           // std::cout << "Scanning for device matching \"" << key << "\"...\n";
            devices.clear();

            watcher.Start();

            ULONGLONG start = GetTickCount64();
            const ULONGLONG duration = 10000;

            DeviceInfo* found = nullptr;

            while (GetTickCount64() - start < duration)
            {
                MSG msg{};
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                found = findDevice(key);
                if (found)
                {
                    // Stop scanning immediately when found
                    watcher.Stop();
                    break;
                }

                Sleep(10);
            }

            watcher.Stop();

            if (!found)
            {
                std::cout << "Device not found.\n";
                continue;
            }

           // std::cout << "Found: " << found->name
           //     << " (" << std::hex << found->address << ")\n";
           // std::cout << "Connecting...\n";

            try
            {
                // Connect directly by MAC address (most reliable)
                connectedDevice = BluetoothLEDevice::FromBluetoothAddressAsync(found->address).get();

                if (connectedDevice)
                {
                    g_promptDevice = found->name.empty() ? "Device" : found->name;
                    g_promptService = 0;
                    g_promptChar = 0;

                   // std::cout << "Connected.\n";
                }
                else
                {
                    std::cout << "Failed to connect.\n";
                }
            }
            catch (...)
            {
                std::cout << "Connection error.\n";
            }

            continue;
        }


        // ScanP
        if (cmd == "ScanP")
        {
            std::cout << "Checking paired devices...\n";

            auto selector = BluetoothLEDevice::GetDeviceSelectorFromPairingState(true);
            auto pairedDevices = DeviceInformation::FindAllAsync(selector).get();

            for (auto const& dev : pairedDevices)
            {
                std::string id = winrt::to_string(dev.Id());
                std::string macHex = id.substr(20, 12);
                uint64_t addr = std::stoull(macHex, nullptr, 16);

                auto it = std::find_if(devices.begin(), devices.end(),
                    [&](const DeviceInfo& d) { return d.address == addr; });

                if (it != devices.end())
                {
                    std::cout << "Paired & Present: " << std::hex << addr
                        << " RSSI: " << std::dec << it->rssi;

                    if (!it->name.empty())
                        std::cout << " Name: " << it->name;

                    std::cout << "\n";
                }
            }
            continue;
        }

        // SEARCH
        if (cmd.rfind("search ", 0) == 0)
        {
            std::string term = cmd.substr(7);
            std::transform(term.begin(), term.end(), term.begin(), ::tolower);

            for (auto& d : devices)
            {
                std::string nameLower = d.name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

                if (nameLower.find(term) != std::string::npos)
                {
                    std::cout << "Match: " << std::hex << d.address
                        << " Name: " << d.name
                        << " RSSI: " << std::dec << d.rssi << "\n";
                }
            }
            continue;
        }

        // OPEN
        if (cmd.rfind("open ", 0) == 0)
        {
            std::string key = cmd.substr(5);

            DeviceInfo* d = findDevice(key);
            if (!d)
            {
                std::cout << "No matching device.\n";
                continue;
            }

            std::cout << "Connecting to " << std::hex << d->address << "...\n";

            connectedDevice = BluetoothLEDevice::FromBluetoothAddressAsync(d->address).get();

            if (!connectedDevice)
            {
                std::cout << "Failed to connect.\n";
                continue;
            }

            g_promptDevice = d->name.empty() ? "Device" : d->name;
            g_promptService = 0;
            g_promptChar = 0;

            std::cout << "Connected.\n";
            continue;
        }

        // ====================================================================
        // CLOSE — disconnect from current device
        // ====================================================================
        if (cmd == "close")
        {
            if (!connectedDevice)
            {
                std::cout << "No device is currently connected.\n";
                continue;
            }

            try
            {
                // Properly dispose the WinRT device
                connectedDevice.Close();
            }
            catch (...)
            {
                // Even if Close throws, we still reset state
            }

            connectedDevice = nullptr;

            // Reset prompt state
            g_promptDevice = "";
            g_promptService = 0;
            g_promptChar = 0;

            std::cout << "Disconnected.\n";
            continue;
        }


        // SERVICES
        if (cmd == "services")
        {
            if (!connectedDevice)
            {
                std::cout << "No device connected.\n";
                continue;
            }

            auto svcs = connectedDevice.GetGattServicesAsync().get().Services();

            int index = 1;
            for (auto const& svc : svcs)
            {
                std::cout << "Service #" << std::dec << index++
                    << ": " << DecodeUuid(svc.Uuid()) << "\n";
            }
            continue;
        }

        // CHARS
        if (cmd.rfind("chars ", 0) == 0)
        {
            if (!connectedDevice)
            {
                std::cout << "No device connected.\n";
                continue;
            }

            int svcIndex = std::stoi(cmd.substr(6));
            auto svcs = connectedDevice.GetGattServicesAsync().get().Services();

            if (svcIndex < 1 || svcIndex >(int)svcs.Size())
            {
                std::cout << "Invalid service index.\n";
                continue;
            }

            auto svc = svcs.GetAt(svcIndex - 1);
            auto chars = svc.GetCharacteristicsAsync().get().Characteristics();

            g_promptService = svcIndex;
            g_promptChar = 0;

            std::string svcUuidStr = GuidNorm(svc.Uuid());

            int index = 1;
            for (auto const& ch : chars)
            {
                auto props = ch.CharacteristicProperties();

                std::cout << "Char #" << std::dec << index++
                    << ": " << DecodeUuid(ch.Uuid(), svcUuidStr)
                    << " Properties: " << (int)props
                    << " (" << DecodeProps(props) << ")\n";
            }
            continue;
        }

        // ====================================================================
        // CHARINFO — detailed characteristic info for a service
        // ====================================================================
        if (cmd == "charsinfo")
        {
            if (!connectedDevice)
            {
                std::cout << "No device connected.\n";
                continue;
            }

            if (g_promptService <= 0)
            {
                std::cout << "Select a service first using: chars <svc>\n";
                continue;
            }

            auto svcs = connectedDevice.GetGattServicesAsync().get().Services();

            if (g_promptService < 1 || g_promptService >(int)svcs.Size())
            {
                std::cout << "Invalid service index.\n";
                continue;
            }

            auto svc = svcs.GetAt(g_promptService - 1);
            auto chars = svc.GetCharacteristicsAsync().get().Characteristics();

            std::string svcUuidStr = GuidNorm(svc.Uuid());

            std::cout << "Characteristics for service #" << g_promptService
                << " (" << DecodeUuid(svc.Uuid()) << "):\n";

            int index = 1;
            for (auto const& ch : chars)
            {
                auto props = ch.CharacteristicProperties();
                std::string uuidStr = GuidNorm(ch.Uuid());
                std::string friendly = DecodeUuid(ch.Uuid(), svcUuidStr);

                std::cout << "--------------------------------------------------\n";
                std::cout << "Char #" << std::dec << index++ << "\n";
                std::cout << "  UUID:       " << uuidStr << "\n";
                std::cout << "  Friendly:   " << friendly << "\n";
                std::cout << "  Properties: " << (int)props
                    << " (" << DecodeProps(props) << ")\n";
            }

            std::cout << "--------------------------------------------------\n";
            continue;
        }


        // READ
        if (cmd.rfind("read ", 0) == 0)
        {
            std::stringstream ss(cmd.substr(5));
            int svc, ch;
            ss >> svc >> ch;

            if (!ss)
            {
                std::cout << "Usage: read <svc> <char>\n";
                continue;
            }

            g_promptService = svc;
            g_promptChar = ch;

            CmdRead(svc, ch);
            continue;
        }

        // WRITE
        if (cmd.rfind("write ", 0) == 0)
        {
            std::stringstream ss(cmd.substr(6));
            int svc, ch;
            ss >> svc >> ch;

            std::string hex;
            std::getline(ss, hex);

            if (!ss || hex.empty())
            {
                std::cout << "Usage: write <svc> <char> <hex bytes>\n";
                continue;
            }

            g_promptService = svc;
            g_promptChar = ch;

            CmdWrite(svc, ch, hex);
            continue;
        }

        // NOTIFY
        if (cmd.rfind("notify ", 0) == 0)
        {
            std::stringstream ss(cmd.substr(7));
            int svc, ch;
            ss >> svc >> ch;

            if (!ss)
            {
                std::cout << "Usage: notify <svc> <char>\n";
                continue;
            }

            g_promptService = svc;
            g_promptChar = ch;

            CmdNotify(svc, ch);
            continue;
        }

        // EXIT
        if (cmd == "exit")
        {
            watcher.Stop();
            break;
        }
    }

    return 0;
}
