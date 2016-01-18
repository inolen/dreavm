#ifndef DREAMCAST_H
#define DREAMCAST_H

// needed for register types
#include "hw/holly/holly.h"
#include "hw/holly/pvr2.h"
#include "hw/sh4/sh4.h"

namespace dvm {

namespace renderer {
class Backend;
}

namespace trace {
class TraceWriter;
}

namespace hw {

namespace aica {
class AICA;
}

namespace gdrom {
class GDROM;
}

namespace holly {
class Holly;
class PVR2;
class TextureCache;
class TileAccelerator;
class TileRenderer;
}

namespace maple {
class Maple;
}

namespace sh4 {
class SH4;
}

class Memory;
class Scheduler;

//
// memory layout
//
#define MEMORY_REGION(name, start, end) \
  name##_START = start, name##_END = end, name##_SIZE = end - start + 1

// clang-format off
enum {
  MEMORY_REGION(AREA0,       0x00000000, 0x03ffffff),
  MEMORY_REGION(BIOS,        0x00000000, 0x001fffff),
  MEMORY_REGION(FLASH,       0x00200000, 0x0021ffff),
  MEMORY_REGION(HOLLY_REG,   0x005f6000, 0x005f7fff),
  MEMORY_REGION(PVR_REG,     0x005f8000, 0x005f8fff),
  MEMORY_REGION(PVR_PALETTE, 0x005f9000, 0x005f9fff),
  MEMORY_REGION(MODEM_REG,   0x00600000, 0x0067ffff),
  MEMORY_REGION(AICA_REG,    0x00700000, 0x00710fff),
  MEMORY_REGION(WAVE_RAM,    0x00800000, 0x009fffff),
  MEMORY_REGION(EXPDEV,      0x01000000, 0x01ffffff),

  MEMORY_REGION(AREA1,       0x04000000, 0x07ffffff),
  MEMORY_REGION(PVR_VRAM32,  0x04000000, 0x047fffff),
  MEMORY_REGION(PVR_VRAM64,  0x05000000, 0x057fffff),

  MEMORY_REGION(AREA2,       0x08000000, 0x0bffffff),

  MEMORY_REGION(AREA3,       0x0c000000, 0x0cffffff),  // 16 mb ram, mirrored 4x
  MEMORY_REGION(MAIN_RAM_1,  0x0c000000, 0x0cffffff),
  MEMORY_REGION(MAIN_RAM_2,  0x0d000000, 0x0dffffff),
  MEMORY_REGION(MAIN_RAM_3,  0x0e000000, 0x0effffff),
  MEMORY_REGION(MAIN_RAM_4,  0x0f000000, 0x0fffffff),

  MEMORY_REGION(AREA4,       0x10000000, 0x13ffffff),
  MEMORY_REGION(TA_CMD,      0x10000000, 0x107fffff),
  MEMORY_REGION(TA_TEXTURE,  0x11000000, 0x11ffffff),

  MEMORY_REGION(AREA5,       0x14000000, 0x17ffffff),
  MEMORY_REGION(MODEM,       0x14000000, 0x17ffffff),

  MEMORY_REGION(AREA6,       0x18000000, 0x1bffffff),
  MEMORY_REGION(UNASSIGNED,  0x18000000, 0x1bffffff),

  MEMORY_REGION(AREA7,       0x1c000000, 0x1fffffff),
  MEMORY_REGION(SH4_REG,     0x1c000000, 0x1fffffff),
  MEMORY_REGION(SH4_CACHE,   0x7c000000, 0x7fffffff),
  MEMORY_REGION(SH4_SQ,      0xe0000000, 0xe3ffffff),

