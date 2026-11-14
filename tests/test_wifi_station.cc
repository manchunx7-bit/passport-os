#include "wifi_mock.h"
#include <iostream>
#define private public
#include "ssid_manager.h"
#include "wifi_station.h"
#undef private
#include "../components/esp-wifi-connect/ssid_manager.cc"
#undef TAG
#include "../components/esp-wifi-connect/wifi_station.cc"

static wifi_ap_record_t ap(const char *ssid,int rssi,uint8_t id=1) {
 wifi_ap_record_t record;std::strcpy(reinterpret_cast<char *>(record.ssid),ssid);
 record.rssi=rssi;record.bssid[5]=id;return record;
}
static void scan(WifiStation &s,std::vector<wifi_ap_record_t> records) {
 mock_scan_records=std::move(records);mock_scan_cursor=0;
 WifiStation::WifiEventHandler(&s,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,nullptr);
 assert(mock_scan_records.empty());
}
static void connected(WifiStation &s) {
 ip_event_got_ip_t event;WifiStation::IpEventHandler(&s,IP_EVENT,IP_EVENT_STA_GOT_IP,&event);
 assert(s.IsConnected());
}
static void disconnected(WifiStation &s) {
 wifi_event_sta_disconnected_t event;WifiStation::WifiEventHandler(&s,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);
}
int main() {
 // A nonresponsive DHCP main must retain local resolution, with retries able
 // to reach backups. Supplied IPv4/IPv6 backups and write failures are safe.
 mock_dns[0].ip.u_addr.ip4.addr=0x0105a8c0;
 ensure_backup_dns(&mock_netif);
 assert(mock_dns[0].ip.u_addr.ip4.addr==0x0105a8c0);
 assert(mock_dns[1].ip.u_addr.ip4.addr==0x050505df && mock_dns[2].ip.u_addr.ip4.addr==0x1d1d1d77);
 const int dns_writes=mock_dns_writes;ensure_backup_dns(&mock_netif);assert(mock_dns_writes==dns_writes);
 mock_dns[1].ip.u_addr.ip4.addr=0x08080808;mock_dns[2].ip.type=6;
 ensure_backup_dns(&mock_netif);assert(mock_dns_writes==dns_writes && mock_dns[1].ip.u_addr.ip4.addr==0x08080808 && mock_dns[2].ip.type==6);
 mock_dns[1]={};mock_dns[2]={};mock_dns_error=ESP_FAIL;
 ensure_backup_dns(&mock_netif);assert(mock_dns[1].ip.u_addr.ip4.addr==0 && mock_dns[2].ip.u_addr.ip4.addr==0);
 mock_dns_error=ESP_OK;
 auto &saved=SsidManager::GetInstance();saved.Clear();
 saved.AddSsid("strong","strong-password");saved.AddSsid("weak-first","old-password");
 saved.AddSsid("weak-first","updated-password");
 SsidManager reload;assert(reload.GetSsidList().size()==2);
 assert(reload.GetSsidList()[0].password=="updated-password");
 assert(saved.GetSsidList()[0].ssid=="weak-first");
 {
  WifiStation station;int connections=0,disconnections=0;
  station.OnConnected([&](const std::string &){++connections;});
  station.OnDisconnected([&](int){++disconnections;});
  station.Start();assert(mock_scan_starts==1);
  scan(station,{ap("weak-first",-79),ap("unknown",-10),ap("strong",-30),ap("strong",-60,2)});
  assert(station.GetSsid()=="strong");assert(station.connect_queue_.size()==1);
  assert(mock_config.sta.sort_method==WIFI_CONNECT_AP_BY_SIGNAL && !mock_config.sta.bssid_set);
  // Strongest saved network fails authentication: retry then use the next saved network.
  for(int i=0;i<6;++i)disconnected(station);
  assert(station.GetSsid()=="weak-first" && station.password_=="updated-password");
  connected(station);assert(connections==1 && !station.timer_handle_->active);
  // A successful connection is stable. Neither a late scan nor timer scans switch it.
  const int previous_connects=mock_connects,previous_scans=mock_scan_starts;
  scan(station,{ap("strong",-15)});station.StartScan();
  assert(station.GetSsid()=="weak-first" && mock_connects==previous_connects && mock_scan_starts==previous_scans);
  // Losing a working connection rescans, rather than retrying the stale SSID first.
  disconnected(station);assert(disconnections==1 && mock_scan_starts==previous_scans+1);
  assert(mock_connects==previous_connects);
  scan(station,{ap("weak-first",-70),ap("strong",-25)});assert(station.GetSsid()=="strong");
  connected(station);assert(connections==2);
  // The network that just worked moves to the front of the saved list.
  assert(saved.GetSsidList()[0].ssid=="strong");
  {SsidManager reload2;assert(reload2.GetSsidList()[0].ssid=="strong");}
  station.Stop();assert(station.connect_queue_.empty());
 }
 {
  // Offline, waiting out a long backoff: a key press scans at once and
  // restarts the backoff, at most every 15 s; a scan in flight is left alone.
  WifiStation station;station.SetNetworkAllowed([](const std::string &){return false;});station.SetScanIntervalRange(10,60);station.Start();
  for(int i=0;i<4;++i)scan(station,{});
  assert(station.timer_handle_->active&&station.timer_handle_->delay==60*1000*1000);
  const int scans=mock_scan_starts;
  mock_now_us+=100*1000*1000;station.ScanNow();
  assert(station.timer_handle_->active&&station.timer_handle_->delay==100*1000);
  station.timer_handle_->callback(station.timer_handle_->arg);assert(mock_scan_starts==scans+1);
  scan(station,{});assert(station.timer_handle_->delay==10*1000*1000);
  mock_now_us+=5*1000*1000;station.ScanNow();assert(station.timer_handle_->delay==10*1000*1000&&station.scan_current_interval_microseconds_==20*1000*1000);
  mock_now_us+=20*1000*1000;station.timer_handle_->active=false;station.ScanNow();
  assert(!station.timer_handle_->active&&station.scan_current_interval_microseconds_==10*1000*1000);
  station.Stop();
 }
 {
  // Screen off, nothing streaming: offline scans wait 5 minutes, a key press
  // still scans at once, and leaving idle restores the normal backoff.
  WifiStation station;station.SetNetworkAllowed([](const std::string &){return false;});station.SetScanIntervalRange(10,60);station.Start();
  station.SetScanIdle(true);scan(station,{});
  assert(station.timer_handle_->delay==int64_t(WifiStation::kIdleScanIntervalSeconds)*1000*1000);
  mock_now_us+=400*1000*1000;station.ScanNow();assert(station.timer_handle_->delay==100*1000);
  station.timer_handle_->callback(station.timer_handle_->arg);
  station.SetScanIdle(false);scan(station,{});assert(station.timer_handle_->delay==10*1000*1000);
  station.Stop();
 }
 assert(mock_timers==0);
 // Saved profiles survive a fresh station/SSID-manager instance. Unsorted dense
 // scans retain the strongest BSSID per SSID without a candidate per unknown AP.
 for(int i=0;i<15;++i)saved.AddSsid("saved-"+std::to_string(i),"password");
 SsidManager after_reboot;assert(after_reboot.GetSsidList().size()==10);
 assert(after_reboot.GetSsidList().front().ssid=="saved-14");
 {
  WifiStation station;station.remember_bssid_=1;station.Start();
  std::vector<wifi_ap_record_t> dense(500,ap("unknown",-5));
  dense.push_back(ap("saved-14",-80,1));dense.push_back(ap("saved-13",-20,2));dense.push_back(ap("saved-13",-45,3));
  dense.push_back(ap("saved-12",-65,4));scan(station,dense);
  assert(station.GetSsid()=="saved-13");assert(station.connect_queue_.size()==9);
  assert(mock_config.sta.bssid_set && mock_config.sta.bssid[5]==2);
  assert(station.connect_queue_[0].ssid=="saved-12");
  station.Stop();
 }
 {
  WifiStation station;station.SetNetworkAllowed([](const std::string &){return false;});mock_scan_error=ESP_FAIL;station.Start();
  assert(station.timer_handle_->active);mock_scan_error=ESP_OK;
  station.timer_handle_->callback(station.timer_handle_->arg);
  scan(station,{});assert(station.timer_handle_->active);
  mock_records_error=ESP_FAIL;scan(station,{ap("saved-14",-10)});
  assert(station.timer_handle_->active);mock_records_error=ESP_OK;
  station.Stop();
 }
 assert(mock_timers==0);
 {
  saved.Clear();saved.AddSsid("hidden","password");saved.AddSsid("home","password");saved.AddSsid("phone","password");
  WifiStation station;station.SetPreferredSsid([]{return std::string("phone");});station.remember_bssid_=1;station.Start();
  scan(station,{ap("home",-25),ap("phone",-80)});assert(station.GetSsid()=="phone");
  for(int i=0;i<6;++i)disconnected(station);assert(station.GetSsid()=="home");
  for(int i=0;i<6;++i)disconnected(station);assert(station.GetSsid()=="hidden");
  assert(!mock_config.sta.bssid_set && mock_config.sta.channel==0);
  connected(station);station.Stop();
 }
 std::cout<<"WiFi: saved profiles, RSSI, fallback, hidden SSID, preferred hotspot, scan errors and backoff PASS\n";
}
