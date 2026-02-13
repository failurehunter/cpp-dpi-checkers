// dpi_check.cpp
// build: g++ -std=c++23 dpi_check.cpp -lcurl -pthread -O2 -o dpi_check

#include <curl/curl.h>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <format>
#include <thread>
#include <vector>
#include <atomic>

using namespace std::chrono;

static const size_t OK_THRESHOLD_BYTES = 64 * 1024;
static long TIMEOUT_MS = 5000;

struct Test {
    std::string id;
    std::string provider;
    std::string url;
    int times{};
};

struct Result {
    std::string id;
    std::string provider;
    long http_code = 0;
    std::atomic<size_t> received{0};
    std::string status;
    std::string detail;
    double elapsed_ms = 0.0;
    bool aborted_by_threshold = false;
};

std::mutex log_mtx;

void log_write(const std::string& s, bool newline) {
    std::lock_guard<std::mutex> lk(log_mtx);
    std::cout << "\r" << s << "\033[K";
    if (newline) {
        std::cout << std::endl;
    } else {
        std::cout << std::flush;
    }
}

void log_line(const std::string& s) { log_write(s, true); }
void log_inline(const std::string& s) { log_write(s, false); }

std::string currentTimestamp() {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1s;

    return std::format(
        "[{:%H:%M:%S}.{:03}]",
        floor<seconds>(now),
        ms.count()
    );
}



void log_start(const std::string& id, const std::string& text) {
    std::string line = std::format("{} {} - {}", currentTimestamp(), id, text);
    log_inline(line);
}

void log_msg(const std::string& prefix, const std::string& msg) {
    std::lock_guard<std::mutex> lk(log_mtx);
    std::string timestamp = currentTimestamp();
    std::string output;

    if (!prefix.empty()) {
        output = std::format("{} {} - {}\n", timestamp, prefix, msg);
    } else {
        output = std::format("{} {}\n", timestamp, msg);
    }

    std::cout << output << std::flush;
}


void log_result(const Result& res) {
    std::string timestamp = currentTimestamp();
    std::string status = res.status;
    if (status.size() > 20) status = status.substr(0, 17) + "...";

    std::string output = std::format(
        "{} {:<15} {:>4} {:>8} {:>10.1f} ms {:<17} {}",
        timestamp,
        res.id,
        res.http_code,
        res.received.load(),
        res.elapsed_ms,
        status,
        res.detail
    );

    log_line(output);
}

static size_t curlWriteToString(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

bool fetchHtml(const std::string& url, std::string& html) { 
    CURL* curl = curl_easy_init(); 
    if (!curl) return false; 
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str()); 
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString); 
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html); 
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); 
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0"); 
    CURLcode res = curl_easy_perform(curl); 
    curl_easy_cleanup(curl); return res == CURLE_OK; 
}

static inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    size_t b = s.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}


std::string extractTestSuiteArray(const std::string& html) {
    const std::string marker = "const TEST_SUITE";
    auto pos = html.find(marker);
    if (pos == std::string::npos) return {};

    // найти первую '[' после маркера
    pos = html.find('[', pos);
    if (pos == std::string::npos) return {};

    size_t depth = 0;
    size_t start = pos;

    for (size_t i = pos; i < html.size(); ++i) {
        if (html[i] == '[') depth++;
        else if (html[i] == ']') {
            depth--;
            if (depth == 0) {
                return html.substr(start, i - start + 1);
            }
        }
    }
    return {};
}

bool parseObject(const std::string& objText, Test& t) {
    auto getString = [&](const std::string& key) -> std::string {
        std::string pat = key + ":";
        size_t p = objText.find(pat);
        if (p == std::string::npos) return "";
        p = objText.find('"', p);
        if (p == std::string::npos) return "";
        size_t q = objText.find('"', p + 1);
        if (q == std::string::npos) return "";
        return objText.substr(p + 1, q - (p + 1));
    };

    auto getInt = [&](const std::string& key) -> int {
        std::string pat = key + ":";
        size_t p = objText.find(pat);
        if (p == std::string::npos) return 0;
        p += pat.size();
        while (p < objText.size() && isspace(objText[p])) p++;
        size_t q = p;
        while (q < objText.size() && isdigit(objText[q])) q++;
        return std::stoi(objText.substr(p, q - p));
    };

    t.id       = getString("id");
    t.provider = getString("provider");
    t.url      = getString("url");
    t.times    = getInt("times");

    return !t.id.empty();
}

