# Builds REDRIVER2 + PsyCross into libred2.a for PS5 (prospero-clang18 wrapper).
# Paths come from the environment (see scripts/env.sh.example)
NAB   ?= $(error set NAB to the ps5-native-app-boilerplate checkout)
export PS5_PAYLOAD_SDK := $(NAB)/.deps/native/ps5-payload-sdk
CC    := sh $(NAB)/tooling/prospero-clang18
SRC   ?= ../../REDRIVER2/src_rebuild
OGL   := $(NAB)/vendor/ps5-opengl-sdk/include
SDL   := $(NAB)/vendor/sdl2-real/include/SDL2
OBJ   := obj
DEFS  := -DNTSC_VERSION -DNDEBUG -DBUILD_CONFIGURATION_STRING=\"PS5\" -D__PROSPERO__ -DGL_GLEXT_PROTOTYPES=1
INC   := -Ishim -I$(SRC)/Game -I$(SRC)/PsyCross/include -I$(SRC)/PsyCross/include/psx -I$(SRC)/PsyCross/src -I$(SRC) -I$(SDL) -Ithird_party -I$(OGL)
WARN  := -w -Wno-error
CFLAGS   := -O2 -ffunction-sections -fdata-sections $(DEFS) $(INC) $(WARN) -std=gnu11 -fno-strict-aliasing -fcommon
CXXFLAGS := -O2 -ffunction-sections -fdata-sections $(DEFS) $(INC) $(WARN) -std=gnu++17 -fno-exceptions -fno-rtti -Wno-c++11-narrowing -Wno-error -include stdint.h -include ps5_compat.h -include strings.h -include stdlib.h -include stdarg.h -include string.h -fno-strict-aliasing

CSRC   := $(shell find $(SRC)/Game -name '*.c') $(shell find $(SRC)/PsyCross/src -name '*.c' -not -name glad.c) $(shell find $(SRC)/utils -name '*.c')
CXXSRC := $(shell find $(SRC)/PsyCross/src -name '*.cpp' -not -name crash_handler.cpp) $(shell find $(SRC)/utils -name "*.cpp") $(SRC)/redriver2_psxpc.cpp
USRC   := $(shell find $(SRC)/PsyCross/src -name '*.C')   # uppercase .C
GLAD   := $(SRC)/PsyCross/src/render/glad.c

OBJS := obj/ps5_al.o $(patsubst $(SRC)/%,$(OBJ)/%.o,$(CSRC) $(CXXSRC) $(USRC) $(GLAD))

libred2.a: $(OBJS)
	rm -f $@ && ar rcs $@ $(OBJS)

$(OBJ)/%.cpp.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@) && $(CC) $(CXXFLAGS) -c $< -o $@
$(OBJ)/%.c.o: $(SRC)/%.c
	@mkdir -p $(dir $@) && $(CC) -x c++ $(CXXFLAGS) -c $< -o $@
$(OBJ)/%.C.o: $(SRC)/%.C
	@mkdir -p $(dir $@) && $(CC) -x c++ $(CXXFLAGS) -c $< -o $@

obj/ps5_al.o: shim/ps5_al.cpp shim/AL/al.h shim/AL/alc.h
	@mkdir -p obj && $(CC) $(CXXFLAGS) -c $< -o $@
