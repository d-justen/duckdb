#include "filter_benchmark_common.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
#include "grafite_benchmark_adapter.hpp"
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
#include "diva_benchmark_adapter.hpp"
#endif

using namespace duckdb;

namespace {
using Clock = std::chrono::steady_clock;
using State = FilterPropagateResult;
constexpr idx_t BATCH_SIZE = 2048;

template <class F>
uint64_t Time(F &&fn) {
	const auto start = Clock::now();
	fn();
	return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

uint64_t UInt(const std::string &text) {
	if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
		throw std::invalid_argument("invalid unsigned integer: " + text);
	}
	return std::stoull(text);
}

struct Config {
	std::string cases, output;
	uint64_t repetitions = 5, warmups = 1, order_seed = 42, construction_seed = 42;
	uint64_t case_index = UINT64_MAX;
	double grafite_bpk = 12, diva_bpk = 16, max_fpr = 0.001;
};

std::vector<std::string> Filters() {
	std::vector<std::string> names {"prf_uncompressed", "prf"};
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
	names.push_back("grafite");
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
	names.push_back("diva");
#endif
	return names;
}

Config Parse(int argc, char **argv) {
	Config config;
	std::map<std::string, std::string> args;
	for (int i = 1; i < argc; i++) {
		std::string arg(argv[i]);
		const auto equal = arg.find('=');
		if (equal == std::string::npos || !args.emplace(arg.substr(0, equal), arg.substr(equal + 1)).second) {
			throw std::invalid_argument("expected unique --option=value arguments");
		}
	}
	for (const auto &arg : args) {
		const auto &key = arg.first;
		const auto &value = arg.second;
		if (key == "--cases")
			config.cases = value;
		else if (key == "--output")
			config.output = value;
		else if (key == "--repetitions")
			config.repetitions = UInt(value);
		else if (key == "--warmups")
			config.warmups = UInt(value);
		else if (key == "--case-index")
			config.case_index = UInt(value);
		else if (key == "--construction-seed")
			config.construction_seed = UInt(value);
		else if (key == "--order-seed")
			config.order_seed = UInt(value);
		else if (key == "--grafite-bpk" || key == "--diva-bpk" || key == "--max-fpr") {
			size_t end;
			const auto number = std::stod(value, &end);
			if (end != value.size() || !std::isfinite(number))
				throw std::invalid_argument("invalid filter parameter");
			if (key == "--grafite-bpk")
				config.grafite_bpk = number;
			if (key == "--diva-bpk")
				config.diva_bpk = number;
			if (key == "--max-fpr")
				config.max_fpr = number;
		} else
			throw std::invalid_argument("unknown argument: " + key);
	}
	if (config.cases.empty() || config.output.empty() || !config.repetitions || config.repetitions >= (1ULL << 32) ||
	    config.warmups >= (1ULL << 32) || config.grafite_bpk <= 2 || config.diva_bpk <= 1 || config.max_fpr < 0 ||
	    config.max_fpr > 1) {
		throw std::invalid_argument("invalid JPB benchmark configuration");
	}
	return config;
}

struct Case {
	std::string query, pattern, layout, sql_path;
	uint64_t domain, rows, group_size, seed, build_keys;
};

std::vector<Case> ReadCases(const std::string &path) {
	std::ifstream input(path);
	if (!input)
		throw std::runtime_error("cannot read cases: " + path);
	const auto slash = path.find_last_of("/\\");
	const auto directory = slash == std::string::npos ? "" : path.substr(0, slash + 1);
	std::vector<Case> cases;
	std::string line;
	while (std::getline(input, line)) {
		std::vector<std::string> fields;
		std::istringstream stream(line);
		std::string field;
		while (std::getline(stream, field, '\t'))
			fields.push_back(field);
		if (fields.size() != 9)
			throw std::invalid_argument("expected nine tab-separated case fields");
		Case c {fields[0],       fields[1],       fields[2],       directory + fields[8], UInt(fields[3]),
		        UInt(fields[4]), UInt(fields[5]), UInt(fields[6]), UInt(fields[7])};
		if (!c.domain || !c.rows || !c.group_size || c.build_keys < 2 || c.build_keys > c.domain ||
		    c.domain >= (1ULL << 63) || c.rows >= (1ULL << 63) || (c.layout != "ordered" && c.layout != "uniform") ||
		    c.query.find_first_of(",\"\r") != std::string::npos ||
		    (c.pattern != "range" && c.pattern != "shoulder" && c.pattern != "seasonal" && c.pattern != "scattered")) {
			throw std::invalid_argument("invalid JPB case");
		}
		cases.push_back(c);
	}
	if (cases.empty())
		throw std::invalid_argument("empty JPB cases");
	return cases;
}

uint64_t Checksum(const std::vector<uint64_t> &values) {
	uint64_t hash = 14695981039346656037ULL;
	for (const auto value : values)
		hash = (hash ^ value) * 1099511628211ULL;
	return hash;
}

std::vector<uint64_t> BuildKeys(Connection &con, const Case &c) {
	std::ifstream input(c.sql_path);
	if (!input)
		throw std::runtime_error("cannot read selection SQL: " + c.sql_path);
	std::string sql((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	auto result = con.Query(sql);
	if (result->HasError())
		throw std::runtime_error(result->GetError());
	std::vector<uint64_t> keys;
	while (auto chunk = result->Fetch()) {
		for (idx_t i = 0; i < chunk->size(); i++) {
			const auto value = chunk->GetValue(0, i);
			if (value.IsNull())
				throw std::runtime_error("null build key");
			const auto key = value.GetValue<uint64_t>();
			if (!key || key > c.domain || (!keys.empty() && key <= keys.back())) {
				throw std::runtime_error("build keys must be sorted, distinct and in the domain");
			}
			keys.push_back(key);
		}
	}
	if (keys.size() != c.build_keys)
		throw std::runtime_error("selected build cardinality differs from workload");
	return keys;
}

struct Group {
	idx_t begin, count;
	uint64_t lower, upper;
};

struct Probes {
	std::vector<uint64_t> keys;
	std::vector<Group> groups;
	uint64_t checksum;
};

Probes GenerateProbes(const Case &c) {
	Probes probes;
	probes.keys.resize(c.rows);
	if (c.layout == "ordered") {
		// Generate floor(i * domain / rows) without a potentially overflowing product.
		// Both rows and domain are below 2^63, so carry + remainder fits uint64_t.
		uint64_t rank = 0, carry = 0;
		const auto step = c.domain / c.rows, remainder = c.domain % c.rows;
		for (uint64_t i = 0; i < c.rows; i++) {
			probes.keys[i] = rank + 1;
			rank += step;
			carry += remainder;
			if (carry >= c.rows) {
				rank++;
				carry -= c.rows;
			}
		}
	} else {
		std::mt19937_64 rng(c.seed);
		for (auto &key : probes.keys) {
			key = rng() % c.domain + 1;
		}
	}
	for (idx_t begin = 0; begin < c.rows;) {
		const auto count = std::min<uint64_t>(c.group_size, c.rows - begin);
		const auto bounds = std::minmax_element(probes.keys.begin() + begin, probes.keys.begin() + begin + count);
		probes.groups.push_back({begin, count, *bounds.first, *bounds.second});
		begin += count;
	}
	probes.checksum = Checksum(probes.keys);
	return probes;
}

struct Truth {
	std::vector<uint64_t> membership, group_positives;
	std::vector<bool> empty_ranges;
	uint64_t positives = 0, empty_count = 0;
	bool Contains(uint64_t key) const {
		return (membership[key / 64] >> (key % 64)) & 1;
	}
};

Truth BuildTruth(const Case &c, const std::vector<uint64_t> &build, const Probes &probes) {
	Truth truth;
	truth.membership.resize(c.domain / 64 + 1, 0);
	for (const auto key : build)
		truth.membership[key / 64] |= 1ULL << (key % 64);
	for (const auto &group : probes.groups) {
		uint64_t positives = 0;
		for (idx_t i = group.begin; i < group.begin + group.count; i++)
			positives += truth.Contains(probes.keys[i]);
		truth.positives += positives;
		truth.group_positives.push_back(positives);
		const auto first = std::lower_bound(build.begin(), build.end(), group.lower);
		const bool empty = first == build.end() || *first > group.upper;
		truth.empty_ranges.push_back(empty);
		truth.empty_count += empty;
	}
	return truth;
}

class Summary {
public:
	virtual ~Summary() = default;
	virtual void Build(ClientContext &context, const std::vector<uint64_t> &keys, const Config &config,
	                   uint32_t build_seed) = 0;
	virtual State Range(uint64_t lower, uint64_t upper) = 0;
	virtual idx_t Points(Vector &keys, idx_t count, SelectionVector &selection) = 0;
	virtual idx_t Bytes() const = 0;
	virtual std::string Mode() const {
		return "na";
	}
	BenchmarkBuildInfo build_info;
	uint64_t finalization_ns = 0;
};

class PRFSummary : public Summary {
public:
	explicit PRFSummary(bool compress) : compress(compress) {
	}
	void Build(ClientContext &context, const std::vector<uint64_t> &keys, const Config &config,
	           uint32_t build_seed) override {
		filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::UBIGINT);
		const auto lower = Value::UBIGINT(keys.front()), upper = Value::UBIGINT(keys.back());
		PrefixRangeFilter::Sizing sizing;
		if (!PrefixRangeFilter::TryComputeSizing(lower, upper, keys.size(), sizing, config.max_fpr)) {
			throw std::runtime_error("PRF sizing failed");
		}
		filter->Initialize(context, keys.size(), lower, upper, sizing);
		auto state = filter->InitializeBuildState(context, true);
		Vector input(LogicalType::UBIGINT, reinterpret_cast<data_ptr_t>(const_cast<uint64_t *>(keys.data())),
		             keys.size());
		filter->InsertKeys(input, keys.size(), *state);
		filter->MergeBuildState(*state);
		finalization_ns = Time([&] {
			if (compress)
				filter->Compress(context, config.max_fpr);
			else
				filter->Analyze();
		});
	}
	State Range(uint64_t lower, uint64_t upper) override {
		return filter->LookupRange(Value::UBIGINT(lower), Value::UBIGINT(upper));
	}
	idx_t Points(Vector &keys, idx_t count, SelectionVector &selection) override {
		return filter->LookupKeys(keys, selection, count);
	}
	idx_t Bytes() const override {
		const auto info = filter->GetCompressionInfo(false);
		return (info.mode == CompressionMode::DIRECT_RANGES
		            ? info.range_count * 2 * sizeof(uint64_t)
		            : ((info.logical_bucket_count + 63) / 64) * sizeof(uint64_t)) +
		       info.range_index_bytes;
	}
	std::string Mode() const override {
		return filter->GetCompressionInfo(false).mode == CompressionMode::DIRECT_RANGES ? "direct_ranges" : "bitmap";
	}

private:
	bool compress;
	duckdb::unique_ptr<PrefixRangeFilter> filter;
};

template <class Adapter, bool GRAFITE>
class RangeSummary : public Summary {
public:
	void Build(ClientContext &, const std::vector<uint64_t> &keys, const Config &config, uint32_t build_seed) override {
		filter.Build(keys, GRAFITE ? config.grafite_bpk : config.diva_bpk, build_seed);
		build_info = filter.BuildInfo();
	}
	State Range(uint64_t lower, uint64_t upper) override {
		return filter.RangeProbe(lower, upper) ? State::NO_PRUNING_POSSIBLE : State::FILTER_ALWAYS_FALSE;
	}
	idx_t Points(Vector &keys, idx_t count, SelectionVector &selection) override {
		const auto data = FlatVector::GetData<uint64_t>(keys);
		idx_t matches = 0;
		for (idx_t i = 0; i < count; i++) {
			if (filter.Probe(data[i]))
				selection.set_index(matches++, i);
		}
		return matches;
	}
	idx_t Bytes() const override {
		return filter.SizeBytes();
	}
	std::string Mode() const override {
		return build_info.representation;
	}

private:
	Adapter filter;
};

std::unique_ptr<Summary> MakeSummary(const std::string &name) {
	if (name == "prf")
		return std::unique_ptr<Summary>(new PRFSummary(true));
	if (name == "prf_uncompressed")
		return std::unique_ptr<Summary>(new PRFSummary(false));
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_GRAFITE)
	if (name == "grafite")
		return std::unique_ptr<Summary>(new RangeSummary<GrafiteBenchmarkFilter, true>());
#endif
#if defined(DUCKDB_FILTER_BENCHMARK_HAS_DIVA)
	if (name == "diva")
		return std::unique_ptr<Summary>(new RangeSummary<DivaBenchmarkFilter, false>());
#endif
	throw std::invalid_argument("unavailable filter: " + name);
}

struct Measurement {
	uint64_t build_ns = 0, finalization_ns = 0, range_ns = 0, point_ns = 0, bytes = 0;
	uint64_t groups_rejected = 0, groups_accepted = 0, groups_mixed = 0;
	uint64_t rows_rejected = 0, rows_accepted = 0, point_probes = 0, point_candidates = 0;
	uint64_t candidates = 0, false_positives = 0, false_negatives = 0, range_false_positives = 0;
	std::string mode;
	BenchmarkBuildInfo build_info;
	uint32_t build_seed = 0;
};

Measurement Run(ClientContext &context, const Config &config, const std::string &name,
                const std::vector<uint64_t> &build, Probes &probes, const Truth &truth, uint32_t build_seed) {
	Measurement m;
	std::vector<State> states(probes.groups.size());
	// Common output scratch allocation is outside measurement, like prepared probe inputs.
	std::vector<uint64_t> accepted((probes.keys.size() + 63) / 64, 0);
	SelectionVector selection(BATCH_SIZE);
	std::unique_ptr<Summary> summary;
	m.build_ns = Time([&] {
		summary = MakeSummary(name);
		summary->Build(context, build, config, build_seed);
	});
	m.finalization_ns = summary->finalization_ns;
	m.bytes = summary->Bytes();
	m.mode = summary->Mode();
	m.build_seed = build_seed;
	m.build_info = summary->build_info;
	m.range_ns = Time([&] {
		for (idx_t i = 0; i < probes.groups.size(); i++) {
			states[i] = summary->Range(probes.groups[i].lower, probes.groups[i].upper);
		}
	});
	for (idx_t i = 0; i < states.size(); i++) {
		if (states[i] == State::FILTER_ALWAYS_FALSE) {
			m.groups_rejected++;
			m.rows_rejected += probes.groups[i].count;
		} else if (states[i] == State::FILTER_ALWAYS_TRUE) {
			m.groups_accepted++;
			m.rows_accepted += probes.groups[i].count;
		} else {
			m.groups_mixed++;
			m.point_probes += probes.groups[i].count;
		}
		m.range_false_positives += truth.empty_ranges[i] && states[i] != State::FILTER_ALWAYS_FALSE;
	}
	if (m.point_probes) {
		m.point_ns = Time([&] {
			for (idx_t g = 0; g < states.size(); g++) {
				if (states[g] != State::NO_PRUNING_POSSIBLE)
					continue;
				const auto &group = probes.groups[g];
				for (idx_t offset = 0; offset < group.count; offset += BATCH_SIZE) {
					const auto count = std::min<idx_t>(BATCH_SIZE, group.count - offset);
					const auto begin = group.begin + offset;
					Vector input(LogicalType::UBIGINT, reinterpret_cast<data_ptr_t>(probes.keys.data() + begin), count);
					const auto matches = summary->Points(input, count, selection);
					m.point_candidates += matches;
					// Consume the same selected-row representation for every implementation.
					for (idx_t i = 0; i < matches; i++) {
						const auto row = begin + selection.get_index(i);
						accepted[row / 64] |= 1ULL << (row % 64);
					}
				}
			}
		});
	}
	// Validate actual timed outputs, including both range shortcuts, outside all timers.
	uint64_t matched_positives = 0, matched_points = 0;
	for (idx_t g = 0; g < states.size(); g++) {
		if (states[g] == State::FILTER_ALWAYS_TRUE)
			matched_positives += truth.group_positives[g];
		if (states[g] != State::NO_PRUNING_POSSIBLE)
			continue;
		const auto &group = probes.groups[g];
		for (idx_t i = group.begin; i < group.begin + group.count; i++) {
			if ((accepted[i / 64] >> (i % 64)) & 1) {
				matched_points++;
				matched_positives += truth.Contains(probes.keys[i]);
			}
		}
	}
	m.candidates = m.rows_accepted + m.point_candidates;
	m.false_negatives = truth.positives - matched_positives;
	m.false_positives = m.candidates - matched_positives;
	if (matched_points != m.point_candidates || m.false_negatives) {
		throw std::runtime_error("incorrect selection or false negative in " + name);
	}
	return m;
}

void Write(std::ostream &out, const Config &config, const Case &c, const std::string &filter, uint64_t rep,
           const Probes &probes, const Truth &truth, uint64_t build_checksum, const Measurement &m) {
	out << c.query << ',' << c.pattern << ',' << c.layout << ',' << c.domain << ',' << c.rows << ',' << c.group_size
	    << ',' << c.seed << ',' << c.build_keys << ',' << filter << ',' << rep << ',' << m.build_ns << ','
	    << m.finalization_ns << ',' << m.range_ns << ',' << m.point_ns << ',' << m.build_ns + m.range_ns + m.point_ns
	    << ',' << probes.groups.size() << ',' << m.groups_rejected << ',' << m.groups_accepted << ',' << m.groups_mixed
	    << ',' << m.rows_rejected << ',' << m.rows_accepted << ',' << m.point_probes << ',' << m.point_candidates << ','
	    << m.candidates << ',' << truth.positives << ',' << m.false_positives << ',' << m.false_negatives << ','
	    << truth.empty_count << ',' << m.range_false_positives << ',' << m.bytes << ',' << m.mode << ','
	    << build_checksum << ',' << probes.checksum << ',' << std::setprecision(17) << config.grafite_bpk << ','
	    << config.diva_bpk << ',' << config.max_fpr << ',' << double(m.bytes) * 8 / c.build_keys << ','
	    << config.construction_seed << ',' << m.build_seed << ",sorted_distinct," << m.build_info.CSV() << '\n';
	if (!out)
		throw std::runtime_error("failed to write JPB measurements");
}
} // namespace

