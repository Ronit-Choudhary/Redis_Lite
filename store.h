#ifndef STORE_H
#define STORE_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <variant>
#include <shared_mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <fstream>
#include <mutex>
#include <list>   

class Store {
private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Value {
        std::variant<
            std::string,
            std::vector<std::string>,
            std::unordered_set<std::string>,
            std::unordered_map<std::string, std::string>
        > data;

        std::optional<TimePoint> expiry;
    };

    std::unordered_map<std::string, Value> data;
    mutable std::shared_mutex mutex;

    //  TTL CLEANUP THREAD
    std::atomic<bool> running;
    bool isLoading;
    std::thread cleanup_thread;
    void cleanupExpiredKeys();

    //  AOF
    std::ofstream aof_file;
    std::mutex aof_mutex;

    void appendToAOF(const std::string& command);
    void loadFromAOF();

    //  MEMORY MANAGEMENT (NEW)
    size_t current_memory = 0;
    size_t max_memory = 100 * 1024 * 1024; // 100MB

    //  LRU (NEW)
    std::list<std::string> lru_list;

    std::unordered_map<
        std::string,
        std::list<std::string>::iterator
    > lru_map;

    //  LRU HELPERS (NEW)
    void touchLRU(const std::string& key);
    void evictIfNeeded();
    size_t estimateSize(const std::string& key, const Value& val);
    void removeKeyInternal(const std::string& key);

public:
    Store();
    ~Store();

    //  MEMORY CONFIG (NEW)
    void setMaxMemory(size_t bytes);
    size_t getMaxMemory() const;
    size_t getCurrentMemory() const;

    //  CORE KV
    void set(const std::string& key, const std::string& value, std::optional<int> ttl);

    std::optional<std::string> get(const std::string& key);

    bool del(const std::string& key);

    bool exists(const std::string& key);

    std::vector<std::string> keys();

    //  LIST
    int lpush(const std::string& key, const std::vector<std::string>& values);

    std::optional<std::string> lpop(const std::string& key);

    int rpush(const std::string& key, const std::vector<std::string>& values);

    std::optional<std::string> rpop(const std::string& key);

    std::vector<std::string> lrange(const std::string& key, int start, int end);

    int llen(const std::string& key);

    // SET
    int sadd(const std::string& key, const std::vector<std::string>& values);

    int srem(const std::string& key, const std::vector<std::string>& values);

    std::vector<std::string> smembers(const std::string& key);

    bool sismember(const std::string& key, const std::string& value);

    int scard(const std::string& key);

    //  HASHMAP
    int hset(const std::string& key,const std::string& field,const std::string& value);

    std::optional<std::string> hget(const std::string& key, const std::string& field);

    int hdel(const std::string& key, const std::vector<std::string>& fields);

    int hlen(const std::string& key);

    std::vector<std::pair<std::string, std::string>> hgetall(const std::string& key);

    // FLUSH (NEW)
    void flushAll();
};

#endif