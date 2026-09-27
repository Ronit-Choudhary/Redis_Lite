#include "store.h"
#include <mutex>
#include <sstream>

// constructor
Store::Store() : running(true), isLoading(true) {
    loadFromAOF();
    isLoading = false;

    aof_file.open("aof.txt", std::ios::app);
    cleanup_thread = std::thread(&Store::cleanupExpiredKeys, this);
}

Store::~Store() {
    running = false;
    if (cleanup_thread.joinable()) cleanup_thread.join();
    if (aof_file.is_open()) aof_file.close();
}

// LRU

void Store::touchLRU(const std::string& key) {
    auto it = lru_map.find(key);

    if (it != lru_map.end()) {
        lru_list.erase(it->second);
    }

    lru_list.push_front(key);
    lru_map[key] = lru_list.begin();
}

size_t Store::estimateSize(const std::string& key, const Value& val) {
    size_t size = key.size();

    if (std::holds_alternative<std::string>(val.data)) {
        size += std::get<std::string>(val.data).size();
    }
    else if (std::holds_alternative<std::vector<std::string>>(val.data)) {
        for (auto& v : std::get<std::vector<std::string>>(val.data)) {
            size += v.size();
        }
    }
    else if (std::holds_alternative<std::unordered_set<std::string>>(val.data)) {
        for (auto& v : std::get<std::unordered_set<std::string>>(val.data)) {
            size += v.size();
        }
    }
    else if (std::holds_alternative<std::unordered_map<std::string, std::string>>(val.data)) {
        for (auto& [k, v] : std::get<std::unordered_map<std::string, std::string>>(val.data)) {
            size += k.size() + v.size();
        }
    }

    return size;
}

void Store::removeKeyInternal(const std::string& key) {
    auto it = data.find(key);
    if (it == data.end()) return;

    current_memory -= estimateSize(key, it->second);

    data.erase(it);

    auto lit = lru_map.find(key);
    if (lit != lru_map.end()) {
        lru_list.erase(lit->second);
        lru_map.erase(lit);
    }
}

void Store::evictIfNeeded() {
    while (current_memory > max_memory && !lru_list.empty()) {
        std::string lru_key = lru_list.back();
        removeKeyInternal(lru_key);
    }
}


// ---------------- AOF ----------------

void Store::appendToAOF(const std::string& command) {
    if (isLoading) return; 
    std::lock_guard<std::mutex> lock(aof_mutex);
    if (aof_file.is_open()) {
        aof_file << command << "\n";
        aof_file.flush();
    }
}

void Store::loadFromAOF() {
    std::ifstream infile("aof.txt");
    std::string line;

    while (std::getline(infile, line)) {
        std::stringstream ss(line);

        std::string cmd;
        ss >> cmd;

        if (cmd == "SET") {
            std::string key, value;
            ss >> key >> value;

            std::string ex;
            ss >> ex;

            if (ex == "EX") {
                int ttl;
                ss >> ttl;
                set(key, value, ttl);
            } else {
                set(key, value, std::nullopt);
            }
        }
        else if (cmd == "DEL") {
            std::string key;
            ss >> key;
            del(key);
        }
        else if (cmd == "LPUSH") {
            std::string key;
            ss >> key;

            std::vector<std::string> values;
            std::string val;

            while (ss >> val) {
                values.push_back(val);
            }

            lpush(key, values);
        }
        else if (cmd == "LPOP") {
            std::string key;
            ss >> key;

            lpop(key);
        }
        else if (cmd == "RPUSH") {
            std::string key;
            ss >> key;

            std::vector<std::string> values;
            std::string val;

            while (ss >> val) {
                values.push_back(val);
            }

            rpush(key, values);
        }
        else if (cmd == "RPOP") {
            std::string key;
            ss >> key;

            rpop(key);
        }
        else if (cmd == "SADD") {
            std::string key;
            ss >> key;

            std::vector<std::string> values;
            std::string val;

            while (ss >> val) {
                values.push_back(val);
            }

            sadd(key, values);
        }
        else if (cmd == "SREM") {
            std::string key;
            ss >> key;

            std::vector<std::string> values;
            std::string val;

            while (ss >> val) {
                values.push_back(val);
            }

            srem(key, values);
        }
        else if (cmd == "HSET") {
            std::string key, field, value;

            ss >> key >> field >> value;

            hset(key, field, value);
        }
        else if (cmd == "HDEL") {
            std::string key;
            ss >> key;

            std::vector<std::string> fields;
            std::string f;

            while (ss >> f) {
                fields.push_back(f);
            }

            hdel(key, fields);
        }

    }
}

