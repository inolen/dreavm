#include "hw/pvr/ta.h"
#include "core/list.h"
#include "core/profiler.h"
#include "core/rb_tree.h"
#include "core/string.h"
#include "hw/holly/holly.h"
#include "hw/pvr/pixel_convert.h"
#include "hw/pvr/pvr.h"
#include "hw/pvr/tr.h"
#include "hw/pvr/trace.h"
#include "hw/scheduler.h"
#include "hw/sh4/sh4.h"
#include "sys/exception_handler.h"
#include "sys/filesystem.h"
#include "sys/thread.h"
#include "ui/nuklear.h"

#define TA_MAX_CONTEXTS 8
#define TA_YUV420_MACROBLOCK_SIZE 384
#define TA_YUV422_MACROBLOCK_SIZE 512
#define TA_MAX_MACROBLOCK_SIZE \
  MAX(TA_YUV420_MACROBLOCK_SIZE, TA_YUV422_MACROBLOCK_SIZE)

struct ta_texture_entry {
  struct texture_entry;
  struct memory_watch *texture_watch;
  struct memory_watch *palette_watch;
  struct list_node free_it;
  struct rb_node live_it;
};

struct ta {
  struct device;
  struct texture_provider provider;
  uint8_t *rb_ram;

  /* yuv data converter state */
  uint8_t *yuv_data;
  int yuv_width;
  int yuv_height;
  int yuv_macroblock_size;
  int yuv_macroblock_count;

  /* texture cache entry pool. free entries are in a linked list, live entries
     are in a tree ordered by texture key, textures queued for invalidation are
     in the the invalid_entries linked list */
  struct ta_texture_entry entries[8192];
  struct list free_entries;
  struct rb_tree live_entries;
  int num_invalidated;

  /* tile context pool. free contexts are in a linked list, live contexts are
     are in a tree ordered by the context's guest address */
  struct tile_ctx contexts[TA_MAX_CONTEXTS];
  struct list free_contexts;
  struct rb_tree live_contexts;

  /* the pending context is the last context requested to be rendered by the
     emulation thread. a mutex is used to synchronize access with the graphics
     thread */
  mutex_t pending_mutex;
  struct tile_ctx *pending_context;

  /* buffers used by the tile contexts. allocating here instead of inside each
     tile_ctx to avoid blowing the stack when a tile_ctx is needed temporarily
     on the stack for searching */
  uint8_t params[TA_MAX_CONTEXTS * TA_MAX_PARAMS];

  /* debug info */
  int frame;
  int frames_skipped;
  int num_textures;
  struct trace_writer *trace_writer;
};

int g_param_sizes[0x100 * TA_NUM_PARAMS * TA_NUM_VERTS];
int g_poly_types[0x100 * TA_NUM_PARAMS * TA_NUM_LISTS];
int g_vertex_types[0x100 * TA_NUM_PARAMS * TA_NUM_LISTS];

static holly_interrupt_t list_interrupts[] = {
    HOLLY_INTC_TAEOINT,  /* TA_LIST_OPAQUE */
    HOLLY_INTC_TAEOMINT, /* TA_LIST_OPAQUE_MODVOL */
    HOLLY_INTC_TAETINT,  /* TA_LIST_TRANSLUCENT */
    HOLLY_INTC_TAETMINT, /* TA_LIST_TRANSLUCENT_MODVOL */
    HOLLY_INTC_TAEPTIN   /* TA_LIST_PUNCH_THROUGH */
};

static int ta_entry_cmp(const struct rb_node *rb_lhs,
                        const struct rb_node *rb_rhs) {
  const struct ta_texture_entry *lhs =
      rb_entry(rb_lhs, const struct ta_texture_entry, live_it);
  texture_key_t lhs_key = tr_texture_key(lhs->tsp, lhs->tcw);

  const struct ta_texture_entry *rhs =
      rb_entry(rb_rhs, const struct ta_texture_entry, live_it);
  texture_key_t rhs_key = tr_texture_key(rhs->tsp, rhs->tcw);

  if (lhs_key < rhs_key) {
    return -1;
  } else if (lhs_key > rhs_key) {
    return 1;
  } else {
    return 0;
  }
}

static int ta_context_cmp(const struct rb_node *rb_lhs,
                          const struct rb_node *rb_rhs) {
  const struct tile_ctx *lhs = rb_entry(rb_lhs, const struct tile_ctx, live_it);
  const struct tile_ctx *rhs = rb_entry(rb_rhs, const struct tile_ctx, live_it);

  if (lhs->addr < rhs->addr) {
    return -1;
  } else if (lhs->addr > rhs->addr) {
    return 1;
  } else {
    return 0;
  }
}

