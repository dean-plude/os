/* stltest.exe — the C++ standard library through msvcp140.dll and its
 * satellites (msvcp140_1, msvcp140_atomic_wait), built against Microsoft's
 * STL headers as Visual Studio builds a program */
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <locale>
#include <map>
#include <memory_resource>
#include <mutex>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

static int pass, fail;
#define CHECK(what, cond)                                                   \
    do {                                                                    \
        if (cond)                                                           \
            pass++;                                                         \
        else {                                                              \
            fail++;                                                         \
            std::printf("FAIL: %s (line %d)\n", what, __LINE__);            \
        }                                                                   \
    } while (0)

static void strings_and_containers()
{
    std::string s = "Nova";
    s += "OS";
    s.insert(0, "Hello, ");
    CHECK("string", s == "Hello, NovaOS" && s.find("Nova") == 7);
    std::wstring w = L"wide";
    CHECK("wstring", w.size() == 4 && w[1] == L'i');
    std::vector<int> v{5, 3, 9, 1};
    std::sort(v.begin(), v.end());
    CHECK("vector sort", v == (std::vector<int>{1, 3, 5, 9}));
    std::map<std::string, int> m{{"b", 2}, {"a", 1}};
    CHECK("map", m.begin()->first == "a" && m["b"] == 2);
}

static void streams()
{
    std::ostringstream o;
    o << 42 << ' ' << 3.5 << ' ' << std::hex << 255;
    CHECK("ostringstream", o.str() == "42 3.5 ff");
    std::istringstream i("17 2.25 word");
    int n;
    double d;
    std::string word;
    i >> n >> d >> word;
    CHECK("istringstream", n == 17 && d == 2.25 && word == "word");
    std::ostringstream c;
    c.imbue(std::locale::classic());
    c << std::fixed;
    c.precision(2);
    c << 1234.5;
    CHECK("locale numbers", c.str() == "1234.50");
    CHECK("ctype", std::use_facet<std::ctype<char>>(std::locale::classic()).toupper('q') == 'Q');
    std::cout << "stltest: cout works" << std::endl;
    CHECK("cout", std::cout.good());
}

static void exceptions()
{
    std::vector<int> v(3);
    bool caught = false;
    try {
        v.at(10) = 1;
    } catch (const std::out_of_range &e) {
        caught = e.what() != nullptr;
    }
    CHECK("out_of_range from at()", caught);
    std::exception_ptr p;
    try {
        throw std::runtime_error("boom");
    } catch (...) {
        p = std::current_exception();
    }
    std::string msg;
    try {
        std::rethrow_exception(p);
    } catch (const std::runtime_error &e) {
        msg = e.what();
    }
    CHECK("exception_ptr", msg == "boom");
    std::error_code ec = std::make_error_code(std::errc::no_such_file_or_directory);
    CHECK("system_category", ec.value() == ENOENT && !ec.message().empty());
}

static void threads()
{
    std::mutex mu;
    std::condition_variable cv;
    int total = 0, done = 0;
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&, t] {
            for (int k = 0; k < 1000; k++) {
                std::lock_guard<std::mutex> g(mu);
                total += t + 1;
            }
            std::lock_guard<std::mutex> g(mu);
            done++;
            cv.notify_one();
        });
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return done == 4; });
    }
    for (auto &t : ts) t.join();
    CHECK("threads + mutex + condition_variable", total == 10000);
    auto f = std::async(std::launch::async, [] { return 6 * 7; });
    CHECK("async/future", f.get() == 42);
    std::shared_mutex sm;
    {
        std::shared_lock<std::shared_mutex> r1(sm), r2(sm);
    }
    std::unique_lock<std::shared_mutex> wl(sm);
    CHECK("shared_mutex", wl.owns_lock());
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK("sleep_for + steady_clock", std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(15));
}

static void atomic_wait()                     /* msvcp140_atomic_wait.dll */
{
    std::atomic<int> flag{0};
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        flag.store(1);
        flag.notify_one();
    });
    flag.wait(0);
    t.join();
    CHECK("atomic wait/notify", flag.load() == 1);
}

static void pmr()                             /* msvcp140_1.dll */
{
    char buf[256];
    std::pmr::monotonic_buffer_resource res(buf, sizeof buf);
    std::pmr::vector<int> v(&res);
    for (int k = 0; k < 10; k++) v.push_back(k);
    CHECK("pmr vector", v.size() == 10 && v[9] == 9);
    CHECK("pmr default resource", std::pmr::get_default_resource() != nullptr);
}

static void files()
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "stltest-dir";
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK("create_directories", fs::create_directories(dir));
    {
        std::ofstream out(dir / "a.txt");
        out << "line one\nline two\n";
    }
    std::ifstream in(dir / "a.txt");
    std::string l1, l2;
    std::getline(in, l1);
    std::getline(in, l2);
    in.close();
    CHECK("fstream", l1 == "line one" && l2 == "line two");
    CHECK("file_size", fs::file_size(dir / "a.txt") == 18);
    int n = 0;
    for (auto &e : fs::directory_iterator(dir)) n += e.path().filename() == "a.txt";
    CHECK("directory_iterator", n == 1);
    CHECK("remove_all", fs::remove_all(dir) == 2 && !fs::exists(dir));
}

static void text()
{
    char b[32];
    auto r = std::to_chars(b, b + sizeof b, 0.1);
    CHECK("to_chars", std::string(b, r.ptr) == "0.1");
    double d = 0;
    std::from_chars("2.5e3", "2.5e3" + 5, d);
    CHECK("from_chars", d == 2500.0);
    CHECK("format", std::format("{}-{:>4}-{:.2f}", 7, "ab", 3.14159) == "7-  ab-3.14");
    std::regex re("(\\w+)@(\\w+)\\.org");
    std::smatch m;
    std::string mail = "mail nova@example.org now";
    CHECK("regex", std::regex_search(mail, m, re) && m[2] == "example");
}

int main()
{
    strings_and_containers();
    streams();
    exceptions();
    threads();
    atomic_wait();
    pmr();
    files();
    text();
    std::printf("stltest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
