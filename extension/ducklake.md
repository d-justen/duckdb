# Building DuckLake with this branch

DuckLake is an out-of-tree extension. Its source revision is pinned in
`.github/config/extensions/ducklake.cmake`; compatibility changes that have not yet landed in DuckLake are stored in
`.github/patches/extensions/ducklake`.

Set up vcpkg once:

```shell
make setup-vcpkg
```

Fetch the pinned source into the ignored `extension/external/ducklake` directory and apply the checked-in patches:

```shell
DUCKDB_NEW_EXTENSION_BUILD=1 \
EXTENSION_CONFIGS=.github/config/extensions/ducklake.cmake \
VCPKG_TOOLCHAIN_PATH="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
make sync_out_of_tree_extensions
```

Build DuckDB, DuckLake, and the DuckLake SQL tests together:

```shell
DUCKDB_NEW_EXTENSION_BUILD=1 \
EXTENSION_CONFIGS=.github/config/extensions/ducklake.cmake \
VCPKG_TOOLCHAIN_PATH="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
make release
```

The extension is linked into the resulting CLI. It can be loaded and attached normally:

```sql
LOAD ducklake;
ATTACH 'ducklake:metadata.db' AS lake (DATA_PATH 'data');
```

Run the PRF file-pruning test with:

```shell
./build/release/test/unittest \
  "$PWD/extension/external/ducklake/test/sql/stats/prefix_range_filter_file_pruning.test"
```

## Updating the DuckLake patch

The fetched DuckLake checkout contains one commit for every patch in `.github/patches/extensions/ducklake`. Make changes in
`extension/external/ducklake`, then commit the new change with its intended patch filename as the complete commit message,
for example `0008-prefix-range-file-pruning.patch`. Export the commits back into this repository with:

```shell
DUCKDB_NEW_EXTENSION_BUILD=1 \
EXTENSION_CONFIGS=.github/config/extensions/ducklake.cmake \
VCPKG_TOOLCHAIN_PATH="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
EXPORT_EXTENSION_PATCHES=1 \
make sync_out_of_tree_extensions
```

Do not advance the pinned DuckLake revision without rebuilding the extension and running its SQL tests. When the local
changes have landed upstream, advance the pin and remove patches that are included in the new revision.
