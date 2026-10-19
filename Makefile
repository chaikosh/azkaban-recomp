# Harry Potter and the Prisoner of Azkaban (PS2, SLUS-20926) static recompilation
ELF      := disc/SLUS_209.26
OUT      := build
REC      := $(OUT)/recomp
GEN      := $(REC)/gen
PY       := python3 -I

CC       := clang
ifeq ($(PROFILE),1)
# release code with per-block pc tracking only (HP_PROF sampling); objects in build/prof
OBJDIR   := $(OUT)/prof
OPT      ?= -O2
CHECKS   := -DTRACE_PC
else ifeq ($(RELEASE),1)
# release: no per-block tracing, return-address checks or watchpoints; objects in build/rel
OBJDIR   := $(OUT)/rel
OPT      ?= -O2
CHECKS   :=
else
OBJDIR   := $(OUT)
DEBUGFLAGS ?= -DWATCH
OPT      ?= -O1
CHECKS   := -DRA_CHECK -DTRACE_PC $(DEBUGFLAGS)
endif
CFLAGS   := $(OPT) -g -fno-strict-aliasing -fwrapv -w -Iruntime -I$(GEN) -I/opt/homebrew/include $(CHECKS)
LDFLAGS  := -L/opt/homebrew/lib -lSDL3

RT_SRC   := $(wildcard runtime/*.c)
# low-level IOP (vendored from ps2-web-recomp, MIT) and its bridge
IOP_DIR  := third_party/ps2iop
CXX      := clang++
CXXFLAGS := -std=c++20 -O2 -g -w -I$(IOP_DIR)/include -I$(IOP_DIR)/src -I$(IOP_DIR)/src/iop
IOP_SRC  := $(wildcard $(IOP_DIR)/src/iop/*.cpp) $(IOP_DIR)/src/disc.cpp runtime/iop_bridge.cpp
# GPU renderer (HP_GS=gpu): ps2-web-recomp's GS (MIT) on wgpu-native (BSD-3/MIT/Apache-2.0)
GS_DIR   := third_party/ps2gs
WGPU_DIR := third_party/wgpu-native
CXXFLAGS += -DPS2_GPU -I$(GS_DIR)/include -I$(GS_DIR)/src -I$(WGPU_DIR)/include
GS_SRC   := $(wildcard $(GS_DIR)/src/gs/*.cpp) $(GS_DIR)/src/gs/gpu/gpu.cpp runtime/gs_bridge.cpp
WGSL_OBJ := $(OBJDIR)/obj/wgsl/gs_wgsl.o $(OBJDIR)/obj/wgsl/blit_wgsl.o
IOP_OBJ  := $(patsubst %.cpp,$(OBJDIR)/obj/%.o,$(IOP_SRC) $(GS_SRC)) $(WGSL_OBJ)
WGPU_LIB := $(WGPU_DIR)/lib/libwgpu_native.a
LDFLAGS  += $(WGPU_LIB) -framework Metal -framework QuartzCore -framework Foundation -framework CoreFoundation \
            -framework IOKit -framework IOSurface -lobjc
GEN_SRC   = $(wildcard $(GEN)/*.c)
RT_OBJ   := $(patsubst %.c,$(OBJDIR)/obj/%.o,$(RT_SRC))
GEN_OBJ   = $(patsubst $(GEN)/%.c,$(OBJDIR)/obj/gen/%.o,$(GEN_SRC))

.PHONY: all recomp run clean link gsreplay
all: $(OUT)/hp

recomp: $(GEN)/funcs.h
$(GEN)/funcs.h: recomp/*.py recomp/hooks.txt
	$(PY) recomp/main.py $(ELF) $(REC)

$(OUT)/hp: $(GEN)/funcs.h FORCE
	@$(MAKE) --no-print-directory link

# VU1 microprograms: code images saved by HP_VUCOLLECT=build/recomp/vu1 -> C (empty table if none)
VU1_GEN  := $(REC)/vu1_gen.c
VU1_OBJ  := $(OBJDIR)/obj/vu1_gen.o
$(VU1_GEN): recomp/vu1rec.py $(wildcard $(REC)/vu1/*)
	$(PY) recomp/vu1rec.py $(REC)/vu1 $@
$(VU1_OBJ): $(VU1_GEN) runtime/vu1_core.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -c $< -o $@

link: $(RT_OBJ) $(GEN_OBJ) $(IOP_OBJ) $(VU1_OBJ) | $(WGPU_LIB)
	$(CXX) $(CFLAGS) $(LDFLAGS) -o $(OBJDIR)/hp $^

$(OBJDIR)/obj/%.o: %.cpp $(IOP_DIR)/src/iop/*.hpp $(GS_DIR)/src/gs/*.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# WGSL shaders embedded as C strings (kGsShader, kBlitShader)
$(OBJDIR)/obj/wgsl/gs_wgsl.cpp: $(GS_DIR)/src/gs/gpu/gs.wgsl
	@mkdir -p $(dir $@)
	{ printf 'extern const char kGsShader[];\nconst char kGsShader[] = R"WGSL('; cat $<; printf ')WGSL";\n'; } > $@
$(OBJDIR)/obj/wgsl/blit_wgsl.cpp: $(GS_DIR)/src/gs/gpu/blit.wgsl
	@mkdir -p $(dir $@)
	{ printf 'extern const char kBlitShader[];\nconst char kBlitShader[] = R"WGSL('; cat $<; printf ')WGSL";\n'; } > $@
$(OBJDIR)/obj/wgsl/%.o: $(OBJDIR)/obj/wgsl/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(WGPU_LIB):
	scripts/fetch_wgpu.sh

$(OBJDIR)/obj/%.o: %.c runtime/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -c $< -o $@

$(OBJDIR)/obj/gen/%.o: $(GEN)/%.c runtime/ee.h runtime/ee_gen.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# tools/gsreplay: replays an HP_GSREC trace through the GPU renderer (frames, timing)
GSR_OBJ  := $(patsubst %.cpp,$(OBJDIR)/obj/%.o,$(wildcard $(GS_DIR)/src/gs/*.cpp) $(GS_DIR)/src/gs/gpu/gpu.cpp tools/gsreplay.cpp) $(WGSL_OBJ)
gsreplay: $(OBJDIR)/gsreplay
$(OBJDIR)/gsreplay: $(GSR_OBJ) | $(WGPU_LIB)
	$(CXX) $(CFLAGS) $(WGPU_LIB) -framework Metal -framework QuartzCore -framework Foundation -framework CoreFoundation \
	    -framework IOKit -framework IOSurface -lobjc -o $@ $^

run: all
	$(OUT)/hp $(REC)/image.bin disc

clean:
	rm -rf $(OUT)/obj $(OUT)/hp

FORCE:
