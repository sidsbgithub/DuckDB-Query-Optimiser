PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=waddle
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

test test_debug test_reldebug: check-imdb

.PHONY: check-imdb
check-imdb:
	@test -f data/imdb.duckdb || { \
	  echo "data/imdb.duckdb is missing - test/sql/benchmark.test cannot run."; \
	  echo "Download it once with: python3 ./scripts/download-imdb.py"; \
	  exit 1; }

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: benchmark
benchmark:
	python3 ./scripts/benchmark.py $(BENCH_ARGS)
