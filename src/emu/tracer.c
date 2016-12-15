#include "emu/tracer.h"
#include "core/math.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/tr.h"
#include "hw/pvr/trace.h"
#include "ui/nuklear.h"
#include "ui/window.h"

static const char *s_param_names[] = {
    "TA_PARAM_END_OF_LIST", "TA_PARAM_USER_TILE_CLIP", "TA_PARAM_OBJ_LIST_SET",
    "TA_PARAM_RESERVED0",   "TA_PARAM_POLY_OR_VOL",    "TA_PARAM_SPRITE",
    "TA_PARAM_RESERVED1",   "TA_PARAM_VERTEX",
};

static const char *s_list_names[] = {
    "TA_LIST_OPAQUE",        "TA_LIST_OPAQUE_MODVOL",
    "TA_LIST_TRANSLUCENT",   "TA_LIST_TRANSLUCENT_MODVOL",
    "TA_LIST_PUNCH_THROUGH",
};

static const char *s_pixel_format_names[] = {
    "PXL_INVALID", "PXL_RGBA",     "PXL_RGBA5551",
    "PXL_RGB565",  "PXL_RGBA4444", "PXL_RGBA8888",
};

static const char *s_filter_mode_names[] = {
    "FILTER_NEAREST", "FILTER_BILINEAR",
};

static const char *s_wrap_mode_names[] = {
    "WRAP_REPEAT", "WRAP_CLAMP_TO_EDGE", "WRAP_MIRRORED_REPEAT",
};

static const char *s_depthfunc_names[] = {
    "NONE",    "NEVER",  "LESS",   "EQUAL",  "LEQUAL",
    "GREATER", "NEQUAL", "GEQUAL", "ALWAYS",
};

static const char *s_cullface_names[] = {
    "NONE", "FRONT", "BACK",
};

static const char *s_blendfunc_names[] = {
    "NONE",
    "ZERO",
    "ONE",
    "SRC_COLOR",
    "ONE_MINUS_SRC_COLOR",
    "SRC_ALPHA",
    "ONE_MINUS_SRC_ALPHA",
    "DST_ALPHA",
    "ONE_MINUS_DST_ALPHA",
    "DST_COLOR",
    "ONE_MINUS_DST_COLOR",
};

static const char *s_shademode_names[] = {
    "DECAL", "MODULATE", "DECAL_ALPHA", "MODULATE_ALPHA",
};

struct tracer_texture_entry {
  struct texture_entry;
  struct rb_node live_it;
  struct list_node free_it;
};

struct tracer {
  struct window *window;
  struct window_listener listener;
  struct texture_provider provider;
  struct render_backend *rb;
  struct tr *tr;

  // ui state
  int show_params[TA_NUM_PARAMS];
  bool running;

  // trace state
  struct trace *trace;
  struct tile_ctx ctx;
  uint8_t params[TA_MAX_PARAMS];
  struct trace_cmd *current_cmd;
  int current_param_offset;
  int current_context;
  int num_contexts;

  // render state
  struct render_context rctx;
  struct surface surfs[TA_MAX_SURFS];
  struct vertex verts[TA_MAX_VERTS];
  int sorted_surfs[TA_MAX_SURFS];
  struct param_state states[TA_MAX_PARAMS];

  struct tracer_texture_entry textures[1024];
  struct rb_tree live_textures;
  struct list free_textures;
};

static int tracer_texture_cmp(const struct rb_node *rb_lhs,
                              const struct rb_node *rb_rhs) {
  const struct tracer_texture_entry *lhs =
      rb_entry(rb_lhs, const struct tracer_texture_entry, live_it);
  texture_key_t lhs_key = tr_texture_key(lhs->tsp, lhs->tcw);

  const struct tracer_texture_entry *rhs =
      rb_entry(rb_rhs, const struct tracer_texture_entry, live_it);
  texture_key_t rhs_key = tr_texture_key(rhs->tsp, rhs->tcw);

  if (lhs_key < rhs_key) {
    return -1;
  } else if (lhs_key > rhs_key) {
    return 1;
  } else {
    return 0;
  }
}

