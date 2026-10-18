# Harry Potter and the Prisoner of Azkaban (PS2, SLUS-20926) static recompilation
ELF      := disc/SLUS_209.26
OUT      := build
REC      := $(OUT)/recomp
GEN      := $(REC)/gen
PY       := python3 -I

CC       := clang
DEBUGFLAGS ?= -DWATCH
OPT      ?= -O1
CFLAGS   := $(OPT) -g -fno-strict-aliasing -fwrapv -w -Iruntime -I$(GEN) -I/opt/homebrew/include -DRA_CHECK -DTRACE_PC $(DEBUGFLAGS)
LDFLAGS  := -L/opt/homebrew/lib -lSDL3

RT_SRC   := $(wildcard runtime/*.c)
GEN_SRC   = $(wildcard $(GEN)/*.c)
RT_OBJ   := $(patsubst %.c,$(OUT)/obj/%.o,$(RT_SRC))
GEN_OBJ   = $(patsubst $(GEN)/%.c,$(OUT)/obj/gen/%.o,$(GEN_SRC))

.PHONY: all recomp run clean link
all: $(OUT)/hp

recomp: $(GEN)/funcs.h
$(GEN)/funcs.h: recomp/*.py
	$(PY) recomp/main.py $(ELF) $(REC)

$(OUT)/hp: $(GEN)/funcs.h FORCE
	@$(MAKE) --no-print-directory link

link: $(RT_OBJ) $(GEN_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OUT)/hp $^

$(OUT)/obj/%.o: %.c runtime/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -c $< -o $@

$(OUT)/obj/gen/%.o: $(GEN)/%.c runtime/ee.h runtime/ee_gen.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: all
	$(OUT)/hp $(REC)/image.bin disc

clean:
	rm -rf $(OUT)/obj $(OUT)/hp

FORCE:
