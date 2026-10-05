#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/profiling_node.hpp"

#include <iostream>
#include <thread>

using namespace duckdb;

namespace {

optional_ptr<ProfilingNode> FindJoinFilterProfile(ProfilingNode &node) {
	if (node.json_numeric_metrics.count("join_filter")) {
		return node;
	}
	for (auto &child : node.children) {
		auto result = FindJoinFilterProfile(*child);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("Runtime join filter elapsed profiling uses a common PRF build", "[api][profiling]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='join_order,build_side_probe_side'"));
	REQUIRE_NO_FAIL(con.Query("SET enable_perfect_hash_join_filter_pushdown=false"));
	REQUIRE_NO_FAIL(con.Query("SET enable_join_min_max_filter_pushdown=false"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE exact_build AS SELECT (i*2)::INTEGER k FROM range(1300000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE exact_probe AS SELECT i::INTEGER k FROM range(2600000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE small_build AS SELECT (i*2)::INTEGER k FROM range(5000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE small_probe AS SELECT i::INTEGER k FROM range(10000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE shifted_build AS SELECT i::INTEGER k FROM range(50000) t(i) "
	                          "UNION ALL SELECT (i+90000000)::INTEGER k FROM range(50000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE shifted_probe AS SELECT k FROM shifted_build UNION ALL "
	                          "SELECT k+500000 FROM shifted_build"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE sparse_build AS SELECT (i*2000000000)::BIGINT k FROM range(100000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE sparse_probe AS SELECT (i*1000000000)::BIGINT k FROM range(200000) t(i)"));
	con.EnableProfiling();
	con.context->config.emit_profiler_output = false;

	for (auto threads : {1, 4}) {
		REQUIRE_NO_FAIL(con.Query("SET threads=" + to_string(threads)));
		for (auto perfect : {false, true}) {
			unordered_map<string, unordered_map<string, double>> compressed_build_metrics;
			for (auto mode : {"none", "bloom", "compressed", "uncompressed"}) {
				const string filter_mode(mode);
				const bool prf = filter_mode == "compressed" || filter_mode == "uncompressed";
				const bool compressed = filter_mode == "compressed";
				REQUIRE_NO_FAIL(con.Query(string("SET enable_prefix_range_filter=") + (prf ? "true" : "false")));
				REQUIRE_NO_FAIL(
				    con.Query(string("SET enable_prefix_range_filter_compression=") + (compressed ? "true" : "false")));
				REQUIRE_NO_FAIL(con.Query(string("SET enable_join_bloom_filter_pushdown=") +
				                          (filter_mode == "none" ? "false" : "true")));
				for (auto dataset : {"exact", "shifted", "sparse", "small"}) {
					const string name(dataset);
					if (perfect && name != "small") {
						continue;
					}
					CAPTURE(threads, perfect, filter_mode, name);
					// A second non-equality condition prevents perfect hash joins in the ordinary cases.
					auto result = con.Query("SELECT count(*) FROM " + name + "_probe p JOIN " + name +
					                        "_build b ON p.k=b.k" + string(perfect ? "" : " AND p.k%7>=b.k%7"));
					REQUIRE_NO_FAIL(*result);
					REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() ==
					        (name == "exact" ? 1300000 : (name == "small" ? 5000 : 100000)));
					auto root = con.GetProfilingTree();
					REQUIRE(root);
					auto node = FindJoinFilterProfile(*root);
					REQUIRE(node);
					auto &metrics = node->json_numeric_metrics.at("join_filter");
					const auto total = metrics.at("join_finalize_elapsed_seconds");
					const auto build = metrics.at("join_finalize_build_elapsed_seconds");
					const auto prf_build = metrics.at("prf_build_elapsed_seconds");
					const auto compression = metrics.at("prf_compression_elapsed_seconds");
					const auto analysis = metrics.at("prf_analysis_elapsed_seconds");
					REQUIRE(build > 0);
					REQUIRE((prf_build > 0) == prf);
					REQUIRE(prf_build <= build);
					REQUIRE(total == Approx(build + compression + analysis));
					REQUIRE((compression > 0) == compressed);
					REQUIRE((analysis > 0) ==
					        (filter_mode == "uncompressed" && (name == "shifted" || name == "sparse")));
					REQUIRE(metrics.at("finalize_count") == 1);
					REQUIRE(metrics.at("prf_build_count") == (prf ? 1 : 0));
					REQUIRE(metrics.at("bloom_build_count") ==
					        (filter_mode == "bloom" || (prf && name == "sparse") ? 1 : 0));
					REQUIRE(metrics.at("perfect_hash_join_count") == (perfect ? 1 : 0));
					REQUIRE(metrics.at("external_finalize_count") == 0);
					if (prf) {
						auto &prf_metrics = node->json_numeric_metrics.at("prefix_range_filter");
						REQUIRE(prf_metrics.count("build_worker_seconds") == 0);
						REQUIRE(prf_metrics.count("probe_worker_seconds") == 0);
						REQUIRE(prf_metrics.count("build_phase_elapsed_seconds") == 0);
						const auto tasks = name == "exact" && threads == 4 ? 4 : 1;
						REQUIRE(prf_metrics.at("build_task_count") == tasks);
						REQUIRE(prf_metrics.at("local_bitmap_count") == (tasks == 1 ? 0 : tasks));
						REQUIRE(prf_metrics.at("bloom_fallback_selected") == (name == "sparse" ? 1 : 0));
						if (compressed) {
							compressed_build_metrics[name] = prf_metrics;
						} else {
							// Parallel sinking can produce different chunk counts for the same build rows.
							for (auto metric : {"build_task_count", "local_bitmap_count",
							                    "local_bitmap_bytes_allocated", "local_bitmap_peak_bytes"}) {
								REQUIRE(prf_metrics.at(metric) == compressed_build_metrics.at(name).at(metric));
							}
						}
					}
				}
			}
		}
	}
	REQUIRE_NO_FAIL(con.Query("SET enable_prefix_range_filter=true; SET enable_join_bloom_filter_pushdown=false"));
	REQUIRE_NO_FAIL(con.Query("SET profiling_mode='detailed'"));
	for (auto compress : {false, true}) {
		REQUIRE_NO_FAIL(
		    con.Query(string("SET enable_prefix_range_filter_compression=") + (compress ? "true" : "false")));
		REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM sparse_probe p JOIN sparse_build b ON p.k=b.k"));
		auto node = FindJoinFilterProfile(*con.GetProfilingTree());
		REQUIRE(node);
		auto &metrics = node->json_numeric_metrics.at("join_filter");
		auto &prf = node->json_numeric_metrics.at("prefix_range_filter");
		REQUIRE(metrics.at("finalize_count") == 1);
		REQUIRE(metrics.at("bloom_build_count") == 0);
		REQUIRE(metrics.at("prf_build_elapsed_seconds") > 0);
		REQUIRE(metrics.at("prf_build_elapsed_seconds") <= metrics.at("join_finalize_build_elapsed_seconds"));
		REQUIRE(prf.at("final_prf_enabled") == 0);
		REQUIRE(prf.at("bloom_fallback_selected") == 0);
		REQUIRE(prf.at("build_worker_seconds") > 0);
		REQUIRE(prf.at("build_worker_seconds") ==
		        Approx(prf.at("build_initialization_seconds") + prf.at("build_insertion_seconds") +
		               prf.at("build_merge_seconds")));
		REQUIRE(prf.count("probe_worker_seconds") == 1);
	}
}

TEST_CASE("Runtime join filter elapsed profiling counts repeated and reused builds", "[api][profiling]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='join_order,build_side_probe_side'"));
	con.EnableProfiling();
	con.context->config.emit_profiler_output = false;
	for (auto reuse : {false, true}) {
		// A recursive RHS rebuilds four times (including its last empty build). A constant RHS is reused.
		const string join = reuse ? "t JOIN range(3) s(k)" : "range(3) s(k) JOIN t";
		auto result = con.Query("WITH RECURSIVE t(k) AS (SELECT 0::BIGINT UNION ALL SELECT t.k+1 FROM " + join +
		                        " ON s.k=t.k WHERE t.k<3) SELECT max(k) FROM t");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 3);
		auto node = FindJoinFilterProfile(*con.GetProfilingTree());
		REQUIRE(node);
		auto &metrics = node->json_numeric_metrics.at("join_filter");
		REQUIRE(metrics.at("finalize_count") == (reuse ? 1 : 4));
		REQUIRE(metrics.at("join_finalize_elapsed_seconds") > 0);
		REQUIRE(metrics.at("join_finalize_elapsed_seconds") == metrics.at("join_finalize_build_elapsed_seconds"));
		REQUIRE(metrics.at("prf_build_elapsed_seconds") == 0);
		// Repeated snapshots must not add the same completed builds again.
		con.GetProfilingInformation(ProfilerPrintFormat::JSON);
		con.GetProfilingInformation(ProfilerPrintFormat::JSON);
		REQUIRE(metrics.at("finalize_count") == (reuse ? 1 : 4));
	}
}

TEST_CASE("Runtime join filter PRF timing survives recursive rebuilds and reuse", "[api][profiling]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='join_order,build_side_probe_side'"));
	REQUIRE_NO_FAIL(con.Query("SET enable_perfect_hash_join_filter_pushdown=false"));
	REQUIRE_NO_FAIL(con.Query("SET enable_join_min_max_filter_pushdown=false"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE recursive_probe AS SELECT i k FROM range(5000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE recursive_build AS SELECT * FROM recursive_probe"));
	con.EnableProfiling();
	con.context->config.emit_profiler_output = false;
	for (auto compress : {false, true}) {
		REQUIRE_NO_FAIL(
		    con.Query(string("SET enable_prefix_range_filter_compression=") + (compress ? "true" : "false")));
		for (auto reuse : {false, true}) {
			CAPTURE(compress, reuse);
			const string query =
			    reuse ? "WITH RECURSIVE t(depth,n) AS (SELECT 0,0::BIGINT UNION ALL SELECT t.depth+1, counts.n "
			            "FROM t CROSS JOIN (SELECT count(*) n FROM recursive_probe p JOIN recursive_build b "
			            "ON p.k=b.k AND p.k%7>=b.k%7) counts WHERE t.depth<2) SELECT sum(n) FROM t"
			          : "WITH RECURSIVE t(k,depth) AS (SELECT k,0 FROM recursive_build UNION ALL "
			            "SELECT p.k,t.depth+1 FROM recursive_probe p JOIN t ON p.k=t.k AND p.k%7>=t.k%7 "
			            "WHERE t.depth<2) SELECT count(*) FROM t";
			auto result = con.Query(query);
			REQUIRE_NO_FAIL(*result);
			REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == (reuse ? 10000 : 15000));
			auto node = FindJoinFilterProfile(*con.GetProfilingTree());
			REQUIRE(node);
			auto &metrics = node->json_numeric_metrics.at("join_filter");
			REQUIRE(metrics.at("finalize_count") == (reuse ? 1 : 3));
			REQUIRE(metrics.at("prf_build_count") == (reuse ? 1 : 2));
			REQUIRE(metrics.at("prf_build_elapsed_seconds") > 0);
			REQUIRE(metrics.at("prf_build_elapsed_seconds") <= metrics.at("join_finalize_build_elapsed_seconds"));
			REQUIRE((metrics.at("prf_compression_elapsed_seconds") > 0) == compress);
			REQUIRE(metrics.at("prf_analysis_elapsed_seconds") == 0);
			const auto build_time = metrics.at("prf_build_elapsed_seconds");
			con.GetProfilingInformation(ProfilerPrintFormat::JSON);
			con.GetProfilingInformation(ProfilerPrintFormat::JSON);
			REQUIRE(metrics.at("prf_build_elapsed_seconds") == build_time);
		}
	}
}

TEST_CASE("Runtime join filter common PRF scan handles nullable build input", "[api][profiling]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1; SET disabled_optimizers='join_order,build_side_probe_side'"));
	REQUIRE_NO_FAIL(con.Query("SET enable_perfect_hash_join_filter_pushdown=false"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE nullable_build AS SELECT NULL::BIGINT k FROM range(10000) "
	                          "UNION ALL SELECT i FROM range(5000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE nullable_probe AS SELECT i k FROM range(10000) t(i)"));
	con.EnableProfiling();
	con.context->config.emit_profiler_output = false;
	for (auto compress : {false, true}) {
		REQUIRE_NO_FAIL(
		    con.Query(string("SET enable_prefix_range_filter_compression=") + (compress ? "true" : "false")));
		auto result = con.Query("SELECT count(*), count(p.k) FROM nullable_probe p JOIN nullable_build b "
		                        "ON p.k=b.k AND p.k%7>=b.k%7");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 5000);
		REQUIRE(result->GetValue(1, 0).GetValue<int64_t>() == 5000);
		auto node = FindJoinFilterProfile(*con.GetProfilingTree());
		REQUIRE(node);
		REQUIRE(node->json_numeric_metrics.at("join_filter").at("prf_build_count") == 1);
	}
}

TEST_CASE("Test query profiler", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	string output;

	con.EnableProfiling();
	// don't pollute the console with profiler info.
	con.context->config.emit_profiler_output = false;

	string query = "SELECT * FROM (SELECT 42) tbl1, (SELECT 33) tbl2";
	REQUIRE_NO_FAIL(con.Query(query));

	output = con.GetProfilingInformation();
	REQUIRE(output.size() > 0);
	bool query_found_in_output = output.find(query) != std::string::npos;
	REQUIRE(query_found_in_output);

	output = con.GetProfilingInformation(ProfilerPrintFormat::JSON);
	REQUIRE(output.size() > 0);
	query_found_in_output = output.find(query) != std::string::npos;
	REQUIRE(query_found_in_output);
}

TEST_CASE("Test query profiler, no query in the profiling output.", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	string output;

	con.EnableProfiling();
	// don't pollute the console with profiler info.
	con.context->config.emit_profiler_output = false;

	// Disable `QUERY_NAME` in profiling output.
	REQUIRE_NO_FAIL(con.Query(R"(PRAGMA custom_profiling_settings = '{"QUERY_NAME": "false"}')"));
	string query = "SELECT * FROM (SELECT 42) tbl1, (SELECT 33) tbl2";
	REQUIRE_NO_FAIL(con.Query(query));

	output = con.GetProfilingInformation();
	REQUIRE(output.size() > 0);
	bool query_not_found_in_output = output.find(query) == std::string::npos;
	REQUIRE(query_not_found_in_output);

	output = con.GetProfilingInformation(ProfilerPrintFormat::JSON);
	REQUIRE(output.size() > 0);
	query_not_found_in_output = output.find(query) == std::string::npos;
	REQUIRE(query_not_found_in_output);
}

TEST_CASE("Test latency when interrupting query", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	con.EnableProfiling();

	con.context->config.emit_profiler_output = false;

	// Test interupting a query and running a new one afterward.
	// The latency should reflect the new one.
	std::thread t([&con]() {
		string query = "explain analyze select sum(range) from range(1_000_000_000);";
		con.Query(query);
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	con.Interrupt();
	t.join();

	string query = "explain analyze select 42;";
	REQUIRE_NO_FAIL(con.Query(query));

	auto profiling_info = con.GetProfilingTree()->GetProfilingInfo();
	auto latency = profiling_info.GetMetricValue<double>(MetricType::LATENCY);
	auto query_name = profiling_info.GetMetricValue<string>(MetricType::QUERY_NAME);
	REQUIRE(query == query_name);
	REQUIRE(latency > 0);
	REQUIRE(latency < 0.1);
}