static struct rb_callbacks tracer_texture_cb = {&tracer_texture_cmp, NULL,
                                                NULL};

static struct tracer_texture_entry *tracer_find_texture(struct tracer *tracer,
                                                        union tsp tsp,
                                                        union tcw tcw) {
  struct tracer_texture_entry search;
  search.tsp = tsp;
  search.tcw = tcw;

  return rb_find_entry(&tracer->live_textures, &search,
                       struct tracer_texture_entry, live_it,
                       &tracer_texture_cb);
}

static void tracer_add_texture(struct tracer *tracer, union tsp tsp,
                               union tcw tcw, const uint8_t *palette,
                               const uint8_t *texture) {
  struct tracer_texture_entry *entry = tracer_find_texture(tracer, tsp, tcw);
  int new_entry = 0;

  if (!entry) {
    entry = list_first_entry(&tracer->free_textures,
                             struct tracer_texture_entry, free_it);
    CHECK_NOTNULL(entry);
    list_remove(&tracer->free_textures, &entry->free_it);

    entry->tsp = tsp;
    entry->tcw = tcw;

    rb_insert(&tracer->live_textures, &entry->live_it, &tracer_texture_cb);

    new_entry = 1;
  };

  entry->dirty = new_entry ? 0 : 1;
  entry->palette = palette;
  entry->texture = texture;
}

static struct texture_entry *tracer_texture_provider_find_texture(
    void *data, union tsp tsp, union tcw tcw) {
  struct tracer *tracer = data;

  struct tracer_texture_entry *entry = tracer_find_texture(tracer, tsp, tcw);
  CHECK_NOTNULL(entry, "Texture wasn't available in cache");

  return (struct texture_entry *)entry;
}

static void tracer_copy_command(const struct trace_cmd *cmd,
                                struct tile_ctx *ctx) {
  // copy TRACE_CMD_CONTEXT to the current context being rendered
  CHECK_EQ(cmd->type, TRACE_CMD_CONTEXT);

  ctx->autosort = cmd->context.autosort;
  ctx->stride = cmd->context.stride;
  ctx->pal_pxl_format = cmd->context.pal_pxl_format;
  ctx->bg_isp = cmd->context.bg_isp;
  ctx->bg_tsp = cmd->context.bg_tsp;
  ctx->bg_tcw = cmd->context.bg_tcw;
  ctx->bg_depth = cmd->context.bg_depth;
  ctx->rb_width = cmd->context.rb_width;
  ctx->rb_height = cmd->context.rb_height;
  memcpy(ctx->bg_vertices, cmd->context.bg_vertices,
         cmd->context.bg_vertices_size);
  memcpy(ctx->params, cmd->context.params, cmd->context.params_size);
  ctx->size = cmd->context.params_size;
}

static inline bool param_state_empty(struct param_state *param_state) {
  return !param_state->num_surfs && !param_state->num_verts;
}

static inline bool tracer_param_hidden(struct tracer *tracer, union pcw pcw) {
  return !tracer->show_params[pcw.para_type];
}

static void tracer_prev_param(struct tracer *tracer) {
  int offset = tracer->current_param_offset;

  while (--offset >= 0) {
    struct param_state *param_state = &tracer->rctx.states[offset];

    if (param_state_empty(param_state)) {
      continue;
    }

    union pcw pcw = *(union pcw *)(tracer->ctx.params + offset);

    // found the next visible param
    if (!tracer_param_hidden(tracer, pcw)) {
      tracer->current_param_offset = offset;
      break;
    }
  }
}

