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

/** \file
 * \ingroup eevee
 *
 * An instance contains all structures needed to do a complete render.
 */

#pragma once

#include "BKE_object.h"
#include "DEG_depsgraph_query.h"
#include "DNA_ID.h"

#include "eevee_film.hh"
#include "eevee_light.hh"
#include "eevee_motion_blur.hh"
#include "eevee_renderpasses.hh"
#include "eevee_sampling.hh"
#include "eevee_shader.hh"
#include "eevee_view.hh"

#include "eevee_engine.h"

namespace blender::eevee {

class Instance {
 private:
  /** Random number generator, this is its persistent state. */
  Sampling sampling_;
  /** Outputs passes. */
  RenderPasses render_passes_;
  /** Shader module. shared between instances. */
  ShaderModule &shaders_;
  /** Shading passes. Shared between views. Objects will subscribe to one of them. */
  ShadingPasses shading_passes_;
  /** Shaded view(s) for the main output. */
  MainView main_view_;
  /** Point of view in the scene. Can be init from viewport or camera object. */
  Camera camera_;
  /** Velocity module containing motion data. */
  Velocity velocity_;
  /** Motion blur data. */
  MotionBlurModule motion_blur_;
  /** Lookdev own lightweight instance. May not be allocated. */
  // Lookdev *lookdev_ = nullptr;

  Scene *scene_ = nullptr;
  ViewLayer *view_layer_ = nullptr;
  Depsgraph *depsgraph_ = nullptr;
  /** Only available when rendering for final render. */
  const RenderLayer *render_layer_ = nullptr;
  RenderEngine *render_ = nullptr;
  /** Only available when rendering for viewport. */
  const DRWView *drw_view_ = nullptr;
  const View3D *v3d_ = nullptr;
  const RegionView3D *rv3d_ = nullptr;

 public:
  Instance(ShaderModule &shared_shaders)
      : render_passes_(shared_shaders, camera_, sampling_),
        shaders_(shared_shaders),
        shading_passes_(shared_shaders, lights_, camera_, velocity_, scene_data_),
        main_view_(shared_shaders, shading_passes_, camera_, sampling_, motion_blur_),
        camera_(sampling_),
        velocity_(),
        motion_blur_(camera_, sampling_, velocity_){};
  ~Instance(){};

  /**
   * Init funcion that needs to be called once at the start of a frame.
   * Active camera, render extent and enabled render passes are immutable until next init.
   * This takes care of resizing output buffers and view in case a parameter changed.
   * IMPORTANT: xxx.init() functions are NOT meant to acquire and allocate DRW resources.
   * Any attempt to do so will likely produce use after free situations.
   **/
  void init(const int output_res[2],
            const rcti *output_rect,
            RenderEngine *render,
            Depsgraph *depsgraph,
            Object *camera_object = nullptr,
            const RenderLayer *render_layer = nullptr,
            const DRWView *drw_view = nullptr,
            const View3D *v3d = nullptr,
            const RegionView3D *rv3d = nullptr)
  {
    BLI_assert(camera_object || drw_view);

    render_ = render;
    scene_ = DEG_get_evaluated_scene(depsgraph);
    view_layer_ = DEG_get_evaluated_view_layer(depsgraph);
    depsgraph_ = depsgraph;
    render_layer_ = render_layer;
    drw_view_ = drw_view;
    v3d_ = v3d;
    rv3d_ = rv3d;

    rcti rect;
    {
      rcti rect_full;
      BLI_rcti_init(&rect_full, 0, output_res[0], 0, output_res[1]);
      /* Clip the render border to region bounds. */
      BLI_rcti_isect(output_rect, &rect_full, &rect);
      if (BLI_rcti_is_empty(&rect)) {
        BLI_rcti_init(&rect, 0, output_res[0], 0, output_res[1]);
      }
      output_rect = &rect;
    }

    sampling_.init(scene_);
    camera_.init(render_, depsgraph_, camera_object, drw_view_);
    motion_blur_.init(scene_, render, depsgraph_);
    render_passes_.init(scene_, render_layer, v3d_, output_res, output_rect);
    main_view_.init(scene_, output_res);
    velocity_.init(camera_, render_, depsgraph_, render_passes_);
  }

