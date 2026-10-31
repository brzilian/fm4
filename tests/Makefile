FM4 := ../firmware/app/fm4
CFLAGS := -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -DFM4_HOST -I$(FM4)
CORE := $(FM4)/fm4_core.c
ENGINE := $(filter-out $(CORE) $(FM4)/fm4_main.c $(FM4)/fm4_ui.c,$(wildcard $(FM4)/*.c))
TESTS := engine_test seq_test fx_test multi_test tq5_test presets_test core_test

all: $(TESTS)
	@mkdir -p out
	@for t in $(TESTS); do ./$$t || exit 1; done

core_test: core_test.c $(ENGINE) $(CORE) $(wildcard $(FM4)/*.h)
	$(CC) $(CFLAGS) -o $@ $< $(ENGINE) $(CORE) -lm

%: %.c $(ENGINE) $(wildcard $(FM4)/*.h)
	$(CC) $(CFLAGS) -o $@ $< $(ENGINE) -lm

clean:
	rm -f $(TESTS) out/*
.PHONY: all clean