static struct rb_callbacks ta_entry_cb = {&ta_entry_cmp, NULL, NULL};
static struct rb_callbacks ta_context_cb = {&ta_context_cmp, NULL, NULL};

/* See "57.1.1.2 Parameter Combinations" for information on the poly types. */
static int ta_get_poly_type_raw(union pcw pcw) {
  if (pcw.list_type == TA_LIST_OPAQUE_MODVOL ||
      pcw.list_type == TA_LIST_TRANSLUCENT_MODVOL) {
    return 6;
  }

  if (pcw.para_type == TA_PARAM_SPRITE) {
    return 5;
  }

  if (pcw.volume) {
    if (pcw.col_type == 0) {
      return 3;
    }
    if (pcw.col_type == 2) {
      return 4;
    }
    if (pcw.col_type == 3) {
      return 3;
    }
  }

  if (pcw.col_type == 0 || pcw.col_type == 1 || pcw.col_type == 3) {
    return 0;
  }
  if (pcw.col_type == 2 && pcw.texture && !pcw.offset) {
    return 1;
  }
  if (pcw.col_type == 2 && pcw.texture && pcw.offset) {
    return 2;
  }
  if (pcw.col_type == 2 && !pcw.texture) {
    return 1;
  }

  return 0;
}

/* See "57.1.1.2 Parameter Combinations" for information on the vertex types. */
static int ta_get_vert_type_raw(union pcw pcw) {
  if (pcw.list_type == TA_LIST_OPAQUE_MODVOL ||
      pcw.list_type == TA_LIST_TRANSLUCENT_MODVOL) {
    return 17;
  }

  if (pcw.para_type == TA_PARAM_SPRITE) {
    return pcw.texture ? 16 : 15;
  }

  if (pcw.volume) {
    if (pcw.texture) {
      if (pcw.col_type == 0) {
        return pcw.uv_16bit ? 12 : 11;
      }
      if (pcw.col_type == 2 || pcw.col_type == 3) {
        return pcw.uv_16bit ? 14 : 13;
      }
    }

    if (pcw.col_type == 0) {
      return 9;
    }
    if (pcw.col_type == 2 || pcw.col_type == 3) {
      return 10;
    }
  }

  if (pcw.texture) {
    if (pcw.col_type == 0) {
      return pcw.uv_16bit ? 4 : 3;
    }
    if (pcw.col_type == 1) {
      return pcw.uv_16bit ? 6 : 5;
    }
    if (pcw.col_type == 2 || pcw.col_type == 3) {
      return pcw.uv_16bit ? 8 : 7;
    }
  }

  if (pcw.col_type == 0) {
    return 0;
  }
  if (pcw.col_type == 1) {
    return 1;
  }
  if (pcw.col_type == 2 || pcw.col_type == 3) {
    return 2;
  }

  return 0;
}

/* Parameter size can be determined by only the union pcw for every parameter
   other than vertex parameters. For vertex parameters, the vertex type derived
   from the last poly or modifier volume parameter is needed. */
static int ta_get_param_size_raw(union pcw pcw, int vertex_type) {
  switch (pcw.para_type) {
    case TA_PARAM_END_OF_LIST:
      return 32;
    case TA_PARAM_USER_TILE_CLIP:
      return 32;
    case TA_PARAM_OBJ_LIST_SET:
      return 32;
    case TA_PARAM_POLY_OR_VOL: {
      int type = ta_get_poly_type_raw(pcw);
      return type == 0 || type == 1 || type == 3 ? 32 : 64;
    }
    case TA_PARAM_SPRITE:
      return 32;
    case TA_PARAM_VERTEX: {
      return vertex_type == 0 || vertex_type == 1 || vertex_type == 2 ||
                     vertex_type == 3 || vertex_type == 4 || vertex_type == 7 ||
                     vertex_type == 8 || vertex_type == 9 || vertex_type == 10
                 ? 32
                 : 64;
    }
    default:
      return 0;
  }
}

static void ta_soft_reset(struct ta *ta) {
  /* FIXME what are we supposed to do here? */
}

static struct ta_texture_entry *ta_alloc_texture(struct ta *ta, union tsp tsp,
                                                 union tcw tcw) {
  /* remove from free list */
  struct ta_texture_entry *entry =
      list_first_entry(&ta->free_entries, struct ta_texture_entry, free_it);
  CHECK_NOTNULL(entry);
  list_remove(&ta->free_entries, &entry->free_it);

  /* reset entry */
  memset(entry, 0, sizeof(*entry));
  entry->tsp = tsp;
  entry->tcw = tcw;

  /* add to live tree */
  rb_insert(&ta->live_entries, &entry->live_it, &ta_entry_cb);

  ta->num_textures++;

  return entry;
}

