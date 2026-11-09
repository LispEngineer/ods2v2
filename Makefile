# ods2v2 - from-scratch ODS2 implementation
#
# v1.3
#
# Sanitizers are ON by default for dev builds to catch my bad bugs.
# `make release` builds without them for a normal, fast binary
# once things are stable.
#
# DEPENDENCY NOTE: the main `ods2` tool uses a BSD copy of
# linenoise (third_party/linenoise/, BSD licensed, single .c/.h file)
# for interactive-mode command history and line editing (up/down
# arrows, Ctrl-A/E/W). Vendored rather than linked as a system
# library specifically so `make` keeps working with zero external
# dependencies everywhere - no separate install step on any platform.

CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -Iinclude -g
SANFLAGS = -fsanitize=address,undefined
BUILD    = build

# Shared sources for any tool that mounts/reads/writes a volume
# through the full ods2_volume.h API - most tools need this complete
# set even if they don't exercise every piece of it.
VOLUME_SRCS = src/ods5_names.c src/ods5_header.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c \
              src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c \
              src/ods2_header_build.c src/ods2_directory_write.c src/ods2_time.c \
              src/ods2_text_var.c src/ods2_records.c src/ods2_acl.c

TOOLS = ods2 ods2_init read_home_block read_file_header ods2_ls ods2_cat ods2_mkdir \
        ods2_put ods2_check_headers ods2_dump_root ods2_dump_header ods2_rm ods2_filedump

# `ods2` is the actual, intended day-to-day interface (DIR,
# CREATE/DIRECTORY, COPY, TYPE, DELETE, SET DEFAULT - everything a
# normal user needs, all in one binary). Built by default via
#  make test / make all / make tools.
MAIN_TOOLS = ods2 ods2_init

# These ten tools are single-purpose tools used while building this
# project - each testing or inspecting one specific piece in
# isolation. See README.md - not built by default - `make dev-tools` builds them.
DEV_TOOLS = read_home_block read_file_header ods2_ls ods2_cat ods2_mkdir \
            ods2_put ods2_check_headers ods2_dump_root ods2_dump_header ods2_rm ods2_filedump

$(BUILD)/ods2_make_loop: tests/ods2_make_loop.c $(VOLUME_SRCS) include/ods2_volume.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_make_loop.c $(VOLUME_SRCS) -o $@

TESTS = $(BUILD)/ods2_ondisk_selftest $(BUILD)/ods2_checksum_selftest $(BUILD)/ods2_validate_selftest $(BUILD)/ods2_bitmap_selftest $(BUILD)/ods2_indexf_selftest $(BUILD)/ods2_retrieval_selftest $(BUILD)/ods2_root_header_selftest $(BUILD)/ods2_directory_selftest $(BUILD)/ods2_volume_selftest $(BUILD)/ods2_wildcard_selftest $(BUILD)/ods2_read_file_selftest $(BUILD)/ods2_header_build_selftest $(BUILD)/ods2_directory_write_selftest $(BUILD)/ods2_path_selftest $(BUILD)/ods2_versions_selftest $(BUILD)/ods2_time_selftest $(BUILD)/ods2_file_alloc_selftest $(BUILD)/ods2_dir_order_selftest $(BUILD)/ods2_dir_eof_selftest $(BUILD)/ods2_large_file_selftest $(BUILD)/ods2_record_attrs_selftest $(BUILD)/ods2_indexf_large_selftest $(BUILD)/ods2_text_var_selftest $(BUILD)/ods5_names_selftest $(BUILD)/ods5_directory_selftest $(BUILD)/ods5_header_selftest $(BUILD)/ods5_indexf_chain_selftest $(BUILD)/ods5_write_selftest $(BUILD)/ods5_review_selftest $(BUILD)/ods2_records_selftest $(BUILD)/ods2_acl_selftest

.PHONY: all test test-large tools dev-tools release clean starter-volume

all: test tools dev-tools starter-volume