// ---------------- CLEANUP ----------------

void Store::cleanupExpiredKeys() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::unique_lock lock(mutex);
        auto now = std::chrono::steady_clock::now();

        for (auto it = data.begin(); it != data.end(); ) {
            if (it->second.expiry.has_value() &&
                now > *(it->second.expiry)) {

                std::string key = it->first;
                ++it;
                removeKeyInternal(key); // 🔥 FIXED

            } else {
                ++it;
            }
        }
    }
}
// ---------------- SET ----------------

void Store::set(const std::string& key, const std::string& value, std::optional<int> ttl) {
    {
        std::unique_lock lock(mutex);

        Value v;
        v.data = value;

        if (ttl.has_value()) {
            v.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(*ttl);
        }

        removeKeyInternal(key);

        data[key] = v;

        current_memory += estimateSize(key, v);

        touchLRU(key);

        evictIfNeeded();
    }

    std::string cmd = "SET " + key + " " + value;
    if (ttl.has_value()) {
        cmd += " EX " + std::to_string(*ttl);
    }

    appendToAOF(cmd);
}
// GET 

std::optional<std::string> Store::get(const std::string& key) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);
    if (it == data.end()) return std::nullopt;

    if (it->second.expiry.has_value() &&
        std::chrono::steady_clock::now() > *(it->second.expiry)) {

        removeKeyInternal(key);
        return std::nullopt;
    }

    touchLRU(key);

    if (std::holds_alternative<std::string>(it->second.data)) {
        return std::get<std::string>(it->second.data);
    }

    throw std::runtime_error("WRONGTYPE");
}

// DEL 

bool Store::del(const std::string& key) {
    bool removed = false;

    {
        std::unique_lock lock(mutex);

        if (data.find(key) != data.end()) {
            removeKeyInternal(key);
            removed = true;
        }
    }

    if (removed) appendToAOF("DEL " + key);

    return removed;
}

// ---------------- EXISTS ----------------

bool Store::exists(const std::string& key) {
    std::shared_lock lock(mutex);

    auto it = data.find(key);
    if (it == data.end()) return false;

    if (it->second.expiry.has_value() &&
        std::chrono::steady_clock::now() > *it->second.expiry) {
        return false;
    }

    return true;
}

// ---------------- KEYS ----------------

std::vector<std::string> Store::keys() {
    std::shared_lock lock(mutex);

    std::vector<std::string> result;

    for (auto& [key, val] : data) {
        if (val.expiry.has_value() &&
            std::chrono::steady_clock::now() > *val.expiry) {
            continue;
        }
        result.push_back(key);
    }

    return result;
}

// ================= LIST =================

// LPUSH
int Store::lpush(const std::string& key, const std::vector<std::string>& values) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);

    // 🔹 NEW KEY
    if (it == data.end()) {
        Value v;
        std::vector<std::string> list;

        for (auto& val : values) {
            list.insert(list.begin(), val);
        }

        v.data = list;
        v.expiry = std::nullopt;

        data[key] = v;

        current_memory += estimateSize(key, v);
        touchLRU(key);
        evictIfNeeded();

        std::string cmd = "LPUSH " + key;
        for (auto& val : values) cmd += " " + val;
        appendToAOF(cmd);

        return list.size();
    }

    // 🔹 EXISTING KEY
    auto& val = it->second;

    if (!std::holds_alternative<std::vector<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& list = std::get<std::vector<std::string>>(val.data);

    //  MEMORY BEFORE
    size_t before = estimateSize(key, val);

    for (auto& v : values) {
        list.insert(list.begin(), v);
    }

    //  MEMORY AFTER
    size_t after = estimateSize(key, val);
    current_memory += (after - before);

    //  LRU + EVICTION
    touchLRU(key);
    evictIfNeeded();

    std::string cmd = "LPUSH " + key;
    for (auto& v : values) cmd += " " + v;
    appendToAOF(cmd);

    return list.size();
}