static struct ta_texture_entry *ta_find_texture(struct ta *ta, union tsp tsp,
                                                union tcw tcw) {
  struct ta_texture_entry search;
  search.tsp = tsp;
  search.tcw = tcw;

  return rb_find_entry(&ta->live_entries, &search, struct ta_texture_entry,
                       live_it, &ta_entry_cb);
}

static struct texture_entry *ta_texture_provider_find_texture(void *data,
                                                              union tsp tsp,
                                                              union tcw tcw) {
  struct ta_texture_entry *entry = ta_find_texture(data, tsp, tcw);

  if (!entry) {
    return NULL;
  }

  return (struct texture_entry *)entry;
}

static void ta_clear_textures(struct ta *ta) {
  LOG_INFO("Texture cache cleared");

  struct rb_node *it = rb_first(&ta->live_entries);

  while (it) {
    struct rb_node *next = rb_next(it);

    struct ta_texture_entry *entry =
        rb_entry(it, struct ta_texture_entry, live_it);

    entry->dirty = 1;

    it = next;
  }
}

static void ta_texture_invalidated(const struct exception *ex, void *data) {
  struct ta_texture_entry *entry = data;
  entry->texture_watch = NULL;
  entry->dirty = 1;
}

static void ta_palette_invalidated(const struct exception *ex, void *data) {
  struct ta_texture_entry *entry = data;
  entry->palette_watch = NULL;
  entry->dirty = 1;
}

static struct tile_ctx *ta_get_context(struct ta *ta, uint32_t addr) {
  struct tile_ctx search;
  search.addr = addr;

  return rb_find_entry(&ta->live_contexts, &search, struct tile_ctx, live_it,
                       &ta_context_cb);
}

static struct tile_ctx *ta_alloc_context(struct ta *ta, uint32_t addr) {
  /* remove from free list */
  struct tile_ctx *ctx =
      list_first_entry(&ta->free_contexts, struct tile_ctx, free_it);
  CHECK_NOTNULL(ctx);
  list_remove(&ta->free_contexts, &ctx->free_it);

  /* reset context */
  uint8_t *params = ctx->params;
  memset(ctx, 0, sizeof(*ctx));
  ctx->addr = addr;
  ctx->params = params;

  /* add to live tree */
  rb_insert(&ta->live_contexts, &ctx->live_it, &ta_context_cb);

  return ctx;
}

static void ta_unlink_context(struct ta *ta, struct tile_ctx *ctx) {
  rb_unlink(&ta->live_contexts, &ctx->live_it, &ta_context_cb);
}

static void ta_free_context(struct ta *ta, struct tile_ctx *ctx) {
  list_add(&ta->free_contexts, &ctx->free_it);
}

static void ta_init_context(struct ta *ta, uint32_t addr) {
  struct tile_ctx *ctx = ta_get_context(ta, addr);

  if (!ctx) {
    ctx = ta_alloc_context(ta, addr);
  }

  ctx->addr = addr;
  ctx->cursor = 0;
  ctx->size = 0;
  ctx->list_type = TA_NUM_LISTS;
  ctx->vertex_type = TA_NUM_VERTS;
}

static void ta_write_context(struct ta *ta, uint32_t addr, void *ptr,
                             int size) {
  struct tile_ctx *ctx = ta_get_context(ta, addr);
  CHECK_NOTNULL(ctx);

  CHECK_LT(ctx->size + size, TA_MAX_PARAMS);
  memcpy(&ctx->params[ctx->size], ptr, size);
  ctx->size += size;

  /* each TA command is either 32 or 64 bytes, with the pcw being in the first
     32 bytes always. check every 32 bytes to see if the command has been
     completely received or not */
  if (ctx->size % 32 == 0) {
    void *param = &ctx->params[ctx->cursor];
    union pcw pcw = *(union pcw *)param;

    int size = ta_get_param_size(pcw, ctx->vertex_type);
    int recv = ctx->size - ctx->cursor;

    if (recv < size) {
      /* wait for the entire command */
      return;
    }

    if (pcw.para_type == TA_PARAM_END_OF_LIST) {
      /* it's common that a TA_PARAM_END_OF_LIST is sent before a
       * valid list has been initialized */
      if (ctx->list_type != TA_NUM_LISTS) {
        holly_raise_interrupt(ta->holly, list_interrupts[ctx->list_type]);
      }

      /* reset list state */
      ctx->list_type = TA_NUM_LISTS;
      ctx->vertex_type = TA_NUM_VERTS;
    } else if (pcw.para_type == TA_PARAM_OBJ_LIST_SET) {
      LOG_FATAL("TA_PARAM_OBJ_LIST_SET unsupported");
    } else if (pcw.para_type == TA_PARAM_POLY_OR_VOL) {
      ctx->vertex_type = ta_get_vert_type(pcw);
    } else if (pcw.para_type == TA_PARAM_SPRITE) {
      ctx->vertex_type = ta_get_vert_type(pcw);
    }

    /* pcw.list_type is only valid for the first global parameter / object
       list set after TA_LIST_INIT or a previous TA_PARAM_END_OF_LIST */
    if ((pcw.para_type == TA_PARAM_OBJ_LIST_SET ||
         pcw.para_type == TA_PARAM_POLY_OR_VOL ||
         pcw.para_type == TA_PARAM_SPRITE) &&
        ctx->list_type == TA_NUM_LISTS) {
      ctx->list_type = pcw.list_type;
    }

    ctx->cursor += recv;
  }
}