static void tracer_next_param(struct tracer *tracer) {
  int offset = tracer->current_param_offset;

  while (++offset < tracer->rctx.num_states) {
    struct param_state *param_state = &tracer->rctx.states[offset];

    if (param_state_empty(param_state)) {
      continue;
    }

    union pcw pcw = *(union pcw *)(tracer->ctx.params + offset);

    // found the next visible param
    if (!tracer_param_hidden(tracer, pcw)) {
      tracer->current_param_offset = offset;
      break;
    }
  }
}

static void tracer_reset_param(struct tracer *tracer) {
  tracer->current_param_offset = -1;
}

static void tracer_prev_context(struct tracer *tracer) {
  struct trace_cmd *begin = tracer->current_cmd->prev;

  // ensure that there is a prev context
  struct trace_cmd *prev = begin;

  while (prev) {
    if (prev->type == TRACE_CMD_CONTEXT) {
      break;
    }

    prev = prev->prev;
  }

  if (!prev) {
    return;
  }

  // walk back to the prev context, reverting any textures that've been added
  struct trace_cmd *curr = begin;

  while (curr != prev) {
    if (curr->type == TRACE_CMD_TEXTURE) {
      struct trace_cmd * override = curr->override;

      if (override) {
        CHECK_EQ(override->type, TRACE_CMD_TEXTURE);

        tracer_add_texture(tracer, override->texture.tsp, override->texture.tcw,
                           override->texture.palette,
                           override->texture.texture);
      }
    }

    curr = curr->prev;
  }

  tracer->current_cmd = curr;
  tracer->current_context--;
  tracer_copy_command(tracer->current_cmd, &tracer->ctx);
  tracer_reset_param(tracer);
}

static void tracer_next_context(struct tracer *tracer) {
  struct trace_cmd *begin =
      tracer->current_cmd ? tracer->current_cmd->next : tracer->trace->cmds;

  // ensure that there is a next context
  struct trace_cmd *next = begin;

  while (next) {
    if (next->type == TRACE_CMD_CONTEXT) {
      break;
    }

    next = next->next;
  }

  if (!next) {
    return;
  }

  // walk towards to the next context, adding any new textures
  struct trace_cmd *curr = begin;

  while (curr != next) {
    if (curr->type == TRACE_CMD_TEXTURE) {
      tracer_add_texture(tracer, curr->texture.tsp, curr->texture.tcw,
                         curr->texture.palette, curr->texture.texture);
    }

    curr = curr->next;
  }

  tracer->current_cmd = curr;
  tracer->current_context++;
  tracer_copy_command(tracer->current_cmd, &tracer->ctx);
  tracer_reset_param(tracer);
}

static void tracer_reset_context(struct tracer *tracer) {
  // calculate the total number of frames for the trace
  struct trace_cmd *cmd = tracer->trace->cmds;

  tracer->num_contexts = 0;

  while (cmd) {
    if (cmd->type == TRACE_CMD_CONTEXT) {
      tracer->num_contexts++;
    }

    cmd = cmd->next;
  }

  // start rendering the first context
  tracer->current_cmd = NULL;
  tracer->current_context = -1;
  tracer_next_context(tracer);
}

static const float SCRUBBER_WINDOW_HEIGHT = 20.0f;

static void tracer_render_scrubber_menu(struct tracer *tracer) {
  struct nk_context *ctx = &tracer->window->nk->ctx;

  nk_style_default(ctx);

  // disable spacing / padding
  ctx->style.window.padding = nk_vec2(0.0f, 0.0f);
  ctx->style.window.spacing = nk_vec2(0.0f, 0.0f);

  struct nk_rect bounds = {
      0.0f, (float)tracer->window->height - SCRUBBER_WINDOW_HEIGHT,
      (float)tracer->window->width, SCRUBBER_WINDOW_HEIGHT};
  nk_flags flags = NK_WINDOW_NO_SCROLLBAR;

  if (nk_begin(ctx, "context scrubber", bounds, flags)) {
    nk_layout_row_dynamic(ctx, SCRUBBER_WINDOW_HEIGHT, 1);

    nk_size frame = tracer->current_context;

    if (nk_progress(ctx, &frame, tracer->num_contexts - 1, true)) {
      int delta = (int)frame - tracer->current_context;
      for (int i = 0; i < ABS(delta); i++) {
        if (delta > 0) {
          tracer_next_context(tracer);
        } else {
          tracer_prev_context(tracer);
        }
      }
    }
  }
  nk_end(ctx);
}

