// dpi_check.cpp
// g++ -std=c++17 dpi_check.cpp -lcurl -pthread -O2 -o dpi_check

#include <curl/curl.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <regex>
#include <nlohmann/json.hpp>

using namespace std::chrono;

static const size_t OK_THRESHOLD_BYTES = 64 * 1024; // 64 KB
static long TIMEOUT_MS = 5000;

struct Test {
    std::string id;
    std::string provider;
    std::string url;
    int times;
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

static void safe_localtime(const time_t* t, std::tm* out) {
#if defined(_WIN32)
    localtime_s(out, t);   // Windows version: note argument order
#else
    localtime_r(t, out);   // POSIX version
#endif
}


void log_inline(const std::string& s) {
    std::lock_guard<std::mutex> lk(log_mtx);
    std::cout << "\r" << s << "\033[K" << std::flush;
}

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(log_mtx);
    std::cout << "\r" << s << "\033[K" << std::endl;
}




void log_start(const std::string& id, const std::string& text) {
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    safe_localtime(&t, &tm);

    std::ostringstream oss;
    oss << "[" << std::put_time(&tm, "%H:%M:%S") << "." 
        << std::setw(3) << std::setfill('0') << ms << "] "
        << id << " - " << text;

    log_inline(oss.str());     // <── не добавляем новой строки
}


void log_msg(const std::string &prefix, const std::string &msg) {
    std::lock_guard<std::mutex> lk(log_mtx);
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    safe_localtime(&t, &tm);
    std::ostringstream oss;
    oss << "[" << std::put_time(&tm, "%H:%M:%S") << "." << std::setw(3) << std::setfill('0') << ms << "] ";
    if (!prefix.empty()) oss << prefix << " - ";
    oss << msg << "\n";
    std::cout << oss.str() << std::flush;
}

void log_result(const Result& res) {
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    safe_localtime(&t, &tm);

    std::ostringstream oss;

    // Время: [HH:MM:SS.mmm]
    oss << "[" << std::put_time(&tm, "%H:%M:%S") << "." 
        << std::setw(3) << std::setfill('0') << ms << "] "
        << std::setfill(' ');

    // Форматируем колонки с фиксированной шириной
    // ID (15 символов, слева)
    oss << std::left << std::setw(15) << res.id << " ";

    // HTTP код (4 символа, справа)
    oss << std::right << std::setw(4) << res.http_code << " ";

    // Bytes (8 символов, справа)
    oss << std::right << std::setw(8) << res.received.load() << " ";

    // Elapsed ms (10 символов, справа, с 1 знаком после запятой)
    oss << std::right << std::setw(10) << std::fixed << std::setprecision(1) << res.elapsed_ms << " ms ";

    // Result (20 символов, слева, если длиннее — обрезаем)
    std::string status = res.status;
    if (status.size() > 20) status = status.substr(0, 17) + "...";
    oss << std::left << std::setw(17) << status << " ";

    // Detail (оставшееся пространство)
    oss << res.detail;

    log_line(oss.str());
}


// Callback libcurl для загрузки HTML в std::string
static size_t curlWriteToString(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

// Функция загрузки HTML по URL
bool fetchHtml(const std::string& url, std::string& html) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

// Преобразование JS-массива в JSON (простейшие замены)
std::string jsArrayToJson(const std::string& js) {
    std::string json = js;
    // Добавить кавычки вокруг ключей (ключ: value -> "ключ": value)
    json = std::regex_replace(json, std::regex(R"((\{|,)\s*([a-zA-Z0-9_]+)\s*:)"), "$1\"$2\":");
    // Заменить одинарные кавычки на двойные
    json = std::regex_replace(json, std::regex("'"), "\"");
    // Убрать лишние запятые перед закрывающими скобками
    json = std::regex_replace(json, std::regex(",(\\s*[}\\]])"), "$1");
    return json;
}

// Функция загрузки и парсинга TEST_SUITE из HTML
void loadTestSuiteFromUrl(std::vector<Test>& tests, const std::string& url) {
    std::string html;
    if (!fetchHtml(url, html)) {
        // Не удалось скачать — не менять tests
        return;
    }

    std::regex re(R"(const\s+TEST_SUITE\s*=\s*(\[[\s\S]*?\]);)");
    std::smatch match;
    if (!std::regex_search(html, match, re)) {
        // Не нашли TEST_SUITE — не менять tests
        return;
    }

    std::string jsArray = match[1];
    std::string jsonText = jsArrayToJson(jsArray);

    try {
        auto j = nlohmann::json::parse(jsonText);
        std::vector<Test> tmpTests;
        for (const auto& item : j) {
            Test t;
            t.id = item.at("id").get<std::string>();
            t.provider = item.at("provider").get<std::string>();
            t.url = item.at("url").get<std::string>();
            t.times = item.at("times").get<int>();
            tmpTests.push_back(std::move(t));
        }
        tests = std::move(tmpTests); // Только после успешного парсинга заменяем основной вектор
    } catch (...) {
        // Ошибка парсинга — не менять tests
        return;
    }
}

// libcurl write callback: receives chunks; just count bytes.
static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t real = size * nmemb;
    Result* res = static_cast<Result*>(userdata);
    res->received += real;
    // Always accept data (return real). We will abort via xferinfo when threshold reached.
    return real;
}