static void ta_register_texture(struct ta *ta, union tsp tsp, union tcw tcw) {
  struct ta_texture_entry *entry = ta_find_texture(ta, tsp, tcw);
  int new_entry = 0;

  if (!entry) {
    entry = ta_alloc_texture(ta, tsp, tcw);
    new_entry = 1;
  }

  /* mark texture source valid for the current frame */
  entry->frame = ta->frame;

  /* set texture address */
  if (!entry->texture) {
    uint32_t texture_addr = tcw.texture_addr << 3;
    int width = 8 << tsp.texture_u_size;
    int height = 8 << tsp.texture_v_size;
    int element_size_bits = tcw.pixel_format == TA_PIXEL_8BPP
                                ? 8
                                : tcw.pixel_format == TA_PIXEL_4BPP ? 4 : 16;
    entry->texture = &ta->rb_ram[texture_addr];
    entry->texture_size = (width * height * element_size_bits) >> 3;
  }

  /* set palette address */
  if (!entry->palette) {
    if (tcw.pixel_format == TA_PIXEL_4BPP ||
        tcw.pixel_format == TA_PIXEL_8BPP) {
      uint32_t palette_addr = 0;
      int palette_size = 0;

      /* palette ram is 4096 bytes, with each palette entry being 4 bytes each,
         resulting in 1 << 10 indexes */
      if (tcw.pixel_format == TA_PIXEL_4BPP) {
        /* in 4bpp mode, the palette selector represents the upper 6 bits of the
           palette index, with the remaining 4 bits being filled in by the
           texture */
        palette_addr = (tcw.p.palette_selector << 4) * 4;
        palette_size = (1 << 4) * 4;
      } else if (tcw.pixel_format == TA_PIXEL_8BPP) {
        /* in 4bpp mode, the palette selector represents the upper 2 bits of the
           palette index, with the remaining 8 bits being filled in by the
           texture */
        palette_addr = ((tcw.p.palette_selector & 0x30) << 4) * 4;
        palette_size = (1 << 8) * 4;
      }

      entry->palette = &ta->pvr->palette_ram[palette_addr];
      entry->palette_size = palette_size;
    }
  }

/* add write callback in order to invalidate on future writes. the callback
   address will be page aligned, therefore it will be triggered falsely in
   some cases. over invalidate in these cases */
#ifdef NDEBUG
  if (!entry->texture_watch) {
    entry->texture_watch = add_single_write_watch(
        entry->texture, entry->texture_size, &ta_texture_invalidated, entry);
  }

  if (entry->palette && !entry->palette_watch) {
    entry->palette_watch = add_single_write_watch(
        entry->palette, entry->palette_size, &ta_palette_invalidated, entry);
  }
#endif

  /* add modified entries to the trace */
  if (ta->trace_writer && (new_entry || entry->dirty)) {
    trace_writer_insert_texture(ta->trace_writer, tsp, tcw, entry->palette,
                                entry->palette_size, entry->texture,
                                entry->texture_size);
  }
}

static void ta_register_textures(struct ta *ta, struct tile_ctx *ctx,
                                 int *num_polys) {
  const uint8_t *data = ctx->params;
  const uint8_t *end = ctx->params + ctx->size;
  int vertex_type = 0;

  *num_polys = 0;

  while (data < end) {
    union pcw pcw = *(union pcw *)data;

    switch (pcw.para_type) {
      case TA_PARAM_POLY_OR_VOL:
      case TA_PARAM_SPRITE: {
        const union poly_param *param = (const union poly_param *)data;

        vertex_type = ta_get_vert_type(param->type0.pcw);

        if (param->type0.pcw.texture) {
          ta_register_texture(ta, param->type0.tsp, param->type0.tcw);
        }

        (*num_polys)++;
      } break;

      default:
        break;
    }

    data += ta_get_param_size(pcw, vertex_type);
  }
}

