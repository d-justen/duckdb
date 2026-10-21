// Diagnostic-only generated classes expose their bitmaps; production classes do not.
#include "prf_baseline.inc"
#include "prf_candidate.inc"
#include "duckdb.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

using namespace duckdb;
using Clock = std::chrono::steady_clock;

static uint64_t Nanos(Clock::time_point start) {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

class Workers {
public:
	explicit Workers(idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			threads.emplace_back([this]() {
				idx_t seen = 0;
				std::unique_lock<std::mutex> lock(mutex);
				while (true) {
					wake.wait(lock, [&]() { return stopping || epoch != seen; });
					if (stopping)
						return;
					seen = epoch;
					auto work = state;
					lock.unlock();
					for (idx_t task = next.fetch_add(1); task < work->TaskCount(); task = next.fetch_add(1)) {
						work->ExecuteTask(task);
					}
					lock.lock();
					if (++finished == threads.size())
						done.notify_one();
				}
			});
		}
	}
	~Workers() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		for (auto &thread : threads)
			thread.join();
	}
	void Run(PrefixRangeFilter::ParallelCompressionState &work) {
		if (work.TaskCount() == 1) {
			work.ExecuteTask(0);
			return;
		}
		std::unique_lock<std::mutex> lock(mutex);
		state = &work;
		next = 0;
		finished = 0;
		epoch++;
		wake.notify_all();
		done.wait(lock, [&]() { return finished == threads.size(); });
	}

private:
	std::mutex mutex;
	std::condition_variable wake, done;
	vector<std::thread> threads;
	std::atomic<idx_t> next {0};
	idx_t epoch = 0, finished = 0;
	bool stopping = false;
	PrefixRangeFilter::ParallelCompressionState *state = nullptr;
};

struct Snapshot {
	uint64_t min, span;
	idx_t shift, words;
	const uint64_t *bitmap;
	vector<std::pair<uint64_t, uint64_t>> ranges;
};
struct Result {
	unique_ptr<PrefixRangeFilter> filter;
	Snapshot snapshot;
	uint64_t build_ns, compress_ns, first_ns, warm_ns, shared_ns = 0, controlled_ns = 0;
	idx_t found;
};

