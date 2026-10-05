# Host Linux configuration for the pinned opengnm-psbc source tree.
# The upstream standalone Makefile expects config.mak, but does not currently
# ship a generic Linux config.mak. Keep this small and close to config.orbis.mak.

DESTDIR=/usr/local
BINDIR=/bin

OPENGNM_INCLUDE?=../opengnm/include

SHARED_FLAGS=\
	-Iinclude/ \
	-Ilibpsbc/ \
	-I$(OPENGNM_INCLUDE) \
	-I../Vulkan-Headers/include \
	-Isrc/ \
	-Isrc/amd \
	-Isrc/amd/common \
	-Isrc/amd/common/nir \
	-Isrc/amd/compiler \
	-Isrc/amd/vulkan \
	-Isrc/amd/vulkan/nir \
	-Isrc/vulkan/runtime \
	-Isrc/vulkan/runtime/bvh \
	-Isrc/vulkan/util \
	-Isrc/compiler \
	-Isrc/compiler/nir \
	-Isrc/compiler/spirv \
	-Isrc/gallium/include \
	-Isrc/mesa \
	-Isrc/mesa/main \
	-Isrc/util \
	-Icmd/psbc \
	-Iinclude/mesa \
	-D_XOPEN_SOURCE=500 \
	-DUTIL_ARCH_LITTLE_ENDIAN=1 \
	-DUTIL_ARCH_BIG_ENDIAN=0 \
	-DHAVE_STRUCT_TIMESPEC=1 \
	-DHAVE_PTHREAD=1

CC=clang
CXX=clang++
LD=clang++
AR=llvm-ar
PYTHON=python3

CFLAGS=-std=gnu11 -Wall -O2 -g $(SHARED_FLAGS) \
	-Wno-macro-redefined -Wno-typedef-redefinition \
	-Wno-unused-function -Wno-unused-variable \
	-Dalloca=__builtin_alloca -DHAVE_SYSCONF=1 -include strings.h

CXXFLAGS=-std=c++17 -Wall -O2 -g $(SHARED_FLAGS) \
	-Wno-macro-redefined -Wno-typedef-redefinition \
	-Wno-unused-function -Wno-unused-variable \
	-Dalloca=__builtin_alloca -DHAVE_SYSCONF=1 -include strings.h

LDFLAGS=-lm -lpthread