static void ta_save_register_state(struct ta *ta, struct tile_ctx *ctx) {
  struct pvr *pvr = ta->pvr;
  struct address_space *space = ta->sh4->memory_if->space;

  /* autosort */
  if (!pvr->FPU_PARAM_CFG->region_header_type) {
    ctx->autosort = !pvr->ISP_FEED_CFG->presort;
  } else {
    uint32_t region_data = as_read32(space, 0x05000000 + *pvr->REGION_BASE);
    ctx->autosort = !(region_data & 0x20000000);
  }

  /* texture stride */
  ctx->stride = pvr->TEXT_CONTROL->stride * 32;

  /* texture palette pixel format */
  ctx->pal_pxl_format = pvr->PAL_RAM_CTRL->pixel_format;

  /* write out video width to help with unprojecting the screen space
     coordinates */
  if (pvr->SPG_CONTROL->interlace ||
      (!pvr->SPG_CONTROL->NTSC && !pvr->SPG_CONTROL->PAL)) {
    /* interlaced and VGA mode both render at full resolution */
    ctx->rb_width = 640;
    ctx->rb_height = 480;
  } else {
    ctx->rb_width = 320;
    ctx->rb_height = 240;
  }

  /* according to the hardware docs, this is the correct calculation of the
     background ISP address. however, in practice, the second TA buffer's ISP
     address comes out to be 0x800000 when booting the bios and the vram is
     only 8mb total. by examining a raw memory dump, the ISP data is only ever
     available at 0x0 when booting the bios, so masking this seems to be the
     correct solution */
  uint32_t vram_offset =
      0x05000000 +
      ((ctx->addr + pvr->ISP_BACKGND_T->tag_address * 4) & 0x7fffff);

  /* get surface parameters */
  ctx->bg_isp.full = as_read32(space, vram_offset);
  ctx->bg_tsp.full = as_read32(space, vram_offset + 4);
  ctx->bg_tcw.full = as_read32(space, vram_offset + 8);
  vram_offset += 12;

  /* get the background depth */
  ctx->bg_depth = *(float *)pvr->ISP_BACKGND_D;

  /* get the byte size for each vertex. normally, the byte size is
     ISP_BACKGND_T.skip + 3, but if parameter selection volume mode is in
     effect and the shadow bit is 1, then the byte size is
     ISP_BACKGND_T.skip * 2 + 3 */
  int vertex_size = pvr->ISP_BACKGND_T->skip;
  if (!pvr->FPU_SHAD_SCALE->intensity_volume_mode &&
      pvr->ISP_BACKGND_T->shadow) {
    vertex_size *= 2;
  }
  vertex_size = (vertex_size + 3) * 4;

  /* skip to the first vertex */
  vram_offset += pvr->ISP_BACKGND_T->tag_offset * vertex_size;

  /* copy vertex data to context */
  for (int i = 0, bg_offset = 0; i < 3; i++) {
    CHECK_LE(bg_offset + vertex_size, (int)sizeof(ctx->bg_vertices));

    as_memcpy_to_host(space, &ctx->bg_vertices[bg_offset], vram_offset,
                      vertex_size);

    bg_offset += vertex_size;
    vram_offset += vertex_size;
  }
}

static void ta_end_render(struct ta *ta) {
  /* let the game know rendering is complete */
  holly_raise_interrupt(ta->holly, HOLLY_INTC_PCEOVINT);
  holly_raise_interrupt(ta->holly, HOLLY_INTC_PCEOIINT);
  holly_raise_interrupt(ta->holly, HOLLY_INTC_PCEOTINT);
}

static void ta_render_timer(void *data) {
  struct ta *ta = data;

  /* ideally, the graphics thread has parsed the pending context, uploaded its
     textures, etc. during the estimated render time. however, if it hasn't
     finished, the emulation thread must be paused to avoid altering
     the yet-to-be-uploaded texture memory */
  mutex_lock(ta->pending_mutex);
  mutex_unlock(ta->pending_mutex);

  ta_end_render(ta);
}

