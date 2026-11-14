"""Exercise the production Home refresh scheduler across app visits."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/apps/home/app_home.cc').read_text()
begin = source.index('struct WeatherSchedule {')
end = source.index('WeatherSchedule s_weather_schedule;', begin)
schedule = source[begin:end]
constants = '\n'.join(re.findall(r'constexpr int64_t k(?:Weather(?:FirstDelay|Period|Retry)|AirPeriod)Ms = \d+;', source))
code = r'''
#include "apps/home/home_profile.h"
#include <cassert>
#include <cstdio>
#include <cstring>
''' + constants + '\n' + schedule + r'''
int main() {
    home_profile_t city{};
    std::snprintf(city.lat,sizeof(city.lat),"22.2707");
    std::snprintf(city.lon,sizeof(city.lon),"113.5767");
    WeatherSchedule fresh;
    assert(fresh.due(14999,0,false,city)==0);
    assert(fresh.due(15000,0,false,city)==WeatherSchedule::Both);
    fresh.begin(WeatherSchedule::Both,15000,city);
    fresh.finish(WeatherSchedule::Weather,16000,ESP_OK);
    fresh.finish(WeatherSchedule::Air,17000,ESP_OK);
    // A new Home worker has a later entry time but shares the original deadlines.
    assert(fresh.due(115000,100000,false,city)==0);
    std::snprintf(city.name,sizeof(city.name),"New name");
    assert(fresh.due(115000,100000,false,city)==0);
    // Weather refreshes every 15 minutes; air quality keeps its 30-minute ceiling.
    assert(fresh.due(915999,900000,false,city)==0);
    assert(fresh.due(916000,900000,false,city)==WeatherSchedule::Weather);
    assert(fresh.due(1816999,1800000,false,city)==WeatherSchedule::Weather);
    assert(fresh.due(1817000,1800000,false,city)==WeatherSchedule::Both);
    assert(fresh.due(115000,100000,true,city)==WeatherSchedule::Both);
    auto changed=city;std::snprintf(changed.lat,sizeof(changed.lat),"39.9042");
    assert(fresh.due(115000,100000,false,changed)==WeatherSchedule::Both);
    auto empty=city;empty.lon[0]=0;
    assert(fresh.due(115000,100000,true,empty)==0);

    WeatherSchedule retry;
    retry.begin(WeatherSchedule::Both,15000,city);
    retry.finish(WeatherSchedule::Weather,20000,ESP_FAIL);
    retry.finish(WeatherSchedule::Air,21000,ESP_OK);
    assert(retry.due(139999,130000,false,city)==0);
    assert(retry.due(140000,130000,false,city)==WeatherSchedule::Weather);
    retry.begin(WeatherSchedule::Weather,140000,city);
    retry.finish(WeatherSchedule::Weather,145000,ESP_OK);
    assert(retry.due(155000,150000,false,city)==0);
    // AQ failure does not introduce a new two-minute network retry cycle.
    retry.finish(WeatherSchedule::Air,150000,ESP_FAIL);
    assert(retry.due(270000,260000,false,city)==0);

    WeatherSchedule interrupted;
    interrupted.begin(WeatherSchedule::Both,15000,city);
    assert(interrupted.due(16000,16000,false,city)==WeatherSchedule::Both);
    interrupted.finish(WeatherSchedule::Weather,17000,ESP_OK);
    assert(interrupted.due(18000,18000,false,city)==WeatherSchedule::Air);
    puts("Home schedule: reuse across visits, 15-minute weather / 30-minute AQ expiry, bounded weather retry, AQ request ceiling, manual/location refresh, cancelled/partial work PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='passport-weather-schedule-') as temporary:
    path = Path(temporary) / 'schedule.cc'
    path.write_text(code)
    executable = path.with_suffix('')
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-Itests/home_mocks', '-Itests/radio_mocks', '-Imain',
                    str(path), '-o', str(executable)], cwd=root, check=True)
    subprocess.run([str(executable)], check=True)