static void tracer_param_tooltip(struct tracer *tracer, int list_type,
                                 int vertex_type, int offset) {
  struct nk_context *ctx = &tracer->window->nk->ctx;
  struct param_state *param_state = &tracer->rctx.states[offset];
  int surf_id = param_state->num_surfs - 1;
  int vert_id = param_state->num_verts - 1;

  if (nk_tooltip_begin(ctx, 300.0f)) {
    nk_layout_row_dynamic(ctx, ctx->style.font->height, 1);

    nk_labelf(ctx, NK_TEXT_LEFT, "list type: %s", s_list_names[list_type]);
    nk_labelf(ctx, NK_TEXT_LEFT, "surf: %d", surf_id);

    // find sorted position
    int sort = 0;
    for (int i = 0, n = tracer->rctx.num_surfs; i < n; i++) {
      int idx = tracer->rctx.sorted_surfs[i];
      if (idx == surf_id) {
        sort = i;
        break;
      }
    }
    nk_labelf(ctx, NK_TEXT_LEFT, "sort: %d", sort);

    // render source TA information
    if (vertex_type == -1) {
      const union poly_param *param =
          (const union poly_param *)(tracer->ctx.params + offset);

      nk_labelf(ctx, NK_TEXT_LEFT, "pcw: 0x%x", param->type0.pcw.full);
      nk_labelf(ctx, NK_TEXT_LEFT, "isp_tsp: 0x%x", param->type0.isp_tsp.full);
      nk_labelf(ctx, NK_TEXT_LEFT, "tsp: 0x%x", param->type0.tsp.full);
      nk_labelf(ctx, NK_TEXT_LEFT, "tcw: 0x%x", param->type0.tcw.full);

      int poly_type = ta_get_poly_type(param->type0.pcw);

      nk_labelf(ctx, NK_TEXT_LEFT, "poly type: %d", poly_type);

      switch (poly_type) {
        case 1:
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_a: %.2f",
                    param->type1.face_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_r: %.2f",
                    param->type1.face_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_g: %.2f",
                    param->type1.face_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_b: %.2f",
                    param->type1.face_color_b);
          break;

        case 2:
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_a: %.2f",
                    param->type2.face_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_r: %.2f",
                    param->type2.face_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_g: %.2f",
                    param->type2.face_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_color_b: %.2f",
                    param->type2.face_color_b);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_offset_color_a: %.2f",
                    param->type2.face_offset_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_offset_color_r: %.2f",
                    param->type2.face_offset_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_offset_color_g: %.2f",
                    param->type2.face_offset_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "face_offset_color_b: %.2f",
                    param->type2.face_offset_color_b);
          break;

        case 5:
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color: 0x%x",
                    param->sprite.base_color);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color: 0x%x",
                    param->sprite.offset_color);
          break;
      }
    } else {
      const union vert_param *param =
          (const union vert_param *)(tracer->ctx.params + offset);

      nk_labelf(ctx, NK_TEXT_LEFT, "vert type: %d", vertex_type);

      switch (vertex_type) {
        case 0:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type0.xyz[0], param->type0.xyz[1],
                    param->type0.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color: 0x%x",
                    param->type0.base_color);
          break;

        case 1:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type1.xyz[0], param->type1.xyz[1],
                    param->type1.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_a: %.2f",
                    param->type1.base_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_r: %.2f",
                    param->type1.base_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_g: %.2f",
                    param->type1.base_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_b: %.2f",
                    param->type1.base_color_b);
          break;

        case 2:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type2.xyz[0], param->type2.xyz[1],
                    param->type2.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_intensity: %.2f",
                    param->type2.base_intensity);
          break;

        case 3:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type3.xyz[0], param->type3.xyz[1],
                    param->type3.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "uv: {%.2f, %.2f}", param->type3.uv[0],
                    param->type3.uv[1]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color: 0x%x",
                    param->type3.base_color);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color: 0x%x",
                    param->type3.offset_color);
          break;

        case 4:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type4.xyz[0], param->type4.xyz[1],
                    param->type4.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "uv: {0x%x, 0x%x}", param->type4.uv[0],
                    param->type4.uv[1]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color: 0x%x",
                    param->type4.base_color);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color: 0x%x",
                    param->type4.offset_color);
          break;

        case 5:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type5.xyz[0], param->type5.xyz[1],
                    param->type5.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "uv: {%.2f, %.2f}", param->type5.uv[0],
                    param->type5.uv[1]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_a: %.2f",
                    param->type5.base_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_r: %.2f",
                    param->type5.base_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_g: %.2f",
                    param->type5.base_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_b: %.2f",
                    param->type5.base_color_b);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_a: %.2f",
                    param->type5.offset_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_r: %.2f",
                    param->type5.offset_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_g: %.2f",
                    param->type5.offset_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_b: %.2f",
                    param->type5.offset_color_b);
          break;

        case 6:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type6.xyz[0], param->type6.xyz[1],
                    param->type6.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "uv: {0x%x, 0x%x}", param->type6.uv[0],
                    param->type6.uv[1]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_a: %.2f",
                    param->type6.base_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_r: %.2f",
                    param->type6.base_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_g: %.2f",
                    param->type6.base_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_color_b: %.2f",
                    param->type6.base_color_b);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_a: %.2f",
                    param->type6.offset_color_a);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_r: %.2f",
                    param->type6.offset_color_r);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_g: %.2f",
                    param->type6.offset_color_g);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_color_b: %.2f",
                    param->type6.offset_color_b);
          break;

        case 7:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type7.xyz[0], param->type7.xyz[1],
                    param->type7.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "uv: {%.2f, %.2f}", param->type7.uv[0],
                    param->type7.uv[1]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_intensity: %.2f",
                    param->type7.base_intensity);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_intensity: %.2f",
                    param->type7.offset_intensity);
          break;

        case 8:
          nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}",
                    param->type8.xyz[0], param->type8.xyz[1],
                    param->type8.xyz[2]);
          nk_labelf(ctx, NK_TEXT_LEFT, "uv: {0x%x, 0x%x}", param->type8.uv[0],
                    param->type8.uv[1]);
          nk_labelf(ctx, NK_TEXT_LEFT, "base_intensity: %.2f",
                    param->type8.base_intensity);
          nk_labelf(ctx, NK_TEXT_LEFT, "offset_intensity: %.2f",
                    param->type8.offset_intensity);
          break;
      }
    }

    // always render translated surface information. new surfaces can be created
    // without receiving a new TA_PARAM_POLY_OR_VOL / TA_PARAM_SPRITE
    struct surface *surf = &tracer->rctx.surfs[surf_id];

    // TODO separator

    nk_layout_row_static(ctx, 40.0f, 40, 1);
    nk_image(ctx, nk_image_id((int)surf->texture));

    nk_layout_row_dynamic(ctx, ctx->style.font->height, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "depth_write: %d", surf->depth_write);
    nk_labelf(ctx, NK_TEXT_LEFT, "depth_func: %s",
              s_depthfunc_names[surf->depth_func]);
    nk_labelf(ctx, NK_TEXT_LEFT, "cull: %s", s_cullface_names[surf->cull]);
    nk_labelf(ctx, NK_TEXT_LEFT, "src_blend: %s",
              s_blendfunc_names[surf->src_blend]);
    nk_labelf(ctx, NK_TEXT_LEFT, "dst_blend: %s",
              s_blendfunc_names[surf->dst_blend]);
    nk_labelf(ctx, NK_TEXT_LEFT, "shade: %s", s_shademode_names[surf->shade]);
    nk_labelf(ctx, NK_TEXT_LEFT, "ignore_tex_alpha: %d",
              surf->ignore_tex_alpha);
    nk_labelf(ctx, NK_TEXT_LEFT, "first_vert: %d", surf->first_vert);
    nk_labelf(ctx, NK_TEXT_LEFT, "num_verts: %d", surf->num_verts);

    // render translated vert only when rendering a vertex tooltip
    if (vertex_type != -1) {
      struct vertex *vert = &tracer->rctx.verts[vert_id];

      // TODO separator

      nk_labelf(ctx, NK_TEXT_LEFT, "xyz: {%.2f, %.2f, %.2f}", vert->xyz[0],
                vert->xyz[1], vert->xyz[2]);
      nk_labelf(ctx, NK_TEXT_LEFT, "uv: {%.2f, %.2f}", vert->uv[0],
                vert->uv[1]);
      nk_labelf(ctx, NK_TEXT_LEFT, "color: 0x%08x", vert->color);
      nk_labelf(ctx, NK_TEXT_LEFT, "offset_color: 0x%08x", vert->offset_color);
    }

    nk_tooltip_end(ctx);
  }
}

