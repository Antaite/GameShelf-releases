#pragma once
// The user's list of servers (name, address, account, password) and which one is in use.
// Stored in an ini-like text file on the SD card. Portable: also built by the desktop tests.
#include <string>
#include <vector>

namespace servers {

struct Server {
    std::string id;    // "s1", "s2", … (stable, used as the section name)
    std::string name;  // shown in the menu and the title, e.g. "家裡 NAS"
    std::string url;   // address without account / password, e.g. https://nas:5006/games/
    std::string user, pass;
};

struct Store {
    std::vector<Server> list;
    std::string current;  // id of the server in use, "" = none

    Server* find(const std::string& id);
    const Server* find(const std::string& id) const;
    const Server* active() const { return find(current); }
    int indexOf(const std::string& id) const;
    std::string newId() const;
    // Removes a server; if it was the one in use, the next one (or none) becomes current.
    void remove(const std::string& id);
};

bool load(const std::string& path, Store& out);  // false if the file does not exist / is unreadable
bool save(const std::string& path, const Store& s);  // written to a temporary file, then renamed

// "https://user:p%40ss@host:5006/x/" -> url "https://host:5006/x/", user "user", pass "p@ss".
// Adds a trailing '/' and "http://" when no scheme was typed.
// What is wrong with a typed address ("" = looks fine): checked before any connection is tried.
std::string checkAddress(const std::string& typed);
void splitAddress(const std::string& typed, std::string& url, std::string& user, std::string& pass);
// The address the HTTP code uses: account and password put back in, %-encoded.
std::string fullUrl(const Server& s);
// "host:port" (or just "host" for default ports) for showing in the menu.
std::string hostOf(const std::string& url);

// First start of v3: turn the single old address (with account:password inside) into server 1.
bool migrateOld(Store& s, const std::string& oldUrl, const std::string& name);

}  // namespace servers
