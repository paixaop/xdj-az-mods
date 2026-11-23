# SPDX-License-Identifier: GPL-3.0-only
include src/analysis/build.mk
TEST_ENGINE_OBJ := $(ENGINE_OBJ) $(OUT)/tests/analysis/thread_fixture.o
$(OUT)/fixtures.c: tests/analysis/generate_fixtures.py $(wildcard tests/analysis/fixtures/*.json)
	$(PYTHON) $< $@
$(OUT)/test_phrases: $(OUT)/fixtures.c $(TEST_ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O0 $(LDFLAGS) $^ $(LDLIBS) -o $@
PHRASE_EDGE_PARTS := boundaries classifier fills rhythm features decorators high_boundaries
PHRASE_EDGE_TESTS := $(addprefix tests/analysis/test_phrase_edges_,$(addsuffix .c,$(PHRASE_EDGE_PARTS)))
PHRASE_EDGE_OBJECTS := $(addprefix $(OUT)/src/analysis/phrase_,$(addsuffix .o,$(PHRASE_EDGE_PARTS)))
$(OUT)/test_phrase_edges: tests/analysis/test_phrase_edges.c $(PHRASE_EDGE_TESTS) $(TEST_ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/analysis/test_phrase_edges.c $(PHRASE_EDGE_TESTS) $(filter-out $(PHRASE_EDGE_OBJECTS),$(TEST_ENGINE_OBJ)) $(LDLIBS) -o $@
$(OUT)/test_analysis: tests/analysis/test_analysis.c $(TEST_ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUT)/test_execution: tests/analysis/test_execution.c src/analysis/execution.c $(OUT)/src/analysis/memory.o $(OUT)/vendor/analysis/mimalloc/src/static.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(OUT)/src/analysis/memory.o $(OUT)/vendor/analysis/mimalloc/src/static.o $(LDLIBS) -o $@
$(OUT)/test_vocal_kernels: tests/analysis/test_vocal_kernels.c src/analysis/vocals.c $(TEST_ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(LDFLAGS) $< $(filter-out $(OUT)/src/analysis/vocals.o,$(TEST_ENGINE_OBJ)) $(LDLIBS) -o $@
$(OUT)/test_vocal_frontend: tests/analysis/test_vocal_frontend.c src/analysis/vocals.c $(TEST_ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(LDFLAGS) $< $(filter-out $(OUT)/src/analysis/vocals.o,$(TEST_ENGINE_OBJ)) $(LDLIBS) -o $@
$(OUT)/test_parallel_analysis: tests/analysis/test_parallel_analysis.c $(TEST_ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUT)/test_activation_math: tests/analysis/test_activation_math.c tests/analysis/fixtures/musl-activation-bits.inc $(filter $(OUT)/vendor/analysis/musl-math/%,$(TEST_ENGINE_OBJ))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(filter %.o,$^) $(LDLIBS) -o $@
test: $(OUT)/test_phrase_edges $(OUT)/test_phrases $(OUT)/test_execution $(OUT)/test_vocal_kernels $(OUT)/test_vocal_frontend $(OUT)/test_analysis $(OUT)/test_parallel_analysis $(OUT)/test_activation_math
	$(OUT)/test_phrase_edges
	$(OUT)/test_phrases
	$(OUT)/test_execution
	$(OUT)/test_vocal_kernels
	$(OUT)/test_vocal_frontend
	$(OUT)/test_activation_math
	$(OUT)/test_analysis tests/analysis/fixtures $(ANALYSIS_ASSETS)
	$(OUT)/test_parallel_analysis tests/analysis/fixtures $(ANALYSIS_ASSETS)
clean:
	rm -rf $(OUT)
-include $(ENGINE_OBJ:.o=.d)

$(OUT)/analysis_runtime_test.o: src/azmod/analysis_runtime.c src/azmod/analysis_runtime.h src/azmod/analysis_source.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dclock_gettime=fixture_clock_gettime -Dpthread_cond_timedwait=fixture_condition_wait -c $< -o $@
$(OUT)/test_runtime: tests/analysis/test_runtime.c tests/analysis/test_api.inc src/azmod/analysis_api.c $(OUT)/analysis_runtime_test.o src/azmod/analysis_owner.c $(TEST_ENGINE_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(filter-out %.inc src/azmod/analysis_api.c,$^) $(LDLIBS) -o $@
test-runtime: $(OUT)/test_runtime
	$(OUT)/test_runtime tests/analysis/fixtures/pvdi-voice.wav $(ANALYSIS_ASSETS)