static void tracer_render_side_menu(struct tracer *tracer) {
  struct nk_context *ctx = &tracer->window->nk->ctx;

  nk_style_default(ctx);

  // transparent menu backgrounds / selectables
  ctx->style.window.fixed_background.data.color.a = 0;
  ctx->style.selectable.normal.data.color.a = 0;

  {
    struct nk_rect bounds = {0.0f, 0.0, 240.0f,
                             tracer->window->height - SCRUBBER_WINDOW_HEIGHT};

    char label[128];

    if (nk_begin(ctx, "side menu", bounds, 0)) {
      // parem filters
      if (nk_tree_push(ctx, NK_TREE_TAB, "filters", NK_MINIMIZED)) {
        for (int i = 0; i < TA_NUM_PARAMS; i++) {
          snprintf(label, sizeof(label), "Show %s", s_param_names[i]);
          nk_checkbox_text(ctx, label, (int)strlen(label),
                           &tracer->show_params[i]);
        }

        nk_tree_pop(ctx);
      }

      // context parameters
      if (nk_tree_push(ctx, NK_TREE_TAB, "params", 0)) {
        // param list
        int list_type = 0;
        int vertex_type = 0;

        for (int offset = 0; offset < tracer->rctx.num_states; offset++) {
          struct param_state *param_state = &tracer->rctx.states[offset];

          if (param_state_empty(param_state)) {
            continue;
          }

          union pcw pcw = *(union pcw *)(tracer->ctx.params + offset);
          int param_selected = (offset == tracer->current_param_offset);

          if (!tracer_param_hidden(tracer, pcw)) {
            struct nk_rect bounds = nk_widget_bounds(ctx);

            snprintf(label, sizeof(label), "0x%04x %s", offset,
                     s_param_names[pcw.para_type]);
            nk_selectable_label(ctx, label, NK_TEXT_LEFT, &param_selected);

            switch (pcw.para_type) {
              case TA_PARAM_POLY_OR_VOL:
              case TA_PARAM_SPRITE: {
                const union poly_param *param =
                    (const union poly_param *)(tracer->ctx.params + offset);

                list_type = param->type0.pcw.list_type;
                vertex_type = ta_get_vert_type(param->type0.pcw);

                if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
                  tracer_param_tooltip(tracer, list_type, -1, offset);
                }
              } break;

              case TA_PARAM_VERTEX: {
                if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
                  tracer_param_tooltip(tracer, list_type, vertex_type, offset);
                }
              } break;
            }

            if (param_selected) {
              tracer->current_param_offset = offset;
            }
          }
        }

        nk_tree_pop(ctx);
      }

      // texture menu
      if (nk_tree_push(ctx, NK_TREE_TAB, "textures", 0)) {
        nk_layout_row_static(ctx, 40.0f, 40, 4);

        rb_for_each_entry(entry, &tracer->live_textures,
                          struct tracer_texture_entry, live_it) {
          struct nk_rect bounds = nk_widget_bounds(ctx);

          nk_image(ctx, nk_image_id((int)entry->handle));

          if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds)) {
            // disable spacing for tooltip
            struct nk_vec2 original_spacing = ctx->style.window.spacing;
            ctx->style.window.spacing = nk_vec2(0.0f, 0.0f);

            if (nk_tooltip_begin(ctx, 380.0f)) {
              nk_layout_row_static(ctx, 184.0f, 184, 2);

              if (nk_group_begin(ctx, "texture preview",
                                 NK_WINDOW_NO_SCROLLBAR)) {
                nk_layout_row_static(ctx, 184.0f, 184, 1);
                nk_image(ctx, nk_image_id((int)entry->handle));
                nk_group_end(ctx);
              }

              if (nk_group_begin(ctx, "texture info", NK_WINDOW_NO_SCROLLBAR)) {
                nk_layout_row_static(ctx, ctx->style.font->height, 184, 1);
                nk_labelf(ctx, NK_TEXT_LEFT, "addr: 0x%08x",
                          entry->tcw.texture_addr << 3);
                nk_labelf(ctx, NK_TEXT_LEFT, "format: %s",
                          s_pixel_format_names[entry->format]);
                nk_labelf(ctx, NK_TEXT_LEFT, "filter: %s",
                          s_filter_mode_names[entry->filter]);
                nk_labelf(ctx, NK_TEXT_LEFT, "wrap_u: %s",
                          s_wrap_mode_names[entry->wrap_u]);
                nk_labelf(ctx, NK_TEXT_LEFT, "wrap_v: %s",
                          s_wrap_mode_names[entry->wrap_v]);
                nk_labelf(ctx, NK_TEXT_LEFT, "mipmaps: %d", entry->mipmaps);
                nk_labelf(ctx, NK_TEXT_LEFT, "width: %d", entry->width);
                nk_labelf(ctx, NK_TEXT_LEFT, "height: %d", entry->height);
                nk_group_end(ctx);
              }

              nk_tooltip_end(ctx);
            }

            // restore spacing
            ctx->style.window.spacing = original_spacing;
          }
        }

        nk_tree_pop(ctx);
      }
    }

    nk_end(ctx);
  }
}