# STARTER VOLUME: a genuine, empty ODS-2 volume, real VMS 7.1's own
# INITIALIZE (RA92 geometry, 2,940,951 blocks)
#
# `make starter-volume` decompresses it to ./transfer.dsk in the
# current directory. Won't overwrite an existing transfer.dsk

starter-volume: samples/empty_ods2_volume.dsk.gz
	@if [ -e transfer.dsk ]; then \
		echo "transfer.dsk already exists - not overwriting it. Move it aside" \
			"first, or gunzip -c samples/empty_ods2_volume.dsk.gz > some_other_name.dsk"; \
		exit 1; \
	fi
	gunzip -c samples/empty_ods2_volume.dsk.gz > transfer.dsk
	@echo "Created transfer.dsk - a fresh, empty, real-VMS-INITIALIZE'd ODS-2 volume."
	@echo "Try: ./ods2 transfer.dsk DIR"

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ods2_ondisk_selftest: tests/ods2_ondisk_selftest.c include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $< -o $@

$(BUILD)/ods2_checksum_selftest: tests/ods2_checksum_selftest.c src/ods2_checksum.c include/ods2_checksum.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_checksum_selftest.c src/ods2_checksum.c -o $@

$(BUILD)/ods2_validate_selftest: tests/ods2_validate_selftest.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c include/ods2_validate.h include/ods2_ondisk.h include/ods2_checksum.h include/ods2_retrieval.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_validate_selftest.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_bitmap_selftest: tests/ods2_bitmap_selftest.c src/ods2_bitmap.c include/ods2_bitmap.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_bitmap_selftest.c src/ods2_bitmap.c -o $@

$(BUILD)/ods2_indexf_selftest: tests/ods2_indexf_selftest.c src/ods2_indexf.c include/ods2_indexf.h include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_indexf_selftest.c src/ods2_indexf.c -o $@

$(BUILD)/ods2_retrieval_selftest: tests/ods2_retrieval_selftest.c src/ods2_retrieval.c include/ods2_retrieval.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_retrieval_selftest.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_root_header_selftest: tests/ods2_root_header_selftest.c src/ods2_retrieval.c include/ods2_retrieval.h include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_root_header_selftest.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_directory_selftest: tests/ods2_directory_selftest.c src/ods2_directory.c include/ods2_directory.h include/ods2_ondisk.h src/ods5_names.c src/ods5_fold_table.h include/ods5_names.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_directory_selftest.c src/ods2_directory.c src/ods5_names.c -o $@

samples/synthetic_disk.img: samples/sample.bin samples/indexf_headers.bin samples/root_dir.bin tools/build_synthetic_disk.py
	python3 tools/build_synthetic_disk.py

$(BUILD)/ods2_volume_selftest: tests/ods2_volume_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c src/ods2_time.c include/ods2_volume.h include/ods2_time.h samples/synthetic_disk.img src/ods5_names.c src/ods5_header.c src/ods5_fold_table.h include/ods5_names.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_volume_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c src/ods2_time.c src/ods5_names.c src/ods5_header.c -o $@

$(BUILD)/ods2_wildcard_selftest: tests/ods2_wildcard_selftest.c src/ods2_wildcard.c include/ods2_wildcard.h src/ods5_names.c src/ods5_fold_table.h include/ods5_names.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_wildcard_selftest.c src/ods2_wildcard.c src/ods5_names.c -o $@

$(BUILD)/ods2_read_file_selftest: tests/ods2_read_file_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c src/ods2_time.c include/ods2_volume.h include/ods2_time.h src/ods5_names.c src/ods5_header.c src/ods5_fold_table.h include/ods5_names.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_read_file_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c src/ods2_time.c src/ods5_names.c src/ods5_header.c -o $@