int main(int argc, char **argv) {
	try {
		if (argc == 2 && std::string(argv[1]) == "--list-filters") {
			for (const auto &name : Filters())
				std::cout << name << '\n';
			return 0;
		}
		const auto config = Parse(argc, argv);
		const auto cases = ReadCases(config.cases);
		if (config.case_index != UINT64_MAX && config.case_index >= cases.size())
			throw std::invalid_argument("case-index is outside the supplied cases");
		std::ofstream out(config.output);
		if (!out)
			throw std::runtime_error("cannot write benchmark output");
		out << "query,pattern,layout,dimension_cardinality,probe_rows,row_group_size,seed,build_keys,filter,repetition,"
		       "build_time_ns,finalization_time_ns,range_time_ns,point_time_ns,total_time_ns,range_probes,"
		       "groups_rejected,groups_accepted,groups_mixed,rows_rejected,rows_accepted,point_probes,point_candidates,"
		       "candidate_rows,true_positives,false_positives,false_negatives,empty_ranges,range_false_positives,"
		       "summary_bytes,representation,build_checksum,probe_checksum,grafite_bpk,diva_bpk,max_fpr,measured_bpk,"
		       "construction_seed,build_seed,build_input_order,"
		    << BenchmarkBuildInfo::Header() << "\n";
		DuckDB db(nullptr);
		Connection con(db);
		auto settings = con.Query("SET threads=1");
		if (settings->HasError())
			throw std::runtime_error(settings->GetError());
		Probes probes;
		std::string previous;
		std::mt19937_64 order(config.order_seed);
		for (size_t case_index = 0; case_index < cases.size(); case_index++) {
			const auto &c = cases[case_index];
			if (config.case_index != UINT64_MAX && case_index != config.case_index) {
				// Advance the original ordering RNG even for skipped cases, so
				// checkpointed execution uses the identical per-case filter order.
				for (uint64_t rep = 0; rep < config.warmups + config.repetitions; rep++) {
					auto filters = Filters();
					std::shuffle(filters.begin(), filters.end(), order);
				}
				continue;
			}
			const auto identity = std::to_string(c.domain) + ":" + std::to_string(c.rows) + ":" +
			                      std::to_string(c.group_size) + ":" + std::to_string(c.seed) + ":" + c.layout;
			if (identity != previous) {
				// Release the previous 4 GB column before generating its replacement.
				probes = Probes();
				probes = GenerateProbes(c);
				previous = identity;
			}
			const auto keys = BuildKeys(con, c);
			ValidateBenchmarkBuildKeys(keys);
			const auto build_checksum = BenchmarkBuildChecksum(keys);
			const auto truth = BuildTruth(c, keys, probes);
			for (uint64_t rep = 0; rep < config.warmups + config.repetitions; rep++) {
				auto filters = Filters();
				std::shuffle(filters.begin(), filters.end(), order);
				for (const auto &name : filters) {
					const auto measured_rep = rep < config.warmups ? 0 : rep - config.warmups;
					const auto build_seed = BenchmarkBuildSeed(config.construction_seed, build_checksum, measured_rep);
					const auto m = Run(*con.context, config, name, keys, probes, truth, build_seed);
					if (rep >= config.warmups)
						Write(out, config, c, name, rep - config.warmups, probes, truth, build_checksum, m);
				}
				out.flush();
				std::cerr << c.query << " repetition " << rep + 1 << '/' << config.warmups + config.repetitions << '\n';
			}
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "JPB benchmark: " << error.what() << '\n';
		return 1;
	}
}