static void ta_start_render(struct ta *ta, uint32_t addr) {
  struct tile_ctx *ctx = ta_get_context(ta, addr);
  CHECK_NOTNULL(ctx);

  /* save off required register state that may be modified by the time the
     context is rendered */
  ta_save_register_state(ta, ctx);

  /* if the graphics thread is still parsing the previous context, skip this
     one */
  if (!mutex_trylock(ta->pending_mutex)) {
    ta_unlink_context(ta, ctx);
    ta_free_context(ta, ctx);
    ta_end_render(ta);
    ta->frames_skipped++;
    return;
  }

  /* free the previous pending context if it wasn't rendered */
  if (ta->pending_context) {
    ta_free_context(ta, ta->pending_context);
    ta->pending_context = NULL;
  }

  /* set the new pending context */
  ta_unlink_context(ta, ctx);
  ta->pending_context = ctx;

  /* increment internal frame number. this frame number is assigned to each
     texture source registered by this context */
  ta->frame++;

  /* register the source of each texture referenced by the context with the
     tile renderer. note, the process of actually uploading the texture to the
     render backend happens lazily while rendering the context (keeping all
     backend operations on the same thread). this registration just lets the
     backend know where the texture's source data is */
  int num_polys = 0;
  ta_register_textures(ta, ta->pending_context, &num_polys);

  /* supposedly, the dreamcast can push around ~3 million polygons per second
     through the TA / PVR. with that in mind, a very poor estimate can be made
     for how long the TA would take to render a frame based on the number of
     polys pushed: 1,000,000,000 / 3,000,000 = 333 nanoseconds per polygon */
  int64_t ns = num_polys * INT64_C(333);
  scheduler_start_timer(ta->scheduler, &ta_render_timer, ta, ns);

  if (ta->trace_writer) {
    trace_writer_render_context(ta->trace_writer, ta->pending_context);
  }

  /* unlock the mutex, enabling the graphics thread to start parsing the
     pending context */
  mutex_unlock(ta->pending_mutex);
}

static void ta_yuv_init(struct ta *ta) {
  struct pvr *pvr = ta->pvr;

  /* FIXME only YUV420 -> YUV422 supported for now */
  CHECK_EQ(pvr->TA_YUV_TEX_CTRL->format, 0);

  /* FIXME only format 0 supported for now */
  CHECK_EQ(pvr->TA_YUV_TEX_CTRL->tex, 0);

  int u_size = pvr->TA_YUV_TEX_CTRL->u_size + 1;
  int v_size = pvr->TA_YUV_TEX_CTRL->v_size + 1;

  /* setup internal state for the data conversion */
  ta->yuv_data = &ta->rb_ram[pvr->TA_YUV_TEX_BASE->base_address];
  ta->yuv_width = u_size * 16;
  ta->yuv_height = v_size * 16;
  ta->yuv_macroblock_size = TA_YUV420_MACROBLOCK_SIZE;
  ta->yuv_macroblock_count = u_size * v_size;

  /* reset number of macroblocks processed */
  pvr->TA_YUV_TEX_CNT->num = 0;
}

static void ta_yuv_process_block(struct ta *ta, const uint8_t *in_uv,
                                 const uint8_t *in_y, uint8_t *out_uyvy) {
  uint8_t *out_row0 = out_uyvy;
  uint8_t *out_row1 = out_uyvy + (ta->yuv_width << 1);

  /* reencode 8x8 subblock of YUV420 data as UYVY422 */
  for (int j = 0; j < 8; j += 2) {
    for (int i = 0; i < 8; i += 2) {
      uint8_t u = in_uv[0];
      uint8_t v = in_uv[64];
      uint8_t y0 = in_y[0];
      uint8_t y1 = in_y[1];
      uint8_t y2 = in_y[8];
      uint8_t y3 = in_y[9];

      out_row0[0] = u;
      out_row0[1] = y0;
      out_row0[2] = v;
      out_row0[3] = y1;

      out_row1[0] = u;
      out_row1[1] = y2;
      out_row1[2] = v;
      out_row1[3] = y3;

      in_uv += 1;
      in_y += 2;
      out_row0 += 4;
      out_row1 += 4;
    }

    /* skip past adjacent 8x8 subblock */
    in_uv += 4;
    in_y += 8;
    out_row0 += (ta->yuv_width << 2) - 16;
    out_row1 += (ta->yuv_width << 2) - 16;
  }
}