// xferinfo callback: called often; we can return non-zero to abort.
static int xferinfo_cb(void* p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    Result* res = static_cast<Result*>(p);
    if (res->received >= OK_THRESHOLD_BYTES) {
        res->aborted_by_threshold = true;
        return 1; // abort transfer
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

    // set URL (add unique query param to avoid caching if desired)
    std::string url = t.url;
    if (url.find('?') == std::string::npos) {
        url += "?t=" + std::to_string((unsigned long)std::hash<std::string>{}(res.id + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    } else {
        url += "&t=" + std::to_string((unsigned long)std::hash<std::string>{}(res.id + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // mirror JS redirect: "manual"
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &res);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms); // overall timeout
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, br");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeout_ms / 1000);

    // perform
    log_start(res.id, "Starting request -> " + url);
    CURLcode rc = curl_easy_perform(curl);

    auto t_end = steady_clock::now();
    res.elapsed_ms = duration_cast<duration<double, std::milli>>(t_end - t_start).count();

    // get HTTP response code if available
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


// log summary
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
    // Simple test-suite: можно расширять / читать из файла / CLI
std::vector<Test> tests = {
    {"US.CF-01", "Cloudflare", "https://cdn.cookielaw.org/scripttemplates/202501.2.0/otBannerSdk.js", 1},
    {"US.CF-02", "Cloudflare", "https://genshin.jmp.blue/characters/all#", 1},
    {"US.CF-03", "Cloudflare", "https://api.frankfurter.dev/v1/2000-01-01..2002-12-31", 1},

    {"US.DO-01", "DigitalOcean", "https://genderize.io/", 2},

    {"DE.HE-01", "Hetzner", "https://j.dejure.org/jcg/doctrine/doctrine_banner.webp", 1},
    {"FI.HE-01", "Hetzner", "https://tcp1620-01.dubybot.live/1MB.bin", 1},
    {"FI.HE-02", "Hetzner", "https://tcp1620-02.dubybot.live/1MB.bin", 1},
    {"FI.HE-03", "Hetzner", "https://tcp1620-05.dubybot.live/1MB.bin", 1},
    {"FI.HE-04", "Hetzner", "https://tcp1620-06.dubybot.live/1MB.bin", 1},

    {"FR.OVH-01", "OVH", "https://eu.api.ovh.com/console/rapidoc-min.js", 1},
    {"FR.OVH-02", "OVH", "https://ovh.sfx.ovh/10M.bin", 1},

    {"SE.OR-01", "Oracle", "https://oracle.sfx.ovh/10M.bin", 1},

    {"DE.AWS-01", "AWS", "https://tms.delta.com/delta/dl_anderson/Bootstrap.js", 1},
    {"US.AWS-01", "AWS", "https://d1rbsgppyrdqq4.cloudfront.net/s3fs-public/c7/Konyukhov_asu_0010N_23739.pdf", 1},

    {"US.GC-01", "Google Cloud", "https://api.usercentrics.eu/gvl/v3/en.json", 1},

    {"US.FST-01", "Fastly", "https://openoffice.apache.org/images/blog/rejected.png", 1},
    {"US.FST-02", "Fastly", "https://www.juniper.net/etc.clientlibs/juniper/clientlibs/clientlib-site/resources/fonts/lato/Lato-Regular.woff2", 1},

    {"PL.AKM-01", "Akamai", "https://www.lg.com/lg5-common-gp/library/jquery.min.js", 1},
    {"PL.AKM-02", "Akamai", "https://media-assets.stryker.com/is/image/stryker/gateway_1?$max_width_1410$", 1},

    {"US.CDN77-01", "CDN77", "https://www.winkgo.com/wp-content/themes/mts_sociallyviral/fonts/fontawesome-webfont.woff2", 1},

    {"DE.CNTB-01", "Contabo", "https://cloudlets.io/wp-content/themes/Avada/includes/lib/assets/fonts/fontawesome/webfonts/fa-solid-900.woff2", 1},

    {"FR.SW-01", "Scaleway", "https://renklisigorta.com.tr/teklif-al", 1},

    {"US.CNST-01", "Constant", "https://cdn.xuansiwei.com/common/lib/font-awesome/4.7.0/fontawesome-webfont.woff2?v=4.7.0", 1}
};


if (argc > 1) {
        try {
            TIMEOUT_MS = std::stol(argv[1]);
        } catch (...) {}
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Пытаемся загрузить TEST_SUITE с URL (пример URL — поменяй под нужный)
    loadTestSuiteFromUrl(tests, "https://raw.githubusercontent.com/hyperion-cs/dpi-checkers/refs/heads/main/ru/tcp-16-20/index.html");

    // Далее запускаем воркеры, используя либо дефолтные тесты, либо загруженные

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