$(BUILD)/ods2_header_build_selftest: tests/ods2_header_build_selftest.c src/ods2_header_build.c src/ods5_names.c src/ods2_time.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c include/ods2_header_build.h include/ods2_time.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_header_build_selftest.c src/ods2_header_build.c src/ods5_names.c src/ods2_time.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_directory_write_selftest: tests/ods2_directory_write_selftest.c src/ods2_directory_write.c src/ods2_directory.c include/ods2_directory_write.h include/ods2_directory.h src/ods5_names.c src/ods5_fold_table.h include/ods5_names.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_directory_write_selftest.c src/ods2_directory_write.c src/ods2_directory.c src/ods5_names.c -o $@

$(BUILD)/ods2_path_selftest: tests/ods2_path_selftest.c src/ods2_path.c src/ods2_wildcard.c include/ods2_path.h include/ods2_wildcard.h src/ods5_names.c src/ods5_fold_table.h include/ods5_names.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_path_selftest.c src/ods2_path.c src/ods2_wildcard.c src/ods5_names.c -o $@

$(BUILD)/ods2_versions_selftest: tests/ods2_versions_selftest.c $(VOLUME_SRCS) include/ods2_volume.h include/ods2_directory.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_versions_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods5_names_selftest: tests/ods5_names_selftest.c src/ods5_names.c src/ods2_wildcard.c include/ods5_names.h include/ods2_wildcard.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods5_names_selftest.c src/ods5_names.c src/ods2_wildcard.c -o $@

$(BUILD)/ods5_directory_selftest: tests/ods5_directory_selftest.c src/ods2_directory.c src/ods2_wildcard.c src/ods2_path.c src/ods5_names.c include/ods2_directory.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods5_directory_selftest.c src/ods2_directory.c src/ods2_wildcard.c src/ods2_path.c src/ods5_names.c -o $@

$(BUILD)/ods5_header_selftest: tests/ods5_header_selftest.c src/ods5_header.c src/ods5_names.c include/ods5_header.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods5_header_selftest.c src/ods5_header.c src/ods5_names.c -o $@

$(BUILD)/ods5_indexf_chain_selftest: tests/ods5_indexf_chain_selftest.c $(VOLUME_SRCS) include/ods2_volume.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods5_indexf_chain_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods5_write_selftest: tests/ods5_write_selftest.c src/ods2_init.c include/ods2_init.h $(VOLUME_SRCS) include/ods2_volume.h include/ods5_names.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods5_write_selftest.c src/ods2_init.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods5_review_selftest: tests/ods5_review_selftest.c src/ods2_init.c include/ods2_init.h $(VOLUME_SRCS) include/ods2_volume.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods5_review_selftest.c src/ods2_init.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods2_time_selftest: tests/ods2_time_selftest.c src/ods2_time.c include/ods2_time.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_time_selftest.c src/ods2_time.c -o $@

$(BUILD)/ods2_file_alloc_selftest: tests/ods2_file_alloc_selftest.c $(VOLUME_SRCS) include/ods2_volume.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_file_alloc_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods2_dir_order_selftest: tests/ods2_dir_order_selftest.c $(VOLUME_SRCS) include/ods2_volume.h include/ods2_directory.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_dir_order_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods2_dir_eof_selftest: tests/ods2_dir_eof_selftest.c $(VOLUME_SRCS) include/ods2_volume.h include/ods2_directory.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_dir_eof_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods2_large_file_selftest: tests/ods2_large_file_selftest.c $(VOLUME_SRCS) include/ods2_volume.h include/ods2_header_build.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_large_file_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods2_record_attrs_selftest: tests/ods2_record_attrs_selftest.c $(VOLUME_SRCS) include/ods2_volume.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_record_attrs_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods2_indexf_large_selftest: tests/ods2_indexf_large_selftest.c $(VOLUME_SRCS) include/ods2_volume.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) -D_DEFAULT_SOURCE tests/ods2_indexf_large_selftest.c $(VOLUME_SRCS) -o $@

$(BUILD)/ods2_acl_selftest: tests/ods2_acl_selftest.c src/ods2_acl.c include/ods2_acl.h include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_acl_selftest.c src/ods2_acl.c -o $@

