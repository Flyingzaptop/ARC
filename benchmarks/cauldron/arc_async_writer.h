#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <ostream>
#include <thread>
#include <chrono>
#include <memory>
namespace arc_bench {
// One render producer, one IO consumer. Overflow invalidates the benchmark.
class AsyncWriter {
    static constexpr unsigned capacity=8192;
    std::unique_ptr<std::function<void(std::ostream&)>[]> records_{new std::function<void(std::ostream&)>[capacity]};
    std::atomic<unsigned long long> head_{},tail_{};
    std::atomic<bool> stop_{},failed_{};
    std::atomic<unsigned long long> dropped_{};
    std::thread worker_;
public:
    explicit AsyncWriter(std::ostream& output):worker_([this,&output]{
        for(;;){const auto tail=tail_.load(std::memory_order_relaxed);
            if(tail!=head_.load(std::memory_order_acquire)){
                try{records_[tail%capacity](output);if(!output)failed_=true;}catch(...){failed_=true;}
                records_[tail%capacity]={};tail_.store(tail+1,std::memory_order_release);
            }else if(stop_.load())break;else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }){}
    ~AsyncWriter(){finish();}
    void push(std::function<void(std::ostream&)> record){const auto head=head_.load(std::memory_order_relaxed);
        if(head-tail_.load(std::memory_order_acquire)>=capacity){++dropped_;return;}
        records_[head%capacity]=std::move(record);head_.store(head+1,std::memory_order_release);
    }
    bool finish(){stop_=true;if(worker_.joinable())worker_.join();return !failed_&&!dropped_;}
};
}