  MEMORY_REGION(P0_1,        0x00000000, 0x1fffffff),
  MEMORY_REGION(P0_2,        0x20000000, 0x3fffffff),
  MEMORY_REGION(P0_3,        0x40000000, 0x5fffffff),
  MEMORY_REGION(P0_4,        0x60000000, 0x7fffffff),
  MEMORY_REGION(P1,          0x80000000, 0x9fffffff),
  MEMORY_REGION(P2,          0xa0000000, 0xbfffffff),
  MEMORY_REGION(P3,          0xc0000000, 0xdfffffff),
  MEMORY_REGION(P4,          0xe0000000, 0xffffffff),
};
// clang-format on

//
// registers
//
enum {  //
  R = 0x1,
  W = 0x2,
  RW = 0x3,
  UNDEFINED = 0x0
};

struct Register {
  Register() : flags(RW), value(0) {}
  Register(uint8_t flags, uint32_t value) : flags(flags), value(value) {}

  uint8_t flags;
  uint32_t value;
};

enum {
#define AICA_REG(addr, name, flags, default, type) \
  name##_OFFSET = addr - hw::AICA_REG_START,
#include "hw/aica/aica_regs.inc"
#undef AICA_REG

#define HOLLY_REG(addr, name, flags, default, type) \
  name##_OFFSET = (addr - hw::HOLLY_REG_START) >> 2,
#include "hw/holly/holly_regs.inc"
#undef HOLLY_REG

#define PVR_REG(addr, name, flags, default_value, type) \
  name##_OFFSET = (addr - hw::PVR_REG_START) >> 2,
#include "hw/holly/pvr2_regs.inc"
#undef PVR_REG
};

class Dreamcast {
 public:
  uint8_t *aica_regs() { return aica_regs_; }
  Register *holly_regs() { return holly_regs_; }
  Register *pvr_regs() { return pvr_regs_; }

  uint8_t *bios() { return bios_; }
  uint8_t *flash() { return flash_; }
  uint8_t *wave_ram() { return wave_ram_; }
  uint8_t *palette_ram() { return palette_ram_; }
  uint8_t *video_ram() { return video_ram_; }

  hw::Memory *memory() { return memory_; }
  hw::Scheduler *scheduler() { return scheduler_; }
  hw::aica::AICA *aica() { return aica_; }
  hw::gdrom::GDROM *gdrom() { return gdrom_; }
  hw::holly::Holly *holly() { return holly_; }
  hw::maple::Maple *maple() { return maple_; }
  hw::holly::PVR2 *pvr() { return pvr_; }
  hw::sh4::SH4 *sh4() { return sh4_; }
  hw::holly::TileAccelerator *ta() { return ta_; }
  hw::holly::TextureCache *texcache() { return texcache_; }

  renderer::Backend *rb() { return rb_; }
  void set_rb(renderer::Backend *rb) { rb_ = rb; }

  trace::TraceWriter *trace_writer() { return trace_writer_; }
  void set_trace_writer(trace::TraceWriter *trace_writer) {
    trace_writer_ = trace_writer;
  }

  Dreamcast();
  ~Dreamcast();

  bool Init();

 private:
  Register *holly_regs_;

 public:
#define HOLLY_REG(offset, name, flags, default, type) type &name;
#include "hw/holly/holly_regs.inc"
#undef HOLLY_REG

 private:
  Register *pvr_regs_;

 public:
#define PVR_REG(offset, name, flags, default, type) type &name;
#include "hw/holly/pvr2_regs.inc"
#undef PVR_REG

 private:
  bool MapMemory();

  uint8_t *aica_regs_;
  uint8_t *bios_;
  uint8_t *flash_;
  uint8_t *palette_ram_;
  uint8_t *ram_;
  uint8_t *video_ram_;
  uint8_t *wave_ram_;

  hw::Memory *memory_;
  hw::Scheduler *scheduler_;
  hw::aica::AICA *aica_;
  hw::gdrom::GDROM *gdrom_;
  hw::holly::Holly *holly_;
  hw::maple::Maple *maple_;
  hw::holly::PVR2 *pvr_;
  hw::sh4::SH4 *sh4_;
  hw::holly::TileAccelerator *ta_;
  hw::holly::TextureCache *texcache_;

  // not owned by us
  renderer::Backend *rb_;
  trace::TraceWriter *trace_writer_;
};
}
}

#endif