//LPOP

std::optional<std::string> Store::lpop(const std::string& key) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);

    // key not found
    if (it == data.end()) return std::nullopt;

    auto& val = it->second;

    // type check
    if (!std::holds_alternative<std::vector<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& list = std::get<std::vector<std::string>>(val.data);

    // empty list
    if (list.empty()) return std::nullopt;

    //  MEMORY BEFORE
    size_t before = estimateSize(key, val);

    //  pop front
    std::string front = list.front();
    list.erase(list.begin());

    // If list becomes empty → remove key completely
    if (list.empty()) {
        removeKeyInternal(key);  // handles memory + LRU
    } else {
        // MEMORY AFTER
        size_t after = estimateSize(key, val);
        current_memory += (after - before);

        //  LRU update
        touchLRU(key);
    }

    //  eviction (rare but correct)
    evictIfNeeded();

    //  AOF
    appendToAOF("LPOP " + key);

    return front;
}

//RPUSH

int Store::rpush(const std::string& key, const std::vector<std::string>& values) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);

    //  NEW KEY
    if (it == data.end()) {
        Value v;
        std::vector<std::string> list;

        for (auto& val : values) {
            list.push_back(val);
        }

        v.data = list;
        v.expiry = std::nullopt;

        data[key] = v;

        current_memory += estimateSize(key, v);
        touchLRU(key);
        evictIfNeeded();

        std::string cmd = "RPUSH " + key;
        for (auto& val : values) cmd += " " + val;
        appendToAOF(cmd);

        return list.size();
    }

    //  EXISTING KEY
    auto& val = it->second;

    if (!std::holds_alternative<std::vector<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& list = std::get<std::vector<std::string>>(val.data);

    //  MEMORY BEFORE
    size_t before = estimateSize(key, val);

    for (auto& v : values) {
        list.push_back(v);
    }

    //  MEMORY AFTER
    size_t after = estimateSize(key, val);
    current_memory += (after - before);

    //  LRU + EVICTION
    touchLRU(key);
    evictIfNeeded();

    std::string cmd = "RPUSH " + key;
    for (auto& v : values) cmd += " " + v;
    appendToAOF(cmd);

    return list.size();
}
//RPOP

std::optional<std::string> Store::rpop(const std::string& key) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);

    //  key not found
    if (it == data.end()) return std::nullopt;

    auto& val = it->second;

    //  type check
    if (!std::holds_alternative<std::vector<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& list = std::get<std::vector<std::string>>(val.data);

    //  empty list
    if (list.empty()) return std::nullopt;

    //  MEMORY BEFORE
    size_t before = estimateSize(key, val);

    //  remove from back
    std::string back = list.back();
    list.pop_back();

    // if becomes empty → delete key
    if (list.empty()) {
        removeKeyInternal(key);
    } else {
        //  MEMORY AFTER
        size_t after = estimateSize(key, val);
        current_memory += (after - before);

        // LRU update
        touchLRU(key);
    }

    //  eviction
    evictIfNeeded();

    //  AOF
    appendToAOF("RPOP " + key);

    return back;
}

//  LRANGE
std::vector<std::string> Store::lrange(const std::string& key, int start, int end) {
    std::unique_lock lock(mutex);  // changed from shared_lock

    auto it = data.find(key);
    if (it == data.end()) return {};

    auto& val = it->second;

    if (!std::holds_alternative<std::vector<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    // LRU update (IMPORTANT)
    touchLRU(key);

    const auto& list = std::get<std::vector<std::string>>(val.data);
    int n = list.size();

    if (start < 0) start = n + start;
    if (end < 0) end = n + end;

    if (start < 0) start = 0;
    if (end >= n) end = n - 1;

    if (start > end || start >= n) return {};

    std::vector<std::string> result;

    for (int i = start; i <= end; i++) {
        result.push_back(list[i]);
    }

    return result;
}

int Store::llen(const std::string& key) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);

    if (it == data.end()) return 0;

    auto& val = it->second;

    if (!std::holds_alternative<std::vector<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    //  LRU update (IMPORTANT)
    touchLRU(key);

    const auto& list = std::get<std::vector<std::string>>(val.data);

    return list.size();
}

