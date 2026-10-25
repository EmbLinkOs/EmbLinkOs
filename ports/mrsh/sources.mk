# Pinned mrsh parser/evaluator, with an explicit-spawn backend.
MRSH_ROOT := third_party/mrsh
MRSH_SRCS := $(wildcard $(MRSH_ROOT)/*.c $(MRSH_ROOT)/parser/*.c $(MRSH_ROOT)/builtin/*.c $(MRSH_ROOT)/shell/*.c $(MRSH_ROOT)/shell/task/*.c) $(MRSH_ROOT)/frontend/basic.c ports/mrsh/native.c ports/mrsh/ast_codec.c ports/mrsh/utilities.c third_party/newlib-posix/fnmatch.c third_party/newlib-posix/glob.c
MRSH_INC := -I$(MRSH_ROOT)/include -Iports/mrsh -Ithird_party/newlib-posix
MRSH_OBJS := $(patsubst %.c,build/mrsh/%.o,$(MRSH_SRCS))
build/mrsh/%.o: %.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(NEWLIB_CFLAGS) -std=gnu11 -DEMBLINK_TARGET -D_ELIX_LEVEL=1 -DARG_MAX=4096 $(MRSH_INC) -c $< -o $@
build/sh.elf: $(MRSH_OBJS) build/crt0.o build/syscalls.o $(NEWLIB_LDSCRIPT)
	$(USER_CC) $(NEWLIB_LDFLAGS) build/crt0.o build/syscalls.o $(MRSH_OBJS) -lc -lgcc -o $@
.PHONY: posixsh-host
posixsh-host: | $(BUILD)
	$(HOSTCC) -std=c11 -D_DEFAULT_SOURCE -D_ELIX_LEVEL=1 -DARG_MAX=4096 -O1 -g -Wall -Wextra -Wno-unused-parameter $(MRSH_INC) $(MRSH_SRCS) -o $(BUILD)/posixsh-host

-include $(addsuffix .d,$(MRSH_OBJS))

.PHONY: posixsh-test
posixsh-test: posixsh-host
	python3 tests/posixsh/generate.py --check
	python3 tests/posixsh/host.py
	python3 tests/posixsh/worker_host.py
build/shprobe.o: tests/posixsh/cases.h

MRSH_CODEC_SRCS := $(addprefix $(MRSH_ROOT)/,array.c ast.c buffer.c arithm.c hashtable.c parser/parser.c parser/program.c parser/word.c parser/arithm.c) ports/mrsh/ast_codec.c tests/posixsh/codec_host.c
.PHONY: posixsh-sanitize-test
posixsh-sanitize-test: | $(BUILD)
	$(HOSTCC) -std=c11 -D_DEFAULT_SOURCE -D_ELIX_LEVEL=1 -DARG_MAX=4096 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(MRSH_INC) $(MRSH_SRCS) -o $(BUILD)/posixsh-sanitize
	ASAN_OPTIONS=detect_leaks=1 POSIXSH_BINARY=$(CURDIR)/$(BUILD)/posixsh-sanitize python3 tests/posixsh/host.py
	ASAN_OPTIONS=detect_leaks=1 POSIXSH_BINARY=$(CURDIR)/$(BUILD)/posixsh-sanitize python3 tests/posixsh/worker_host.py
	$(HOSTCC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(MRSH_INC) -Itests/posixsh $(MRSH_CODEC_SRCS) -o $(BUILD)/sh-codec-sanitize
	ASAN_OPTIONS=detect_leaks=1 $(BUILD)/sh-codec-sanitize