static uint64_t Median(vector<uint64_t> values) {
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

// Both algorithms call this exact function, with the same output-buffer address.
__attribute__((noinline)) static idx_t SharedProbe(const Snapshot &s, const vector<uint64_t> &keys,
                                                   SelectionVector &sel) {
	idx_t found = 0;
	for (idx_t i = 0; i < keys.size(); i++) {
		const auto y = keys[i] - s.min;
		const uint8_t in_range = y <= s.span;
		const auto bit_idx = s.shift >= 64 ? 0 : y >> s.shift;
		const uint32_t word_idx = (bit_idx >> 6) & (0U - in_range);
		const auto bit = (s.bitmap[word_idx] >> (bit_idx & 63)) & 1ULL;
		sel.set_index(found, i);
		found += bit & in_range;
	}
	return found;
}

static uint64_t SharedTime(const Snapshot &s, const vector<uint64_t> &keys, SelectionVector &sel, idx_t expected) {
	vector<uint64_t> times;
	for (idx_t pass = 0; pass < 10; pass++) {
		const auto start = Clock::now();
		const auto found = SharedProbe(s, keys, sel);
		const auto elapsed = Nanos(start);
		if (found != expected)
			throw std::runtime_error("shared probe mismatch");
		if (pass)
			times.push_back(elapsed);
	}
	return Median(times);
}

template <class FILTER>
static Result Build(ClientContext &context, Vector &build, Vector &probes, const vector<uint64_t> &keys, idx_t tasks,
                    Workers &workers) {
	Result result;
	auto start = Clock::now();
	auto filter = make_uniq<FILTER>();
	PrefixRangeFilter::Sizing sizing;
	if (!PrefixRangeFilter::TryComputeSizing(Value::UBIGINT(keys.front()), Value::UBIGINT(keys.back()), keys.size(),
	                                         sizing, 0.001))
		throw std::runtime_error("sizing failed");
	filter->Initialize(context, keys.size(), Value::UBIGINT(keys.front()), Value::UBIGINT(keys.back()), sizing, 1.0);
	auto state = filter->InitializeBuildState(context, false);
	filter->InsertKeys(build, keys.size(), *state);
	filter->MergeBuildState(*state);
	state.reset();
	result.build_ns = Nanos(start);
	start = Clock::now();
	if (tasks > 1) {
		auto compression = filter->InitializeParallelCompression(context, 0.001, tasks);
		if (compression) {
			do {
				workers.Run(*compression);
			} while (compression->FinishPass());
		} else {
			filter->Compress(context, 0.001);
		}
	} else {
		filter->Compress(context, 0.001);
	}
	filter->Analyze();
	result.compress_ns = Nanos(start);
	SelectionVector sel(probes.size());
	result.filter = std::move(filter);
	vector<uint64_t> warm;
	for (idx_t pass = 0; pass < 10; pass++) {
		start = Clock::now();
		const auto found = result.filter->LookupKeys(probes, sel, probes.size());
		const auto elapsed = Nanos(start);
		if (pass == 0) {
			result.first_ns = elapsed;
			result.found = found;
		} else {
			if (found != result.found)
				throw std::runtime_error("warm probe mismatch");
			warm.push_back(elapsed);
		}
	}
	result.warm_ns = Median(warm);
	const auto &bitmap = static_cast<FILTER &>(*result.filter).bitmap;
	result.snapshot = {bitmap.min, bitmap.span, bitmap.shift, bitmap.word_count, bitmap.bitmap, {}};
	for (idx_t i = 0; i < bitmap.range_count; i++) {
		result.snapshot.ranges.emplace_back(bitmap.ranges[i].lower, bitmap.ranges[i].width);
	}
	return result;
}

int main(int argc, char **argv) {
	try {
		const idx_t rounds = argc > 1 ? std::stoull(argv[1]) : 11;
		std::ofstream out(argc > 2 ? argv[2] : "prf-comparison.csv");
		if (!out)
			throw std::runtime_error("cannot open CSV");
		out << "round,clusters,tasks,variant,build_ns,compress_ns,first_probe_ns,warm_probe_ns,shared_probe_ns,"
		       "controlled_probe_ns,shift,bytes,found,bitmap_equal\n";
		DuckDB db(nullptr);
		Connection con(db);
		Workers workers(8);
		std::mt19937_64 rng(42);
		const idx_t domain = 100000000, key_count = 1000000, probe_count = 1000000;
		vector<uint64_t> probe_keys(probe_count);
		std::uniform_int_distribution<uint64_t> uniform(0, domain - 1);
		for (auto &key : probe_keys)
			key = uniform(rng);
		Vector probes(LogicalType::UBIGINT, probe_count);
		std::copy(probe_keys.begin(), probe_keys.end(), FlatVector::GetDataMutable<uint64_t>(probes));
		FlatVector::SetSize(probes, probe_count);
		SelectionVector shared_sel(probe_count);
		vector<uint64_t> controlled((domain + 63) / 64);
		vector<idx_t> clusters_list {2, 64, 1024, 8192, 16384, 32768, 65536, 131072, 524288};
		for (idx_t round = 0; round < rounds; round++) {
			std::shuffle(clusters_list.begin(), clusters_list.end(), rng);
			for (const auto clusters : clusters_list) {
				vector<uint64_t> keys;
				keys.reserve(key_count);
				uint64_t cursor = 0;
				for (idx_t c = 0; c < clusters; c++) {
					const auto length = key_count / clusters + (c < key_count % clusters);
					for (idx_t i = 0; i < length; i++)
						keys.push_back(cursor++);
					if (c + 1 < clusters)
						cursor += (domain - key_count) / (clusters - 1) + (c < (domain - key_count) % (clusters - 1));
				}
				Vector build(LogicalType::UBIGINT, keys.size());
				std::copy(keys.begin(), keys.end(), FlatVector::GetDataMutable<uint64_t>(build));
				FlatVector::SetSize(build, keys.size());
				for (idx_t tasks : {idx_t(1), idx_t(8)}) {
					Result results[2];
					// Counterbalance allocation order exactly across an even number of measured rounds.
					const idx_t first = (round + clusters % 3 + (tasks == 8)) % 2;
					for (idx_t step = 0; step < 2; step++) {
						const auto variant = (first + step) % 2;
						results[variant] = variant == 0 ? Build<baseline::NumericPrefixRangeFilter<uint64_t>>(
						                                      *con.context, build, probes, keys, tasks, workers)
						                                : Build<candidate::NumericPrefixRangeFilter<uint64_t>>(
						                                      *con.context, build, probes, keys, tasks, workers);
					}
					const auto &a = results[0].snapshot;
					const auto &b = results[1].snapshot;
					const auto ai = results[0].filter->GetCompressionInfo(),
					           bi = results[1].filter->GetCompressionInfo();
					if (a.shift != b.shift || a.words != b.words || a.min != b.min || a.span != b.span ||
					    a.ranges != b.ranges || ai.mode != bi.mode || ai.active_buckets != bi.active_buckets ||
					    ai.false_positive_rate != bi.false_positive_rate ||
					    ai.logical_bucket_count != bi.logical_bucket_count || ai.run_count != bi.run_count ||
					    ai.range_count != bi.range_count || ai.range_index_count != bi.range_index_count ||
					    results[0].found != results[1].found ||
					    (a.words && std::memcmp(a.bitmap, b.bitmap, a.words * sizeof(uint64_t)))) {
						throw std::runtime_error("final representation mismatch");
					}
					for (idx_t step = 0; step < 2; step++) {
						const auto variant = (first + step) % 2;
						auto &r = results[variant];
						if (r.snapshot.words) {
							r.shared_ns = SharedTime(r.snapshot, probe_keys, shared_sel, r.found);
							std::copy_n(r.snapshot.bitmap, r.snapshot.words, controlled.data());
							auto snapshot = r.snapshot;
							snapshot.bitmap = controlled.data();
							r.controlled_ns = SharedTime(snapshot, probe_keys, shared_sel, r.found);
						}
						out << round << ',' << clusters << ',' << tasks << ',' << (variant ? "candidate" : "baseline")
						    << ',' << r.build_ns << ',' << r.compress_ns << ',' << r.first_ns << ',' << r.warm_ns << ','
						    << r.shared_ns << ',' << r.controlled_ns << ',' << r.snapshot.shift << ','
						    << r.snapshot.words * 8 << ',' << r.found << ",true\n";
						out.flush();
					}
				}
			}
			std::cout << "round " << round << " complete; all bitmaps identical" << std::endl;
		}
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
