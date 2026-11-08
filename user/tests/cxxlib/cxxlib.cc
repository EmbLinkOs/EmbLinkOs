/* user/tests/cxxlib/cxxlib.cc -> /data/apps/cxxlib/cxxlib.elf -- the C++
 * standard library on EmbLinkOS, past what cxxdemo proves.
 *
 * cxxdemo is the floor: constructors, new/delete, guards, destructors at
 * exit, and nothing that drags in a library. This is the rest, the parts a
 * real C++ program reaches for first and that each need something of the
 * OS under them:
 *
 *   1. exceptions        a throw through frames that do not catch it,
 *                        destructors on the way (the unwinder reading
 *                        .eh_frame -- newlib-body.ld's brackets), rethrow,
 *                        catch (...), std::exception_ptr
 *   2. strings           std::string, std::to_string, std::stoi
 *   3. containers        vector + sort, map, unordered_map, set
 *   4. streams           std::cout, std::ostringstream with manipulators
 *   5. smart pointers    unique_ptr, shared_ptr and its deleter
 *   6. std::format
 *   7. threads           std::thread, std::mutex, join (the C library's
 *                        threads, the kernel's thread_create)
 *   8. the filesystem    std::filesystem over the C library's directories
 *
 * Built only under LIBC=emblibc, by EmbCC against lib/libcxx. Each check
 * prints a line; the exit status is the number that failed. */
#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static int g_fails;

static void check(const char *what, bool ok)
{
    std::cout << "  [" << (ok ? "ok" : "FAIL") << "] " << what << '\n';
    if (!ok)
        g_fails++;
}

/* ---- 1. exceptions -------------------------------------------------- */

static int g_live;
struct Guard {
    Guard() { g_live++; }
    ~Guard() { g_live--; }
};

[[gnu::noinline]] static int deep(int n)
{
    Guard g;
    if (n == 0)
        throw std::runtime_error("from the bottom");
    return deep(n - 1) + 1;
}

[[gnu::noinline]] static void rethrows()
{
    try {
        deep(3);
    } catch (...) {
        throw;
    }
}

static void exceptions()
{
    std::cout << "exceptions:\n";
    bool caught = false;
    try {
        deep(5);
    } catch (const std::exception &e) {
        caught = std::string(e.what()) == "from the bottom";
    }
    check("a throw six frames down is caught by its base class", caught);
    check("every destructor on the way ran", g_live == 0);

    caught = false;
    try {
        rethrows();
    } catch (const std::runtime_error &) {
        caught = true;
    }
    check("throw; inside catch (...) rethrows the same object", caught);

    std::exception_ptr saved;
    try {
        throw std::out_of_range("kept");
    } catch (...) {
        saved = std::current_exception();
    }
    caught = false;
    try {
        std::rethrow_exception(saved);
    } catch (const std::out_of_range &e) {
        caught = std::string(e.what()) == "kept";
    }
    check("std::exception_ptr outlives its catch and rethrows", caught);

    caught = false;
    try {
        std::vector<int> v(3);
        (void)v.at(7);
    } catch (const std::out_of_range &) {
        caught = true;
    }
    check("the library's own throw (vector::at) is catchable", caught);
}

/* ---- 2-6. the library ----------------------------------------------- */

static void library()
{
    std::cout << "strings and containers:\n";
    std::string s = "Emb";
    s += "Link" + std::string("OS");
    check("std::string concatenation and find",
          s == "EmbLinkOS" && s.find("Link") == 3 && s.substr(7) == "OS");
    check("std::to_string and std::stoi round-trip",
          std::stoi(std::to_string(-12345)) == -12345);

    std::vector<int> v = { 5, 3, 9, 1, 7 };
    std::sort(v.begin(), v.end());
    check("std::vector + std::sort", v == std::vector<int>({ 1, 3, 5, 7, 9 }));

    std::map<std::string, int> m;
    for (const char *w : { "b", "a", "c", "a" })
        m[w]++;
    check("std::map keeps its keys in order",
          m.begin()->first == "a" && m["a"] == 2 && m.size() == 3);

    std::unordered_map<int, std::string> u;
    for (int i = 0; i < 1000; i++)
        u[i] = std::to_string(i * i);
    check("std::unordered_map past a rehash", u.size() == 1000 && u[31] == "961");

    std::set<int> st(v.begin(), v.end());
    check("std::set", st.count(7) == 1 && st.count(8) == 0);

    std::cout << "streams:\n";
    std::ostringstream o;
    o << std::setw(5) << 42 << '|' << std::hex << 255 << '|' << std::dec
      << std::fixed << std::setprecision(2) << 3.14159;
    check("std::ostringstream with setw, hex, fixed, setprecision",
          o.str() == "   42|ff|3.14");

    std::cout << "smart pointers:\n";
    int deleted = 0;
    {
        std::shared_ptr<int> a(new int(7), [&](int *p) { deleted++; delete p; });
        std::shared_ptr<int> b = a;
        check("shared_ptr counts its owners", a.use_count() == 2 && *b == 7);
    }
    check("...and runs its deleter once, when the last goes", deleted == 1);
    auto up = std::make_unique<std::string>("unique");
    check("unique_ptr", *up == "unique");

    std::cout << "format:\n";
    check("std::format", std::format("{}-{:x}-{:>4}", 42, 255, "ab") == "42-ff-  ab");
}

/* ---- 7. threads ----------------------------------------------------- */

static void threads()
{
    std::cout << "threads:\n";
    std::mutex mu;
    long total = 0;
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; t++)
        pool.emplace_back([&] {
            for (int i = 0; i < 2000; i++) {
                std::lock_guard<std::mutex> lock(mu);
                total++;
            }
        });
    for (auto &th : pool)
        th.join();
    check("four std::threads, one std::mutex, every increment counted",
          total == 8000);
}

/* ---- 8. the filesystem ---------------------------------------------- */

static void filesystem()
{
    namespace fs = std::filesystem;
    std::cout << "filesystem:\n";
    std::error_code ec;
    fs::path dir = "/data/tmp/cxxlib";
    fs::remove_all(dir, ec);
    check("create_directories", fs::create_directories(dir / "sub", ec) && !ec);
    {
        FILE *f = fopen((dir / "sub" / "f.txt").c_str(), "w");
        if (f) {
            fputs("hello", f);
            fclose(f);
        }
    }
    check("exists and file_size", fs::exists(dir / "sub" / "f.txt") &&
                                     fs::file_size(dir / "sub" / "f.txt") == 5);
    int n = 0;
    for (auto &e : fs::recursive_directory_iterator(dir, ec)) {
        (void)e;
        n++;
    }
    check("recursive_directory_iterator finds the directory and the file", n == 2);
    check("remove_all", fs::remove_all(dir, ec) == 3 && !fs::exists(dir));
}

int main()
{
    std::cout << "cxxlib: the C++ standard library on EmbLinkOS\n";
    exceptions();
    library();
    threads();
    filesystem();
    std::cout << "cxxlib: " << (g_fails ? "FAILURES" : "all passed") << '\n';
    return g_fails;
}
