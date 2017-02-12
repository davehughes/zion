IMAGE=zionlang/zion
VERSION=0.1
INSTALL_DIR=/usr/local/zion

UNAME := $(shell uname)
DEBUG_FLAGS := -DZION_DEBUG -g -O0
CFLAGS = \
	-c \
	-Wall \
	-Werror \
	-pthread \
	-DZION_DEBUG \
	-g \
	-O0 \
	-fms-extensions \

ifeq ($(UNAME),Darwin)
	CLANG = ccache clang-3.7
	CLANG_CPP = ccache clang++-3.7
	LLVM_CONFIG = llvm-config-3.7
	LLVM_CFLAGS = $(CFLAGS) -nostdinc++ $(shell $(LLVM_CONFIG) --cxxflags) -g -O0

	CPP = $(CLANG_CPP) -g -O0 -std=c++11 -I /usr/include/c++/v1 -I$(shell $(LLVM_CONFIG) --includedir)/c++/v1
	CC = $(CLANG)
	LINKER = $(CLANG)
	LINKER_OPTS := \
		$(DEBUG_FLAGS) \
		$(shell $(LLVM_CONFIG) --ldflags) \
		-stdlib=libc++ \
		-lstdc++ \
		$(shell $(LLVM_CONFIG) --libs) \
		$(shell $(LLVM_CONFIG) --system-libs) \

	LINKER_DEBUG_OPTS := $(DEBUG_FLAGS)
	LLDB = lldb
else

ifeq ($(UNAME),Linux)
	CLANG := ccache clang-3.9
	CLANG_CPP := ccache clang++-3.9
	LLVM_CONFIG := llvm-config-3.9
	LLVM_CFLAGS = $(CFLAGS) \
				  -nostdinc++ \
				  -I/usr/lib/llvm-3.9/include \
				  -I/usr/include/c++/5 \
				  -I/usr/include/x86_64-linux-gnu \
				  -I/usr/include/x86_64-linux-gnu/c++/5 \
				  -std=gnu++11 \
				  -gsplit-dwarf \
				  -fPIC \
				  -fvisibility-inlines-hidden \
				  -Wall \
				  -W \
				  -Wno-unused-parameter \
				  -Wwrite-strings \
				  -Wcast-qual \
				  -Wno-missing-field-initializers \
				  -pedantic \
				  -Wno-long-long \
				  -Wno-uninitialized \
				  -Wdelete-non-virtual-dtor \
				  -Wno-comment \
				  -Werror=date-time \
				  -ffunction-sections \
				  -fdata-sections \
				  -O0 \
				  -g \
				  -DNDEBUG \
				  -fno-exceptions \
				  -D_GNU_SOURCE \
				  -D__STDC_CONSTANT_MACROS \
				  -D__STDC_FORMAT_MACROS \
				  -D__STDC_LIMIT_MACROS \
				  -DLINUX

	# -I$(shell $(LLVM_CONFIG) --includedir)/llvm
	CPP = $(CLANG_CPP) \
		  -I/usr/include/c++/v1 \
		  -g \
		  -O0 \
		  -fno-color-diagnostics \
		  -fno-caret-diagnostics
	CC = $(CLANG)
	LINKER = $(CLANG)
	LINKER_OPTS := \
		$(DEBUG_FLAGS) \
		-lm \
		$(shell $(LLVM_CONFIG) --ldflags) \
		-lstdc++ \
		$(shell $(LLVM_CONFIG) --libs) \
		$(shell $(LLVM_CONFIG) --system-libs) \

	LINKER_DEBUG_OPTS := $(DEBUG_FLAGS)
	LLDB = lldb-3.9
endif

endif

VPATH = .:$(BUILD_DIR)
BUILD_DIR = build-$(UNAME)

ZION_LLVM_SOURCES = \
				main.cpp \
				signature.cpp \
				patterns.cpp \
				types.cpp \
				type_checker.cpp \
				type_instantiation.cpp \
				var.cpp \
				ast.cpp \
				compiler.cpp \
				bound_type.cpp \
				bound_var.cpp \
				callable.cpp \
				dbg.cpp \
				disk.cpp \
				identifier.cpp \
				lexer.cpp \
				llvm_utils.cpp \
				llvm_test.cpp \
				llvm_types.cpp \
				location.cpp \
				logger.cpp \
				mmap_file.cpp \
				parse_state.cpp \
				parser.cpp \
				phase_scope_setup.cpp \
				render.cpp \
				scopes.cpp \
				status.cpp \
				tests.cpp \
				token.cpp \
				token_queue.cpp \
				unification.cpp \
				utils.cpp \
				unchecked_var.cpp \
				unchecked_type.cpp \
				variant.cpp \
				atom.cpp \
				json.cpp \
				json_lexer.cpp \
				json_parser.cpp \

