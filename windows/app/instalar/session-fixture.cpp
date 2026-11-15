// Test-only session writer: private stdin, loopback, marked disposable root.
#include "../src/installer-context.h"
#include <iostream>
int main() {
  try {
    if (app::installer::TestRoot().empty()) return 2;
    std::string cookie;
    std::getline(std::cin,cookie);
    net::Client client(app::installer::Origin());
    bool ok=client.RestoreCookie(cookie) && client.Get("/api/me").status==200 && app::SaveSession(client.base(),cookie);
    SecureZeroMemory(cookie.data(),cookie.size());
    return ok?0:1;
  } catch (...) { return 2; }
}
