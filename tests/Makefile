FM4 := ../firmware/app/fm4
CFLAGS := -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -DFM4_HOST -I$(FM4)
ENGINE := $(wildcard $(FM4)/*.c)
TESTS := engine_test seq_test fx_test multi_test

all: $(TESTS)
	@mkdir -p out
	@for t in $(TESTS); do ./$$t || exit 1; done

%: %.c $(ENGINE) $(wildcard $(FM4)/*.h)
	$(CC) $(CFLAGS) -o $@ $< $(ENGINE) -lm

clean:
	rm -f $(TESTS) out/*
.PHONY: all clean