  /**
   * Sync with gather data from the scene that can change over a time step.
   * IMPORTANT: xxx.sync() functions area responsible for creating DRW resources (i.e: DRWView) as
   * well as querying temp texture pool. All DRWPasses should be ready by the end end_sync().
   **/
  void begin_sync()
  {
    camera_.sync();
    render_passes_.sync();
    shading_passes_.sync();
    main_view_.sync();

    velocity_.begin_sync(camera_);
  }

  void object_sync(Object *ob)
  {
    const int ob_visibility = DRW_object_visibility_in_active_context(ob);
    const bool partsys_is_visible = (ob_visibility & OB_VISIBLE_PARTICLES) != 0;
    const bool object_is_visible = DRW_object_is_renderable(ob) &&
                                   (ob_visibility & OB_VISIBLE_SELF) != 0;

    if (!partsys_is_visible && !object_is_visible) {
      return;
    }

    /* Gather recalc flag. */
    DrawEngineType *owner = (DrawEngineType *)&DRW_engine_viewport_eevee_type;
    DrawData *dd = DRW_drawdata_ensure((ID *)ob, owner, sizeof(DrawData), nullptr, nullptr);
    if (dd->recalc != 0) {
      dd->recalc = 0;
      sampling_.reset();
    }

    if (partsys_is_visible) {
      /* TODO render particle hair. */
    }

    if (object_is_visible) {
      switch (ob->type) {
        case OB_MESH:
          shading_passes_.opaque.surface_add(ob, nullptr, 0);
          shading_passes_.velocity.mesh_add(ob);
          break;
        default:
          break;
      }
    }
  }

  /* Wrapper to use with DRW_render_object_iter. */
  static void object_sync(void *instance_, Object *ob, RenderEngine *engine, Depsgraph *depsgraph)
  {
    UNUSED_VARS(engine, depsgraph);
    reinterpret_cast<Instance *>(instance_)->object_sync(ob);
  }

  void end_sync(void)
  {
    velocity_.end_sync();
    sampling_.end_sync();
    render_passes_.end_sync();

    scene_data_.push_update();
  }

  void render_sync(void)
  {
    DRW_cache_restart();

    this->begin_sync();
    DRW_render_object_iter(this, render_, depsgraph_, object_sync);
    this->end_sync();

    DRW_render_instance_buffer_finish();
    /* Also we weed to have a correct fbo bound for DRW_hair_update */
    // GPU_framebuffer_bind();
    // DRW_hair_update();
  }

  /**
   * Conceptually renders one sample per pixel.
   * Everything based on random sampling should be done here (i.e: DRWViews jitter)
   **/
  void render_sample(void)
  {
    if (sampling_.finished()) {
      return;
    }

    /* Motion blur may need to do re-sync after a certain number of sample. */
    if (sampling_.do_render_sync()) {
      this->render_sync();
    }

    sampling_.step();

    /* TODO update shadowmaps, planars, etc... */
    // shadow_view_.render();

    main_view_.render(render_passes_);

    motion_blur_.step();
  }

  void render_frame(RenderLayer *render_layer, const char *view_name)
  {
    while (!sampling_.finished()) {
      this->render_sample();
      /* TODO(fclem) print progression. */
    }

    render_passes_.read_result(render_layer, view_name);
  }

  void draw_viewport(DefaultFramebufferList *dfbl)
  {
    this->render_sample();

    render_passes_.resolve_viewport(dfbl);

    // if (lookdev_) {
    // lookdev_->resolve_onto(dfbl->default_fb);
    // }

    if (!sampling_.finished()) {
      DRW_viewport_request_redraw();
    }
  }

  void view_update(void)
  {
    sampling_.reset();
  }

  bool finished(void) const
  {
    return sampling_.finished();
  }
};

}  // namespace blender::eevee