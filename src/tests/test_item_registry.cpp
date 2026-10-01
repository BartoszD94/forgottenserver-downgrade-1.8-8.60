#include "../otpch.h"

#include "../item.h"
#include "test_support.h"

#include <atomic>
#include <barrier>
#include <thread>

extern bool isValidItemPointer(Item* item);

TEST_CASE(item_registry_handles_parallel_construction_copy_lookup_and_destruction)
{
	constexpr size_t workers = 8;
	auto sentinel = std::make_shared<Item>(0);
	std::barrier start(static_cast<std::ptrdiff_t>(workers + 1));
	std::atomic<bool> valid{true};
	{
		std::vector<std::jthread> threads;
		for (size_t worker = 0; worker < workers; ++worker) {
			threads.emplace_back([&]() {
				start.arrive_and_wait();
				for (size_t iteration = 0; iteration < 200; ++iteration) {
					std::vector<std::shared_ptr<Item>> batch;
					for (size_t entry = 0; entry < 32; ++entry) {
						auto item = std::make_shared<Item>(0);
						auto copy = std::make_shared<Item>(*item);
						if (!isValidItemPointer(item.get()) || !isValidItemPointer(copy.get()) ||
						    !isValidItemPointer(sentinel.get()) || isValidItemPointer(nullptr)) {
							valid.store(false);
						}
						batch.push_back(std::move(item));
						batch.push_back(std::move(copy));
					}
				}
			});
		}
		start.arrive_and_wait();
	}
	CHECK(valid.load());
	CHECK(isValidItemPointer(sentinel.get()));
	Item* address = sentinel.get();
	sentinel.reset();
	CHECK(!isValidItemPointer(address));
}

// Retirement is deliberately irreversible. Keep this case last in this process.
TEST_CASE(item_registry_retirement_is_atomic_with_live_worker_items)
{
	constexpr size_t workers = 8;
	std::atomic<size_t> ready{0};
	std::atomic<bool> retired{false};
	std::atomic<bool> valid{true};
	auto sentinel = std::make_shared<Item>(0);
	CHECK(isValidItemPointer(sentinel.get()));
	{
		std::vector<std::jthread> threads;
		for (size_t worker = 0; worker < workers; ++worker) {
			threads.emplace_back([&]() {
				std::vector<std::shared_ptr<Item>> held;
				for (size_t entry = 0; entry < 128; ++entry) {
					held.push_back(std::make_shared<Item>(0));
				}
				ready.fetch_add(1);
				while (!retired.load()) {
					std::this_thread::yield();
				}
				for (const auto& item : held) {
					if (isValidItemPointer(item.get())) {
						valid.store(false);
					}
				}
				auto after = std::make_shared<Item>(0);
				auto copy = std::make_shared<Item>(*after);
				if (isValidItemPointer(after.get()) || isValidItemPointer(copy.get())) {
					valid.store(false);
				}
			});
		}
		while (ready.load() != workers) {
			std::this_thread::yield();
		}
		Item::clearGlobalRegistry();
		retired.store(true);
	}
	CHECK(valid.load());
	CHECK(!isValidItemPointer(sentinel.get()));
}

TFS_TEST_MAIN()