static void tracer_paint(void *data) {
  struct tracer *tracer = data;

  tr_parse_context(tracer->tr, &tracer->ctx, 0, &tracer->rctx);

  // render ui
  tracer_render_side_menu(tracer);
  tracer_render_scrubber_menu(tracer);

  // clamp surfaces the last surface belonging to the current param
  int n = tracer->rctx.num_surfs;
  int last_idx = n;

  if (tracer->current_param_offset >= 0) {
    const struct param_state *offset =
        &tracer->rctx.states[tracer->current_param_offset];
    last_idx = offset->num_surfs;
  }

  // render the context
  rb_begin_surfaces(tracer->rb, tracer->rctx.projection, tracer->rctx.verts,
                    tracer->rctx.num_verts);

  for (int i = 0; i < n; i++) {
    int idx = tracer->rctx.sorted_surfs[i];

    // if this surface comes after the current parameter, ignore it
    if (idx >= last_idx) {
      continue;
    }

    rb_draw_surface(tracer->rb, &tracer->rctx.surfs[idx]);
  }

  rb_end_surfaces(tracer->rb);
}

static void tracer_keydown(void *data, int device_index, enum keycode code,
                           int16_t value) {
  struct tracer *tracer = data;

  if (code == K_F1) {
    if (value) {
      win_enable_debug_menu(tracer->window, !tracer->window->debug_menu);
    }
  } else if (code == K_LEFT && value) {
    tracer_prev_context(tracer);
  } else if (code == K_RIGHT && value) {
    tracer_next_context(tracer);
  } else if (code == K_UP && value) {
    tracer_prev_param(tracer);
  } else if (code == K_DOWN && value) {
    tracer_next_param(tracer);
  }
}