ZION_LLVM_OBJECTS = $(addprefix $(BUILD_DIR)/,$(ZION_LLVM_SOURCES:.cpp=.llvm.o))
ZION_TARGET = zionc
ZION_RUNTIME = \
				rt_int.c \
				rt_fn.c \
				rt_float.c \
				rt_str.c \
				rt_gc.c \
				rt_typeid.c

ZION_RUNTIME_LLIR = $(ZION_RUNTIME:.c=.llir)

TARGETS = $(ZION_TARGET)

timed:
	time make all

all: $(TARGETS) rt_gc

-include $(ZION_LLVM_OBJECTS:.o=.d)

rt_int.c: zion_rt.h

rt_float.c: zion_rt.h

rt_str.c: zion_rt.h

rt_gc.c: zion_rt.h

$(BUILD_DIR)/.gitignore:
	mkdir -p $(BUILD_DIR)
	echo "*" > $(BUILD_DIR)/.gitignore

value_semantics: $(BUILD_DIR)/value_semantics.o
	$(LINKER) $(LINKER_OPTS) $< -o value_semantics

.PHONY: test
test: zionc
	./$(ZION_TARGET) test | tee

.PHONY: test-html
test-html: $(ZION_TARGET)
	COLORIZE=1 ALL_TESTS=1 ./$(ZION_TARGET) test \
		| ansifilter -o /var/tmp/zion-test.html --html -la -F 'Menlo' -e=utf-8
	open /var/tmp/zion-test.html

.PHONY: dbg
dbg: $(ZION_TARGET)
	ALL_TESTS=1 $(LLDB) -s .lldb-script -- ./$(ZION_TARGET) test

$(ZION_TARGET): $(BUILD_DIR)/.gitignore $(ZION_LLVM_OBJECTS) $(ZION_RUNTIME_LLIR)
	@echo Linking $@
	$(LINKER) $(LINKER_OPTS) $(ZION_LLVM_OBJECTS) -o $@
	@echo $@ successfully built
	@ccache -s
	@du -hs $@ | cut -f1 | xargs echo Target \`$@\` is

$(BUILD_DIR)/%.e: %.cpp
	@echo Precompiling $<
	@$(CPP) $(LLVM_CFLAGS) -E $< -o $@

$(BUILD_DIR)/%.llvm.o: %.cpp
	@echo Compiling $<
	@$(CPP) $(LLVM_CFLAGS) $< -E -MMD -MP -MF $(patsubst %.o, %.d, $@) -MT $@ > /dev/null
	@$(CPP) $(LLVM_CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.c
	@echo Compiling $<
	@$(CPP) $(CFLAGS) $< -E -MMD -MP -MF $(patsubst %.o, %.d, $@) -MT $@ > /dev/null
	@$(CC) $(CFLAGS) $< -o $@

%.llir: %.c
	@echo Emitting LLIR from $<
	@$(CLANG) -S -emit-llvm $< -o - | grep -v -e 'llvm\.ident' -e 'Apple LLVM version 6' > $@

rt_gc: rt_gc.c
	$(CLANG) -DRT_GC_TEST -g -std=c11 -Wall -O0 -mcx16 -pthread rt_gc.c -o rt_gc

clean:
	rm -rf $(BUILD_DIR)/* $(TARGETS)

image: Dockerfile
	docker build -t $(IMAGE):$(VERSION) .
	docker tag $(IMAGE):$(VERSION) $(IMAGE):latest

linux-test: image
	docker run \
		--rm \
		--name zion-build \
		-v `pwd`:/opt/zion \
		--privileged \
		-it $(IMAGE):$(VERSION) \
		make -j4 test

linux-shell: image
	docker run \
		--rm \
		--name zion-shell \
		-v `pwd`:/opt/zion \
		--privileged \
		-it $(IMAGE):$(VERSION) \
		bash