// ================SET==================
int Store::sadd(const std::string& key, const std::vector<std::string>& values) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);

    //  NEW KEY
    if (it == data.end()) {
        Value v;
        std::unordered_set<std::string> s;

        int added = 0;
        for (auto& val : values) {
            if (s.insert(val).second) added++;
        }

        v.data = s;
        v.expiry = std::nullopt;

        data[key] = v;

        current_memory += estimateSize(key, v);
        touchLRU(key);
        evictIfNeeded();

        std::string cmd = "SADD " + key;
        for (auto& val : values) cmd += " " + val;
        appendToAOF(cmd);

        return added;
    }

    //  EXISTING KEY
    auto& val = it->second;

    if (!std::holds_alternative<std::unordered_set<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& s = std::get<std::unordered_set<std::string>>(val.data);

    //  MEMORY BEFORE
    size_t before = estimateSize(key, val);

    int added = 0;
    for (auto& v : values) {
        if (s.insert(v).second) added++;
    }

    //  MEMORY AFTER
    size_t after = estimateSize(key, val);
    current_memory += (after - before);

    //  LRU + EVICTION
    touchLRU(key);
    evictIfNeeded();

    std::string cmd = "SADD " + key;
    for (auto& v : values) cmd += " " + v;
    appendToAOF(cmd);

    return added;
}

//SREM

int Store::srem(const std::string& key, const std::vector<std::string>& values) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);
    if (it == data.end()) return 0;

    auto& val = it->second;

    if (!std::holds_alternative<std::unordered_set<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& s = std::get<std::unordered_set<std::string>>(val.data);

    // MEMORY BEFORE
    size_t before = estimateSize(key, val);

    int removed = 0;
    for (auto& v : values) {
        if (s.erase(v)) removed++;
    }

    if (removed == 0) return 0;

    //  If empty → delete key
    if (s.empty()) {
        removeKeyInternal(key);
    } else {
        size_t after = estimateSize(key, val);
        current_memory += (after - before);
        touchLRU(key);
    }

    evictIfNeeded();

    std::string cmd = "SREM " + key;
    for (auto& v : values) cmd += " " + v;
    appendToAOF(cmd);

    return removed;
}

