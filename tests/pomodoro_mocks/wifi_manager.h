#pragma once
class WifiManager {
public:
    static WifiManager &GetInstance() { static WifiManager w; return w; }
    bool IsConnected() const { return connected; }
    int GetRssi() const { return rssi; }
    bool connected = true;
    int rssi = -55;
};
