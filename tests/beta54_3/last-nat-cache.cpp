// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/hle/service/set/last_nat_cache.h"
#include <cassert>
#include <iostream>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path path = std::filesystem::path(argv[1]) / "test-last-nat-cache.txt";
    Service::Set::LastNatCache cache{path};
    assert(cache.Display(11) == "--");
    assert(!cache.Complete(0, 11)); // Completion without this test's begin cannot overwrite.
    for (unsigned nat = 0; nat < 5; ++nat) {
        cache.Begin(11);
        assert(cache.Complete(nat, 11));
        Service::Set::LastNatCache reloaded{path};
        assert(reloaded.Display(11) == std::string(1, "ABCDF"[nat]));
        assert(reloaded.Display(22) == "retest");
        assert(reloaded.Display(std::nullopt) == "--");
    }
    cache.Begin(11);
    assert(!cache.Complete(0, 22)); // Network changed during the test.
    assert(cache.Display(11) == "F");
    cache.Begin(11);
    assert(!cache.Complete(5, 11));
    assert(cache.Display(11) == "F");
    { std::ofstream out(path); out << "CITRON_NAT_1 11 999\n"; }
    assert(cache.Display(11) == "--");
    { std::ofstream out(path); out << "CITRON_NAT_1 11 0 extra\n"; }
    assert(cache.Display(11) == "--");
    { std::ofstream out(path); out << "CITRON_NAT_1 11"; }
    assert(cache.Display(11) == "--");
    std::filesystem::remove(path);
    std::cout << "PASS: native NAT mapping, completed-test gate, restart persistence, changed network, offline, invalid and corrupt records\n";
}