//SMEMBERs
std::vector<std::string> Store::smembers(const std::string& key) {
    std::unique_lock lock(mutex);

    auto it = data.find(key);
    if (it == data.end()) return {};

    auto& val = it->second;

    if (!std::holds_alternative<std::unordered_set<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    // LRU update
    touchLRU(key);

    const auto& s = std::get<std::unordered_set<std::string>>(val.data);

    return std::vector<std::string>(s.begin(), s.end());
}
//SISMEMBER
bool Store::sismember(const std::string& key, const std::string& value) {
    std::unique_lock lock(mutex);  // must be unique_lock (LRU update)

    auto it = data.find(key);

    //  key not found
    if (it == data.end()) return false;

    auto& val = it->second;

    //  type check
    if (!std::holds_alternative<std::unordered_set<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    const auto& s = std::get<std::unordered_set<std::string>>(val.data);

    // LRU update
    touchLRU(key);

    return s.count(value) > 0;
}
//SCARD
int Store::scard(const std::string& key) {
    std::unique_lock lock(mutex);  // must be unique_lock

    auto it = data.find(key);

    // key not found
    if (it == data.end()) return 0;

    auto& val = it->second;

    // type check
    if (!std::holds_alternative<std::unordered_set<std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    const auto& s = std::get<std::unordered_set<std::string>>(val.data);

    // LRU update
    touchLRU(key);

    return s.size();
}

//==============Hashmap================
//HSET
int Store::hset(const std::string& key,
                const std::string& field,
                const std::string& value) {

    std::unique_lock lock(mutex);

    auto it = data.find(key);

    // NEW KEY
    if (it == data.end()) {
        Value v;
        std::unordered_map<std::string, std::string> m;

        m[field] = value;

        v.data = m;
        v.expiry = std::nullopt;

        data[key] = v;

        current_memory += estimateSize(key, v);
        touchLRU(key);
        evictIfNeeded();

        appendToAOF("HSET " + key + " " + field + " " + value);

        return 1;
    }

    auto& val = it->second;

    // TYPE CHECK
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& m = std::get<std::unordered_map<std::string, std::string>>(val.data);

    // MEMORY BEFORE
    size_t before = estimateSize(key, val);

    int added = m.count(field) ? 0 : 1;

    m[field] = value;

    // MEMORY AFTER
    size_t after = estimateSize(key, val);
    current_memory += (after - before);

    // LRU + EVICTION
    touchLRU(key);
    evictIfNeeded();

    appendToAOF("HSET " + key + " " + field + " " + value);

    return added;
}
// HGET
std::optional<std::string> Store::hget(const std::string& key,
                                       const std::string& field) {

    std::unique_lock lock(mutex);  // LRU update

    auto it = data.find(key);

    if (it == data.end()) return std::nullopt;

    auto& val = it->second;

    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& m = std::get<std::unordered_map<std::string, std::string>>(val.data);

    auto fit = m.find(field);

    // LRU update (even if field not found)
    touchLRU(key);

    if (fit == m.end()) return std::nullopt;

    return fit->second;
}

//HDEL
int Store::hdel(const std::string& key,
                const std::vector<std::string>& fields) {

    std::unique_lock lock(mutex);

    auto it = data.find(key);

    // key not found
    if (it == data.end()) return 0;

    auto& val = it->second;

    // TYPE CHECK
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    auto& m = std::get<std::unordered_map<std::string, std::string>>(val.data);

    // MEMORY BEFORE
    size_t before = estimateSize(key, val);

    int removed = 0;

    for (auto& f : fields) {
        if (m.erase(f)) {
            removed++;
        }
    }

    // If map becomes empty → remove key
    if (m.empty()) {
        removeKeyInternal(key);
    } else {
        // MEMORY AFTER
        size_t after = estimateSize(key, val);
        current_memory += (after - before);

        // LRU update
        touchLRU(key);
    }

    // eviction
    evictIfNeeded();

    // AOF
    if (removed > 0) {
        std::string cmd = "HDEL " + key;
        for (auto& f : fields) cmd += " " + f;
        appendToAOF(cmd);
    }

    return removed;
}
//HLEN
int Store::hlen(const std::string& key) {
    std::unique_lock lock(mutex);  // must be unique_lock (LRU)

    auto it = data.find(key);

    // key not found
    if (it == data.end()) return 0;

    auto& val = it->second;

    // type check
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    const auto& m = std::get<std::unordered_map<std::string, std::string>>(val.data);

    //LRU update
    touchLRU(key);

    return m.size();
}
//HGETALL
std::vector<std::pair<std::string, std::string>>
Store::hgetall(const std::string& key) {

    std::unique_lock lock(mutex);  // LRU update requires write lock

    auto it = data.find(key);

    // key not found
    if (it == data.end()) return {};

    auto& val = it->second;

    //type check
    if (!std::holds_alternative<std::unordered_map<std::string, std::string>>(val.data)) {
        throw std::runtime_error("WRONGTYPE");
    }

    const auto& m = std::get<std::unordered_map<std::string, std::string>>(val.data);

    // LRU update
    touchLRU(key);

    std::vector<std::pair<std::string, std::string>> result;

    for (auto& [k, v] : m) {
        result.push_back({k, v});
    }

    return result;
}
    void Store::flushAll() {
        std::unique_lock lock(mutex);

        data.clear();

        // if you have LRU
        lru_list.clear();
        lru_map.clear();

        current_memory = 0;

        appendToAOF("FLUSHALL");
    }

    size_t Store::getMaxMemory() const {
        return max_memory;
    }

    size_t Store::getCurrentMemory() const {
        return current_memory;
    }