$(BUILD)/ods2_records_selftest: tests/ods2_records_selftest.c src/ods2_records.c include/ods2_records.h include/ods2_volume.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_records_selftest.c src/ods2_records.c -o $@

$(BUILD)/ods2_text_var_selftest: tests/ods2_text_var_selftest.c $(VOLUME_SRCS) include/ods2_text_var.h include/ods2_volume.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_text_var_selftest.c $(VOLUME_SRCS) -o $@

ods2_init: tools/ods2_init.c src/ods2_init.c include/ods2_init.h $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_init.c src/ods2_init.c $(VOLUME_SRCS) -o $@

# --- CLI tools (built directly into the project root, e.g. ./ods2_ls,
#     not into build/, since that's where they're actually run from) ---

tools: $(MAIN_TOOLS)

dev-tools: $(DEV_TOOLS)

read_home_block: tools/read_home_block.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c include/ods2_validate.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/read_home_block.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c -o $@

read_file_header: tools/read_file_header.c include/ods2_ondisk.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/read_file_header.c -o $@

ods2_ls: tools/ods2_ls.c $(VOLUME_SRCS) src/ods2_wildcard.c include/ods2_volume.h include/ods2_wildcard.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_ls.c $(VOLUME_SRCS) src/ods2_wildcard.c -o $@

ods2_filedump: tools/ods2_filedump.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_filedump.c $(VOLUME_SRCS) -o $@

ods2_cat: tools/ods2_cat.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c include/ods2_volume.h include/ods2_path.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_cat.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c -o $@

ods2_mkdir: tools/ods2_mkdir.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_mkdir.c $(VOLUME_SRCS) -o $@

ods2_put: tools/ods2_put.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_put.c $(VOLUME_SRCS) -o $@

ods2_check_headers: tools/ods2_check_headers.c $(VOLUME_SRCS) include/ods2_volume.h include/ods2_validate.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_check_headers.c $(VOLUME_SRCS) -o $@

ods2_dump_root: tools/ods2_dump_root.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_dump_root.c $(VOLUME_SRCS) -o $@

ods2_dump_header: tools/ods2_dump_header.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_dump_header.c $(VOLUME_SRCS) -o $@

ods2_rm: tools/ods2_rm.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c include/ods2_volume.h include/ods2_path.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_rm.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c -o $@

ods2: tools/ods2.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c third_party/linenoise/linenoise.c include/ods2_volume.h include/ods2_text_var.h include/ods2_records.h include/ods2_acl.h include/ods2_path.h include/ods2_wildcard.h third_party/linenoise/linenoise.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Ithird_party/linenoise $(SANFLAGS) tools/ods2.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c third_party/linenoise/linenoise.c -o $@

# The same CLI with the fault-injection hook (ODS2_FAULT_AFTER_WRITES, see src/ods2_volume.c), for
# tests/fault_inject.py only: the ods2 people run has no hook.
ods2_fault: tools/ods2.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c third_party/linenoise/linenoise.c include/ods2_volume.h include/ods2_text_var.h include/ods2_records.h include/ods2_acl.h include/ods2_path.h include/ods2_wildcard.h third_party/linenoise/linenoise.h
	$(CC) $(CFLAGS) -DODS2_FAULT_INJECTION -D_DEFAULT_SOURCE -Ithird_party/linenoise $(SANFLAGS) tools/ods2.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c third_party/linenoise/linenoise.c -o $@

# Makes trees deeper than the 255 levels ods2 creates (ODS2_TEST_MAX_DIR_DEPTH, in this test build only), for
# tests/cli_deep_check.sh.
ods2_deep_make: tests/ods2_deep_make.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) -DODS2_FAULT_INJECTION $(SANFLAGS) tests/ods2_deep_make.c $(VOLUME_SRCS) -o $@