static void tracer_close(void *data) {
  struct tracer *tracer = data;

  tracer->running = false;
}

static bool tracer_parse(struct tracer *tracer, const char *path) {
  if (tracer->trace) {
    trace_destroy(tracer->trace);
    tracer->trace = NULL;
  }

  tracer->trace = trace_parse(path);

  if (!tracer->trace) {
    LOG_WARNING("Failed to parse %s", path);
    return false;
  }

  tracer_reset_context(tracer);

  return true;
}

void tracer_run(struct tracer *tracer, const char *path) {
  if (!tracer_parse(tracer, path)) {
    return;
  }

  tracer->running = true;

  while (tracer->running) {
    win_pump_events(tracer->window);
  }
}

struct tracer *tracer_create(struct window *window) {
  // ensure param / poly / vertex size LUTs are generated
  ta_build_tables();

  struct tracer *tracer = calloc(1, sizeof(struct tracer));

  tracer->window = window;
  tracer->listener = (struct window_listener){
      tracer,          &tracer_paint, NULL, NULL,          NULL,
      &tracer_keydown, NULL,          NULL, &tracer_close, {0}};
  tracer->provider =
      (struct texture_provider){tracer, &tracer_texture_provider_find_texture};
  tracer->rb = window->rb;
  tracer->tr = tr_create(tracer->rb, &tracer->provider);

