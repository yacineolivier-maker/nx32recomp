/* time : horloges (heure du PC) et fuseau horaire (heure locale Windows).
 * Interfaces : https://switchbrew.org/wiki/PSC_services#time:u */
#include "ipc.h"

#include <cstdio>
#include <ctime>

namespace {

void ok(Request &, Response &) {}
const uint64_t k_source[2] = {0x4D4B38504354494Dull, 0x31ull}; /* identifiant de la source d'horloge */

int64_t maintenant() { return (int64_t)time(nullptr); }

void push_time_point(Response &rs) { /* SteadyClockTimePoint : s64 secondes + uuid de source */
    rs.push<int64_t>(maintenant());
    rs.push<uint64_t>(k_source[0]);
    rs.push<uint64_t>(k_source[1]);
}

std::shared_ptr<Service> system_clock() {
    auto s = std::make_shared<TableService>("time:ISystemClock");
    s->cmds[0] = [](Request &, Response &rs) { rs.push<int64_t>(maintenant()); }; /* GetCurrentTime */
    s->cmds[1] = ok;
    s->cmds[2] = [](Request &, Response &rs) { rs.push<int64_t>(0); push_time_point(rs); }; /* GetSystemClockContext */
    s->cmds[3] = ok;
    s->cmds[4] = [](Request &, Response &rs) { uint32_t r; svc_new_event(0, &r); rs.copy_handles.push_back(r); };
    return s;
}

std::shared_ptr<Service> steady_clock() {
    auto s = std::make_shared<TableService>("time:ISteadyClock");
    s->cmds[0] = [](Request &, Response &rs) { push_time_point(rs); }; /* GetCurrentTimePoint */
    s->cmds[2] = [](Request &, Response &rs) { rs.push<int64_t>(0); };
    s->cmds[100] = [](Request &, Response &rs) { rs.push<int64_t>(maintenant()); }; /* GetRtcValue */
    s->cmds[101] = [](Request &, Response &rs) { rs.push<uint8_t>(0); };
    s->cmds[102] = [](Request &, Response &rs) { rs.push<uint32_t>(0); };
    s->cmds[200] = [](Request &, Response &rs) { rs.push<int64_t>(0); };
    return s;
}

/* CalendarTime (8) + CalendarAdditionalInfo (0x18) depuis l'heure locale */
void push_calendrier(Response &rs, int64_t posix) {
    time_t t = (time_t)posix;
    struct tm lt;
    localtime_s(&lt, &t);
    rs.push<int16_t>((int16_t)(lt.tm_year + 1900));
    rs.push<int8_t>((int8_t)(lt.tm_mon + 1));
    rs.push<int8_t>((int8_t)lt.tm_mday);
    rs.push<int8_t>((int8_t)lt.tm_hour);
    rs.push<int8_t>((int8_t)lt.tm_min);
    rs.push<int8_t>((int8_t)lt.tm_sec);
    rs.push<int8_t>(0);
    rs.push<uint32_t>((uint32_t)lt.tm_wday);
    rs.push<uint32_t>((uint32_t)lt.tm_yday);
    char nom[8] = {};
    memcpy(nom, lt.tm_isdst ? "CEST" : "CET", lt.tm_isdst ? 4 : 3);
    for (int i = 0; i < 8; i++) rs.push<char>(nom[i]);
    rs.push<uint32_t>(lt.tm_isdst ? 1 : 0);
    rs.push<int32_t>(lt.tm_isdst ? 7200 : 3600);
}

std::shared_ptr<Service> timezone_service() {
    auto s = std::make_shared<TableService>("time:ITimeZoneService");
    s->cmds[0] = [](Request &, Response &rs) { /* GetDeviceLocationName (0x24) */
        char n[0x24] = "Europe/Paris";
        for (char ch : n) rs.push<char>(ch);
    };
    s->cmds[1] = ok;
    s->cmds[2] = [](Request &, Response &rs) { rs.push<int32_t>(1); };
    s->cmds[3] = [](Request &rq, Response &rs) {
        Buf b = rq.out();
        if (b.addr && b.size >= 0x24) { memset(g_mem + b.addr, 0, 0x24); memcpy(g_mem + b.addr, "Europe/Paris", 12); }
        rs.push<int32_t>(1);
    };
    s->cmds[4] = [](Request &rq, Response &) { /* LoadTimeZoneRule : règle opaque, la conversion utilise l'heure locale */
        Buf b = rq.out();
        if (b.addr) memset(g_mem + b.addr, 0, b.size < 0x100 ? b.size : 0x100);
    };
    s->cmds[5] = [](Request &, Response &rs) { for (int i = 0; i < 16; i++) rs.push<uint8_t>(0); };
    s->cmds[100] = [](Request &rq, Response &rs) { push_calendrier(rs, rq.pop<int64_t>()); };  /* ToCalendarTime */
    s->cmds[101] = [](Request &rq, Response &rs) { push_calendrier(rs, rq.pop<int64_t>()); }; /* ToCalendarTimeWithMyRule */
    auto vers_posix = [](Request &rq, Response &rs) { /* ToPosixTime(WithMyRule) : CalendarTime -> s64[] */
        struct tm lt = {};
        lt.tm_year = rq.pop<int16_t>() - 1900;
        lt.tm_mon = rq.pop<int8_t>() - 1;
        lt.tm_mday = rq.pop<int8_t>();
        lt.tm_hour = rq.pop<int8_t>();
        lt.tm_min = rq.pop<int8_t>();
        lt.tm_sec = rq.pop<int8_t>();
        lt.tm_isdst = -1;
        int64_t p = (int64_t)mktime(&lt);
        Buf b = rq.out();
        if (b.addr && b.size >= 8) wr64(b.addr, (uint64_t)p);
        rs.push<int32_t>(1);
    };
    s->cmds[201] = vers_posix;
    s->cmds[202] = vers_posix;
    return s;
}

std::shared_ptr<Service> time_service(const char *nom) {
    auto s = std::make_shared<TableService>(nom);
    s->cmds[0] = [](Request &, Response &rs) { rs.objects.push_back(system_clock()); };
    s->cmds[1] = [](Request &, Response &rs) { rs.objects.push_back(system_clock()); };
    s->cmds[2] = [](Request &, Response &rs) { rs.objects.push_back(steady_clock()); };
    s->cmds[3] = [](Request &, Response &rs) { rs.objects.push_back(timezone_service()); };
    s->cmds[4] = [](Request &, Response &rs) { rs.objects.push_back(system_clock()); };
    s->cmds[5] = [](Request &, Response &rs) { rs.objects.push_back(system_clock()); };
    s->cmds[100] = [](Request &, Response &rs) { rs.push<uint8_t>(1); };
    return s;
}

} // namespace

void time_register() {
    ipc_register("time:u", [] { return time_service("time:u"); });
    ipc_register("time:a", [] { return time_service("time:a"); });
    ipc_register("time:s", [] { return time_service("time:s"); });
}