static void ta_yuv_process_macroblock(struct ta *ta, void *data) {
  struct pvr *pvr = ta->pvr;
  struct address_space *space = ta->sh4->memory_if->space;

  /* YUV420 data comes in as a series 16x16 macroblocks that need to be
     converted into a single UYVY422 texture */
  const uint8_t *in = data;
  uint32_t out_x =
      (pvr->TA_YUV_TEX_CNT->num % (pvr->TA_YUV_TEX_CTRL->u_size + 1)) * 16;
  uint32_t out_y =
      (pvr->TA_YUV_TEX_CNT->num / (pvr->TA_YUV_TEX_CTRL->u_size + 1)) * 16;
  uint8_t *out = &ta->yuv_data[(out_y * ta->yuv_width + out_x) << 1];

  /* process each 8x8 subblock individually */
  /* (0, 0) */
  ta_yuv_process_block(ta, &in[0], &in[128], &out[0]);
  /* (8, 0) */
  ta_yuv_process_block(ta, &in[4], &in[192], &out[16]);
  /* (0, 8) */
  ta_yuv_process_block(ta, &in[32], &in[256], &out[ta->yuv_width * 16]);
  /* (8, 8) */
  ta_yuv_process_block(ta, &in[36], &in[320], &out[ta->yuv_width * 16 + 16]);

  /* reset state once all macroblocks have been processed */
  pvr->TA_YUV_TEX_CNT->num++;

  if ((int)pvr->TA_YUV_TEX_CNT->num >= ta->yuv_macroblock_count) {
    ta_yuv_init(ta);

    /* raise DMA end interrupt */
    holly_raise_interrupt(ta->holly, HOLLY_INTC_TAYUVINT);
  }
}

static void ta_poly_fifo_write(struct ta *ta, uint32_t dst, void *ptr,
                               int size) {
  PROF_ENTER("cpu", "ta_poly_fifo_write");

  CHECK(size % 32 == 0);

  uint8_t *src = ptr;
  uint8_t *end = src + size;
  while (src < end) {
    ta_write_context(ta, ta->pvr->TA_ISP_BASE->base_address, src, 32);
    src += 32;
  }

  PROF_LEAVE();
}

static void ta_yuv_fifo_write(struct ta *ta, uint32_t dst, void *ptr,
                              int size) {
  PROF_ENTER("cpu", "ta_yuv_fifo_write");

  struct holly *holly = ta->holly;
  struct pvr *pvr = ta->pvr;

  CHECK(size % ta->yuv_macroblock_size == 0);

  uint8_t *src = ptr;
  uint8_t *end = src + size;
  while (src < end) {
    ta_yuv_process_macroblock(ta, src);
    src += ta->yuv_macroblock_size;
  }

  PROF_LEAVE();
}

static void ta_texture_fifo_write(struct ta *ta, uint32_t dst, void *ptr,
                                  int size) {
  PROF_ENTER("cpu", "ta_texture_fifo_write");

  uint8_t *src = ptr;
  dst &= 0xeeffffff;
  memcpy(&ta->rb_ram[dst], src, size);

  PROF_LEAVE();
}

static bool ta_init(struct device *dev) {
  struct ta *ta = (struct ta *)dev;
  struct dreamcast *dc = ta->dc;

  ta->rb_ram = memory_translate(dc->memory, "video ram", 0x00000000);

  for (int i = 0; i < array_size(ta->entries); i++) {
    struct ta_texture_entry *entry = &ta->entries[i];
    list_add(&ta->free_entries, &entry->free_it);
  }

  for (int i = 0; i < array_size(ta->contexts); i++) {
    struct tile_ctx *ctx = &ta->contexts[i];

    ctx->params = ta->params + (TA_MAX_PARAMS * i);

    list_add(&ta->free_contexts, &ctx->free_it);
  }

  return true;
}

static void ta_toggle_tracing(struct ta *ta) {
  if (!ta->trace_writer) {
    char filename[PATH_MAX];
    get_next_trace_filename(filename, sizeof(filename));

    ta->trace_writer = trace_writer_open(filename);

    if (!ta->trace_writer) {
      LOG_INFO("Failed to start tracing");
      return;
    }

    /* clear texture cache in order to generate insert events for all
       textures referenced while tracing */
    ta_clear_textures(ta);

    LOG_INFO("Begin tracing to %s", filename);
  } else {
    trace_writer_close(ta->trace_writer);
    ta->trace_writer = NULL;

    LOG_INFO("End tracing");
  }
}

static void ta_debug_menu(struct device *dev, struct nk_context *ctx) {
  struct ta *ta = (struct ta *)dev;

  nk_layout_row_push(ctx, 30.0f);

  if (nk_menu_begin_label(ctx, "TA", NK_TEXT_LEFT, nk_vec2(140.0f, 200.0f))) {
    nk_layout_row_dynamic(ctx, DEBUG_MENU_HEIGHT, 1);

    nk_value_int(ctx, "frames skipped", ta->frames_skipped);
    nk_value_int(ctx, "num textures", ta->num_textures);

    if (!ta->trace_writer && nk_button_label(ctx, "start trace")) {
      ta_toggle_tracing(ta);
    } else if (ta->trace_writer && nk_button_label(ctx, "stop trace")) {
      ta_toggle_tracing(ta);
    }

    nk_menu_end(ctx);
  }
}