  win_add_listener(tracer->window, &tracer->listener);

  // setup tile context buffers
  tracer->ctx.params = tracer->params;

  // setup render context buffers
  tracer->rctx.surfs = tracer->surfs;
  tracer->rctx.surfs_size = array_size(tracer->surfs);
  tracer->rctx.verts = tracer->verts;
  tracer->rctx.verts_size = array_size(tracer->verts);
  tracer->rctx.sorted_surfs = tracer->sorted_surfs;
  tracer->rctx.sorted_surfs_size = array_size(tracer->sorted_surfs);
  tracer->rctx.states = tracer->states;
  tracer->rctx.states_size = array_size(tracer->states);

  // add all textures to free list
  for (int i = 0, n = array_size(tracer->textures); i < n; i++) {
    struct tracer_texture_entry *entry = &tracer->textures[i];
    list_add(&tracer->free_textures, &entry->free_it);
  }

  // initial param filters
  tracer->show_params[TA_PARAM_END_OF_LIST] = 1;
  tracer->show_params[TA_PARAM_USER_TILE_CLIP] = 1;
  tracer->show_params[TA_PARAM_OBJ_LIST_SET] = 1;
  tracer->show_params[TA_PARAM_RESERVED0] = 1;
  tracer->show_params[TA_PARAM_POLY_OR_VOL] = 1;
  tracer->show_params[TA_PARAM_SPRITE] = 1;
  tracer->show_params[TA_PARAM_RESERVED1] = 1;
  tracer->show_params[TA_PARAM_VERTEX] = false;

  return tracer;
}

void tracer_destroy(struct tracer *tracer) {
  if (tracer->trace) {
    trace_destroy(tracer->trace);
  }
  win_remove_listener(tracer->window, &tracer->listener);
  tr_destroy(tracer->tr);
  free(tracer);
}
