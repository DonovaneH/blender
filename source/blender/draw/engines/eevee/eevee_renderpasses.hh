/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2021, Blender Foundation.
 */

#pragma once

#include "BLI_hash_tables.hh"
#include "BLI_vector.hh"

#include "RE_pipeline.h"

#include "eevee_film.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name eRenderPassBit
 *
 * This enum might seems redundant but there is an opportunity to use it for internal debug passes.
 * \{ */

enum eRenderPassBit {
  RENDERPASS_NONE = 0,
  RENDERPASS_COMBINED = (1 << 0),
  RENDERPASS_DEPTH = (1 << 1),
  RENDERPASS_NORMAL = (1 << 2),
  RENDERPASS_VECTOR = (1 << 3),
  /** Used for iterator. */
  RENDERPASS_MAX,
};

ENUM_OPERATORS(eRenderPassBit, RENDERPASS_NORMAL)

static eRenderPassBit to_render_passes_bits(int i_rpasses)
{
  eRenderPassBit rpasses = RENDERPASS_NONE;
  SET_FLAG_FROM_TEST(rpasses, i_rpasses & SCE_PASS_COMBINED, RENDERPASS_COMBINED);
  SET_FLAG_FROM_TEST(rpasses, i_rpasses & SCE_PASS_Z, RENDERPASS_DEPTH);
  SET_FLAG_FROM_TEST(rpasses, i_rpasses & SCE_PASS_NORMAL, RENDERPASS_NORMAL);
  SET_FLAG_FROM_TEST(rpasses, i_rpasses & SCE_PASS_VECTOR, RENDERPASS_VECTOR);
  return rpasses;
}

static const char *to_render_passes_name(eRenderPassBit rpass)
{
  switch (rpass) {
    case RENDERPASS_COMBINED:
      return RE_PASSNAME_COMBINED;
    case RENDERPASS_DEPTH:
      return RE_PASSNAME_Z;
    case RENDERPASS_NORMAL:
      return RE_PASSNAME_NORMAL;
    case RENDERPASS_VECTOR:
      return RE_PASSNAME_VECTOR;
    default:
      BLI_assert(0);
      return "";
  }
}

static eFilmDataType to_render_passes_data_type(eRenderPassBit rpass, const bool use_log_encoding)
{
  switch (rpass) {
    case RENDERPASS_COMBINED:
      return (use_log_encoding) ? FILM_DATA_COLOR_LOG : FILM_DATA_COLOR;
    case RENDERPASS_DEPTH:
      return FILM_DATA_DEPTH;
    case RENDERPASS_NORMAL:
      return FILM_DATA_NORMAL;
    case RENDERPASS_VECTOR:
      return FILM_DATA_MOTION;
    default:
      BLI_assert(0);
      return FILM_DATA_COLOR;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RenderPasses
 * \{ */

class RenderPasses {
 public:
  /** Film for each render pass. A nullptr means the pass is not needed. */
  Film *combined = nullptr;
  Film *depth = nullptr;
  Film *normal = nullptr;
  Film *vector = nullptr;
  Vector<Film *> aovs;

 private:
  ShaderModule &shaders_;
  Camera &camera_;
  Sampling &sampling_;
  eRenderPassBit enabled_passes_ = RENDERPASS_NONE;

 public:
  RenderPasses(ShaderModule &shaders, Camera &camera, Sampling &sampling)
      : shaders_(shaders), camera_(camera), sampling_(sampling){};

  ~RenderPasses()
  {
    delete combined;
    delete depth;
    delete normal;
    delete vector;
  }

  void init(const Scene *scene,
            const RenderLayer *render_layer,
            const View3D *v3d,
            const int extent[2],
            const rcti *output_rect)
  {
    eRenderPassBit enabled_passes;
    if (render_layer) {
      enabled_passes = to_render_passes_bits(render_layer->passflag);
      /* Cannot output motion vectors when using motion blur. */
      if (scene->eevee.flag & SCE_EEVEE_MOTION_BLUR_ENABLED) {
        enabled_passes &= ~RENDERPASS_VECTOR;
      }
    }
    else {
      BLI_assert(v3d);
      enabled_passes = to_render_passes_bits(v3d->shading.render_pass);
      /* We need the depth pass for compositing overlays or GPencil. */
      if (!DRW_state_is_scene_render()) {
        enabled_passes |= RENDERPASS_DEPTH;
      }
    }

    const bool use_log_encoding = scene->eevee.flag & SCE_EEVEE_FILM_LOG_ENCODING;

    rcti fallback_rect;
    if (BLI_rcti_is_empty(output_rect)) {
      BLI_rcti_init(&fallback_rect, 0, extent[0], 0, extent[1]);
      output_rect = &fallback_rect;
    }

    /* HACK to iterate over all passes. */
    enabled_passes_ = static_cast<eRenderPassBit>(0xFFFFFFFF);
    for (RenderPassItem rpi : *this) {
      bool enable = (enabled_passes & rpi.pass_bit) != 0;
      if (enable && rpi.film == nullptr) {
        rpi.film = new Film(shaders_,
                            camera_,
                            sampling_,
                            to_render_passes_data_type(rpi.pass_bit, use_log_encoding),
                            to_render_passes_name(rpi.pass_bit));
      }
      else if (!enable && rpi.film != nullptr) {
        /* Delete unused passes. */
        delete rpi.film;
        rpi.film = nullptr;
      }

      if (rpi.film) {
        rpi.film->init(extent, output_rect);
      }
    }

    enabled_passes_ = enabled_passes;
  }

  void sync(void)
  {
    for (RenderPassItem rpi : *this) {
      rpi.film->sync();
    }
  }

  void end_sync(void)
  {
    for (RenderPassItem rpi : *this) {
      rpi.film->end_sync();
    }
  }

  void resolve_viewport(DefaultFramebufferList *dfbl)
  {
    for (RenderPassItem rpi : *this) {
      if (rpi.pass_bit == RENDERPASS_DEPTH) {
        rpi.film->resolve_viewport(dfbl->depth_only_fb);
      }
      else {
        /* Ensures only one color render pass is enabled. */
        BLI_assert((enabled_passes_ & ~RENDERPASS_DEPTH) == rpi.pass_bit);
        rpi.film->resolve_viewport(dfbl->color_only_fb);
      }
    }
  }

  void read_result(RenderLayer *render_layer, const char *view_name)
  {
    for (RenderPassItem rpi : *this) {
      const char *pass_name = to_render_passes_name(rpi.pass_bit);
      RenderPass *rp = RE_pass_find_by_name(render_layer, pass_name, view_name);
      if (rp) {
        rpi.film->read_result(rp->rect);
      }
    }
  }

 private:
  constexpr Film *&render_pass_bit_to_film_p(eRenderPassBit rpass)
  {
    switch (rpass) {
      case RENDERPASS_COMBINED:
        return combined;
      case RENDERPASS_DEPTH:
        return depth;
      case RENDERPASS_NORMAL:
        return normal;
      case RENDERPASS_VECTOR:
        return vector;
      default:
        BLI_assert(0);
        return combined;
    }
  }

  /**
   * Iterator
   **/

  struct RenderPassItem {
    Film *&film;
    eRenderPassBit pass_bit;

    constexpr explicit RenderPassItem(Film *&film_, eRenderPassBit pass_bit_)
        : film(film_), pass_bit(pass_bit_){};
  };

  class Iterator {
   private:
    RenderPasses &render_passes_;
    int64_t current_;

   public:
    constexpr explicit Iterator(RenderPasses &rpasses, int64_t current)
        : render_passes_(rpasses), current_(current){};

    constexpr Iterator &operator++()
    {
      while (current_ < RENDERPASS_MAX) {
        current_ <<= 1;
        if (current_ & render_passes_.enabled_passes_) {
          break;
        }
      }
      return *this;
    }

    constexpr friend bool operator!=(const Iterator &a, const Iterator &b)
    {
      return a.current_ != b.current_;
    }

    constexpr RenderPassItem operator*()
    {
      eRenderPassBit pass_bit = static_cast<eRenderPassBit>(current_);
      return RenderPassItem(render_passes_.render_pass_bit_to_film_p(pass_bit), pass_bit);
    }
  };

  /* Iterator over all enabled passes. */
  constexpr Iterator begin()
  {
    return Iterator(*this, 1);
  }

  constexpr Iterator end()
  {
    return Iterator(*this, power_of_2_max_constexpr(RENDERPASS_MAX));
  }
};

/** \} */

}  // namespace blender::eevee