void ta_build_tables() {
  static bool initialized = false;

  if (initialized) {
    return;
  }

  initialized = true;

  for (int i = 0; i < 0x100; i++) {
    union pcw pcw = *(union pcw *)&i;

    for (int j = 0; j < TA_NUM_PARAMS; j++) {
      pcw.para_type = j;

      for (int k = 0; k < TA_NUM_VERTS; k++) {
        g_param_sizes[i * TA_NUM_PARAMS * TA_NUM_VERTS + j * TA_NUM_VERTS + k] =
            ta_get_param_size_raw(pcw, k);
      }
    }
  }

  for (int i = 0; i < 0x100; i++) {
    union pcw pcw = *(union pcw *)&i;

    for (int j = 0; j < TA_NUM_PARAMS; j++) {
      pcw.para_type = j;

      for (int k = 0; k < TA_NUM_LISTS; k++) {
        pcw.list_type = k;

        g_poly_types[i * TA_NUM_PARAMS * TA_NUM_LISTS + j * TA_NUM_LISTS + k] =
            ta_get_poly_type_raw(pcw);
        g_vertex_types[i * TA_NUM_PARAMS * TA_NUM_LISTS + j * TA_NUM_LISTS +
                       k] = ta_get_vert_type_raw(pcw);
      }
    }
  }
}

void ta_unlock_pending_context(struct ta *ta) {
  ta_free_context(ta, ta->pending_context);
  ta->pending_context = NULL;

  mutex_unlock(ta->pending_mutex);
}

int ta_lock_pending_context(struct ta *ta, struct tile_ctx **pending_ctx,
                            int *pending_frame) {
  mutex_lock(ta->pending_mutex);

  if (!ta->pending_context) {
    mutex_unlock(ta->pending_mutex);
    return 0;
  }

  *pending_ctx = ta->pending_context;
  *pending_frame = ta->frame;
  return 1;
}

struct texture_provider *ta_texture_provider(struct ta *ta) {
  return &ta->provider;
}

void ta_destroy(struct ta *ta) {
  mutex_destroy(ta->pending_mutex);
  dc_destroy_window_interface(ta->window_if);
  dc_destroy_device((struct device *)ta);
}

struct ta *ta_create(struct dreamcast *dc) {
  ta_build_tables();

  struct ta *ta = dc_create_device(dc, sizeof(struct ta), "ta", &ta_init);
  ta->window_if = dc_create_window_interface(&ta_debug_menu, NULL, NULL, NULL);
  ta->provider =
      (struct texture_provider){ta, &ta_texture_provider_find_texture};
  ta->pending_mutex = mutex_create();

  return ta;
}

REG_W32(pvr_cb, SOFTRESET) {
  struct ta *ta = dc->ta;

  if (!(value & 0x1)) {
    return;
  }

  ta_soft_reset(ta);
}

REG_W32(pvr_cb, STARTRENDER) {
  struct ta *ta = dc->ta;

  if (!value) {
    return;
  }

  ta_start_render(ta, ta->pvr->PARAM_BASE->base_address);
}

REG_W32(pvr_cb, TA_LIST_INIT) {
  struct ta *ta = dc->ta;

  if (!(value & 0x80000000)) {
    return;
  }

  ta_init_context(ta, ta->pvr->TA_ISP_BASE->base_address);
}

REG_W32(pvr_cb, TA_LIST_CONT) {
  if (!(value & 0x80000000)) {
    return;
  }

  LOG_FATAL("Unsupported TA_LIST_CONT");
}

REG_W32(pvr_cb, TA_YUV_TEX_BASE) {
  struct ta *ta = dc->ta;
  struct pvr *pvr = dc->pvr;

  pvr->TA_YUV_TEX_BASE->full = value;

  ta_yuv_init(ta);
}

/* clang-format off */
AM_BEGIN(struct ta, ta_fifo_map);
  AM_RANGE(0x00000000, 0x007fffff) AM_HANDLE("ta poly fifo",
                                             NULL, NULL, NULL,
                                             (mmio_write_string_cb)&ta_poly_fifo_write)
  AM_RANGE(0x00800000, 0x00ffffff) AM_HANDLE("ta yuv fifo",
                                             NULL, NULL, NULL,
                                             (mmio_write_string_cb)&ta_yuv_fifo_write)
  AM_RANGE(0x01000000, 0x01ffffff) AM_HANDLE("ta texture fifo",
                                            NULL, NULL, NULL,
                                            (mmio_write_string_cb)&ta_texture_fifo_write)
AM_END();
/* clang-format on */