test: $(TESTS) $(BUILD)/ods2_make_loop tools ods2_put ods2_dump_header ods2_fault ods2_deep_make
	@echo "--- ods2_ondisk_selftest ---"
	@$(BUILD)/ods2_ondisk_selftest
	@echo "--- ods2_checksum_selftest ---"
	@$(BUILD)/ods2_checksum_selftest
	@echo "--- ods2_validate_selftest ---"
	@$(BUILD)/ods2_validate_selftest
	@echo "--- ods2_bitmap_selftest ---"
	@$(BUILD)/ods2_bitmap_selftest
	@echo "--- ods2_indexf_selftest ---"
	@$(BUILD)/ods2_indexf_selftest
	@echo "--- ods2_retrieval_selftest ---"
	@$(BUILD)/ods2_retrieval_selftest
	@echo "--- ods2_root_header_selftest ---"
	@$(BUILD)/ods2_root_header_selftest
	@echo "--- ods2_directory_selftest ---"
	@$(BUILD)/ods2_directory_selftest
	@echo "--- ods2_volume_selftest ---"
	@ODS2_NOLOCK=1 $(BUILD)/ods2_volume_selftest   # it holds one read-only mount while it mounts the same image for write
	@echo "--- ods2_wildcard_selftest ---"
	@$(BUILD)/ods2_wildcard_selftest
	@echo "--- ods2_read_file_selftest ---"
	@$(BUILD)/ods2_read_file_selftest
	@echo "--- ods2_header_build_selftest ---"
	@$(BUILD)/ods2_header_build_selftest
	@echo "--- ods2_directory_write_selftest ---"
	@$(BUILD)/ods2_directory_write_selftest
	@echo "--- ods2_path_selftest ---"
	@$(BUILD)/ods2_path_selftest
	@echo "--- ods2_versions_selftest ---"
	@$(BUILD)/ods2_versions_selftest
	@echo "--- ods2_time_selftest ---"
	@$(BUILD)/ods2_time_selftest
	@echo "--- ods2_file_alloc_selftest ---"
	@$(BUILD)/ods2_file_alloc_selftest
	@echo "--- ods2_dir_order_selftest ---"
	@$(BUILD)/ods2_dir_order_selftest
	@echo "--- ods2_dir_eof_selftest ---"
	@$(BUILD)/ods2_dir_eof_selftest
	@echo "--- ods2_large_file_selftest ---"
	@$(BUILD)/ods2_large_file_selftest
	@echo "--- ods2_record_attrs_selftest ---"
	@$(BUILD)/ods2_record_attrs_selftest
	@echo "--- ods2_indexf_large_selftest ---"
	@$(BUILD)/ods2_indexf_large_selftest
	@echo "--- ods2_acl_selftest ---"
	@$(BUILD)/ods2_acl_selftest
	@echo "--- ods2_records_selftest ---"
	@$(BUILD)/ods2_records_selftest
	@echo "--- ods2_text_var_selftest ---"
	@$(BUILD)/ods2_text_var_selftest
	@echo "--- ods5_names_selftest ---"
	@$(BUILD)/ods5_names_selftest
	@echo "--- ods5_directory_selftest ---"
	@$(BUILD)/ods5_directory_selftest
	@echo "--- ods5_header_selftest ---"
	@$(BUILD)/ods5_header_selftest
	@echo "--- ods5_indexf_chain_selftest ---"
	@$(BUILD)/ods5_indexf_chain_selftest
	@echo "--- ods5_write_selftest ---"
	@$(BUILD)/ods5_write_selftest
	@echo "--- ods5_review_selftest ---"
	@$(BUILD)/ods5_review_selftest
	@echo "--- CLI file version checks ---"
	@sh tests/cli_versions_check.sh
	@echo "--- CLI directory wildcard checks ---"
	@sh tests/cli_dir_wildcard_check.sh
	@echo "--- CLI directory loop checks ---"
	@sh tests/cli_dir_loop_check.sh
	@echo "--- CLI large file checks ---"
	@sh tests/cli_large_file_check.sh
	@echo "--- CLI exit status checks ---"
	@sh tests/cli_exit_status_check.sh
	@echo "--- CLI record attribute checks ---"
	@sh tests/cli_record_attrs_check.sh
	@echo "--- CLI volume label check ---"
	@sh tests/cli_volume_label_check.sh
	@echo "--- CLI ODS-5 volume check ---"
	@sh tests/cli_ods5_volume_check.sh
	@echo "--- CLI ODS-5 names check ---"
	@sh tests/cli_ods5_names_check.sh
	@echo "--- CLI ODS-5 files check ---"
	@sh tests/cli_ods5_files_check.sh
	@echo "--- cli_ods5_write_check ---"
	@sh tests/cli_ods5_write_check.sh
	@echo "--- cli_ods5_init_check ---"
	@sh tests/cli_ods5_init_check.sh
	@echo "--- cli_nmx_check (file numbers above 65535) ---"
	@sh tests/cli_nmx_check.sh
	@echo "--- cli_ods5_integrity_check ---"
	@sh tests/cli_ods5_integrity_check.sh
	@echo "--- cli_deep_check (trees deeper than 255 levels) ---"
	@sh tests/cli_deep_check.sh
	@echo "--- fault_inject (interrupted writes) ---"
	@python3 tests/fault_inject.py --quick
	@echo "--- cli_ods5_review_check ---"
	@sh tests/cli_ods5_review_check.sh
	@echo "--- cli_records_check ---"
	@sh tests/cli_records_check.sh
	@echo "--- CLI recursive DIR regression check ---"
	@if command -v timeout >/dev/null 2>&1; then \
		rm -f samples/cli_regression_disk.img; \
		cp samples/synthetic_disk.img samples/cli_regression_disk.img; \
		output=$$(timeout 10 ./ods2 samples/cli_regression_disk.img DIR "[...]*.*" 2>&1); \
		rc=$$?; \
		if [ $$rc -eq 124 ]; then \
			echo "FAIL: recursive DIR timed out - likely an infinite recursion regression" \
				"(root's self-referential 000000.DIR entry being treated as a real" \
				"subdirectory - see list_directory_recursive() in tools/ods2.c)"; \
			exit 1; \
		fi; \
		count=$$(echo "$$output" | grep -c "^Directory \[000000\]$$"); \
		if [ "$$count" -ne 1 ]; then \
			echo "FAIL: root listed $$count times during recursive DIR (expected exactly 1) -" \
				"likely the same self-reference regression"; \
			exit 1; \
		fi; \
		rm -f samples/cli_regression_disk.img; \
		echo "PASS: recursive DIR on root terminates correctly, root listed exactly once" \
			"(regression check for a real infinite-recursion bug found via real testing)"; \
	else \
		echo "SKIP: 'timeout' command not available on this platform - this check" \
			"needs it to safely test for infinite recursion without hanging the build"; \
	fi
	@echo "--- all tests passed ---"
	@echo ""
	@echo "Tools built: $(MAIN_TOOLS)"
	@echo "(Additional single-purpose/diagnostic tools available via 'make dev-tools':"
	@echo " $(DEV_TOOLS))"

# 75,000 real files on ODS-2 and ODS-5 (file numbers past 65535); about 3 minutes
test-large: tools ods2_dump_header
	@echo "--- cli_nmx_big_check ---"
	@sh tests/cli_nmx_big_check.sh

release: CFLAGS += -O2
release: SANFLAGS =
release: test

clean:
	rm -rf $(BUILD)
	rm -f samples/synthetic_disk.img samples/synthetic_disk_working.img samples/synthetic_disk_v0.img
	rm -f samples/versions_disk.img samples/versions_cli_disk.img samples/cli_wildcard_disk.img samples/cli_loop_disk.img samples/file_alloc_disk.img samples/dir_order_disk.img samples/dir_eof_disk.img samples/large_file_disk.img samples/cli_status_disk.img samples/cli_attrs_disk.img samples/record_attrs_disk.img samples/indexf_large_disk.img samples/text_var_disk.img samples/cli_label_disk.img samples/cli_label_orig.img
	rm -f $(TOOLS) ods2_fault ods2_deep_make
