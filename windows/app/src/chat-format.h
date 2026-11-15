#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace app {
inline int64_t ChatSeconds(std::string const& stamp) {
  std::tm t{}; int y, mo, d, h, mi; double s = 0;
  if (sscanf_s(stamp.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) < 5) return 0;
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = int(s);
  int64_t utc = _mkgmtime(&t);
  auto tz = stamp.find_first_of("+-", 19);
  if (tz != std::string::npos) { int oh = 0, om = 0; sscanf_s(stamp.c_str() + tz + 1, "%d:%d", &oh, &om); utc -= (stamp[tz] == '+' ? 1 : -1) * (oh * 3600 + om * 60); }
  return utc;
}
inline std::tm ChatLocal(int64_t utc) { std::tm t{}; time_t v = time_t(utc); localtime_s(&t, &v); return t; }
inline int64_t ChatMilliseconds(std::string const& stamp) {
  int millis = 0;
  if (stamp.size() > 19 && stamp[19] == '.') {
    int scale = 100;
    for (size_t i = 20; i < stamp.size() && scale && stamp[i] >= '0' && stamp[i] <= '9'; ++i, scale /= 10)
      millis += (stamp[i] - '0') * scale;
  }
  return ChatSeconds(stamp) * 1000 + millis;
}
inline bool ChatSameDay(std::tm const& a, std::tm const& b) { return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday; }
inline std::wstring ChatClock(std::string const& stamp, time_t now = std::time(nullptr)) {
  auto t = ChatLocal(ChatSeconds(stamp)); wchar_t b[32];
  swprintf_s(b, L"%02d:%02d", t.tm_hour, t.tm_min);
  if (ChatSameDay(t, ChatLocal(now))) return b;
  if (ChatSameDay(t, ChatLocal(now - 86400))) return std::wstring(L"Ontem ") + b;
  swprintf_s(b, L"%02d/%02d %02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min); return b;
}
inline std::wstring ChatShortDate(std::string const& stamp) {
  auto t = ChatLocal(ChatSeconds(stamp)); wchar_t b[16];
  swprintf_s(b, L"%02d/%02d/%02d", t.tm_mday, t.tm_mon + 1, (t.tm_year + 1900) % 100); return b;
}
inline constexpr const wchar_t* ChatDays[] = {L"domingo", L"segunda-feira", L"terça-feira", L"quarta-feira", L"quinta-feira", L"sexta-feira", L"sábado"};
inline constexpr const wchar_t* ChatMonths[] = {L"janeiro", L"fevereiro", L"março", L"abril", L"maio", L"junho", L"julho", L"agosto", L"setembro", L"outubro", L"novembro", L"dezembro"};
inline std::wstring ChatDay(std::string const& stamp) {
  auto t = ChatLocal(ChatSeconds(stamp)); wchar_t day[4]; swprintf_s(day, L"%02d", t.tm_mday);
  return std::wstring(ChatDays[t.tm_wday]) + L", " + day + L" de " + ChatMonths[t.tm_mon];
}
inline std::wstring ChatFullStamp(std::string const& stamp) {
  auto t = ChatLocal(ChatSeconds(stamp)); wchar_t clock[32];
  swprintf_s(clock, L" de %d às %02d:%02d:%02d", t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
  return std::wstring(ChatDays[t.tm_wday]) + L", " + std::to_wstring(t.tm_mday) + L" de " + ChatMonths[t.tm_mon] + clock;
}
inline std::wstring ChatCallText(bool mine, bool rejected, std::wstring const& who) {
  if (rejected) return mine ? who + L" recusou a chamada" : L"Voc\u00ea recusou a chamada";
  return mine ? L"Voc\u00ea ligou \u2014 ningu\u00e9m atendeu" : L"Chamada perdida de " + who;
}
inline std::wstring ChatPinText(bool mine, std::wstring const& who) {
  return (mine ? std::wstring(L"Voc\u00ea") : who) + L" fixou uma mensagem.";
}

}