void parseTestSuiteVector(const std::string& arrayText, std::vector<Test>& out) {
    size_t i = 0;
    while (i < arrayText.size()) {
        auto p = arrayText.find('{', i);
        if (p == std::string::npos) break;

        int depth = 0;
        size_t start = p;
        for (size_t j = p; j < arrayText.size(); ++j) {
            if (arrayText[j] == '{') depth++;
            else if (arrayText[j] == '}') {
                depth--;
                if (depth == 0) {
                    std::string obj = arrayText.substr(start, j - start + 1);
                    Test t;
                    if (parseObject(obj, t)) {
                        out.push_back(std::move(t));
                    }
                    i = j + 1;
                    break;
                }
            }
        }
        i++;
    }
}

void loadTestSuiteFromUrl(std::vector<Test>& tests, const std::string& url) {
    std::string html;
    if (!fetchHtml(url, html)) return;

    std::string arr = extractTestSuiteArray(html);
    if (arr.empty()) return;

    tests.clear();
    parseTestSuiteVector(arr, tests);
}


static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t real = size * nmemb;
    Result* res = static_cast<Result*>(userdata);
    res->received += real;
    return real;
}

static int xferinfo_cb(void* p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    Result* res = static_cast<Result*>(p);
    if (res->received >= OK_THRESHOLD_BYTES) {
        res->aborted_by_threshold = true;
        return 1;
    }
    return 0;
}

void worker(const Test& t, int idx, long timeout_ms) {
    Result res;
    res.id = (t.times > 1) ? (t.id + "@" + std::to_string(idx)) : t.id;
    res.provider = t.provider;

    auto t_start = steady_clock::now();

    CURL* curl = curl_easy_init();
    if (!curl) {
        log_msg(res.id, "curl_easy_init failed");
        return;
    }

    std::string url = t.url;
    if (url.find('?') == std::string::npos) {
        url += "?t=" + std::to_string((unsigned long)std::hash<std::string>{}(res.id + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    } else {
        url += "&t=" + std::to_string((unsigned long)std::hash<std::string>{}(res.id + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &res);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, br");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeout_ms / 1000);

    log_start(res.id, "Starting request -> " + url);
    CURLcode rc = curl_easy_perform(curl);

    auto t_end = steady_clock::now();
    res.elapsed_ms = duration_cast<duration<double, std::milli>>(t_end - t_start).count();

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.http_code);

    switch (rc) {
    case CURLE_OK:
        if (res.received >= OK_THRESHOLD_BYTES) {
            res.status = "Not detected ✅";
            res.detail = "Received >= threshold";
        } else {
            res.status = "Possibly detected ⚠️";
            res.detail = "Stream ended, data too small";
        }
        break;

    case CURLE_OPERATION_TIMEDOUT:
        if (res.received == 0) {
            res.status = "Detected* ❗️";
            res.detail = "Timeout with zero bytes (likely connection blocked)";
        } else {
            res.status = "Detected ❗️";
            res.detail = "Timeout after partial data (read blocked)";
        }
        break;

    case CURLE_ABORTED_BY_CALLBACK:
        if (res.aborted_by_threshold) {
            res.status = "Not detected ✅";
            res.detail = "Early abort: threshold reached";
        } else {
            res.status = "Detected ❗️";
            res.detail = "Unexpected abort before threshold";
        }
        break;

    default:
        {
            std::ostringstream ss;
            ss << "curl_error=" << rc << " (" << curl_easy_strerror(rc) << ")";
            res.status = "Failed to complete detection ⚠️";
            res.detail = ss.str();
        }
        break;
}

{
    std::ostringstream summary;
    summary << "HTTP " << res.http_code
            << " | bytes=" << res.received.load()
            << " | elapsed=" << std::fixed << std::setprecision(1) << res.elapsed_ms << " ms"
            << " | result=" << res.status
            << " | " << res.detail;

    log_result(res);
}
    curl_easy_cleanup(curl);

}

int main(int argc, char** argv) {
std::vector<Test> tests = {};

if (argc > 1) {
        try {
            TIMEOUT_MS = std::stol(argv[1]);
        } catch (...) {}
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    loadTestSuiteFromUrl(tests, "https://raw.githubusercontent.com/hyperion-cs/dpi-checkers/refs/heads/main/ru/tcp-16-20/suite.json");

    std::vector<std::thread> workers;
    for (const auto& t : tests) {
        for (int i = 0; i < t.times; ++i) {
            workers.emplace_back(worker, t, i, TIMEOUT_MS);
        }
    }

    for (auto &th : workers) {
        if (th.joinable()) th.join();
    }

    curl_global_cleanup();
    log_msg("MAIN", "All tests finished.");
    return 0;
}
