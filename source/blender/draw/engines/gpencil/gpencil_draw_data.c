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
 * Copyright 2019, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include "DRW_render.h"

#include "DNA_light_types.h"

#include "BKE_image.h"

#include "BLI_memblock.h"

#include "GPU_uniformbuffer.h"

#include "IMB_imbuf_types.h"

#include "gpencil_engine.h"

GPENCIL_LightPool *gpencil_light_pool_add(GPENCIL_PrivateData *pd)
{
  GPENCIL_LightPool *lightpool = BLI_memblock_alloc(pd->gp_light_pool);
  lightpool->light_used = 0;
  /* Tag light list end. */
  lightpool->light_data[0].color[0] = -1.0;
  if (lightpool->ubo == NULL) {
    lightpool->ubo = GPU_uniformbuffer_create(sizeof(lightpool->light_data), NULL, NULL);
  }
  pd->last_light_pool = lightpool;
  return lightpool;
}

static GPENCIL_MaterialPool *gpencil_material_pool_add(GPENCIL_PrivateData *pd)
{
  GPENCIL_MaterialPool *matpool = BLI_memblock_alloc(pd->gp_material_pool);
  matpool->next = NULL;
  if (matpool->ubo == NULL) {
    matpool->ubo = GPU_uniformbuffer_create(sizeof(matpool->mat_data), NULL, NULL);
  }
  pd->last_material_pool = matpool;
  return matpool;
}

static struct GPUTexture *gpencil_image_texture_get(Image *image, bool *r_alpha_premult)
{
  ImBuf *ibuf;
  ImageUser iuser = {NULL};
  struct GPUTexture *gpu_tex = NULL;
  void *lock;

  iuser.ok = true;
  ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);

  if (ibuf != NULL && ibuf->rect != NULL) {
    gpu_tex = GPU_texture_from_blender(image, &iuser, GL_TEXTURE_2D);
    *r_alpha_premult = (image->alpha_mode == IMA_ALPHA_PREMUL);
  }
  BKE_image_release_ibuf(image, ibuf, lock);

  return gpu_tex;
}

static void gpencil_uv_transform_get(const float ofs[2],
                                     const float scale[2],
                                     const float rotation,
                                     float r_uvmat[3][2])
{
  /* OPTI this could use 3x2 matrices and reduce the number of operations drastically. */
  float mat[4][4];
  float scale_v3[3] = {scale[0], scale[1], 0.0};
  /* Scale */
  size_to_mat4(mat, scale_v3);
  /* Offset to center. */
  translate_m4(mat, 0.5f + ofs[0], 0.5f + ofs[1], 0.0f);
  /* Rotation; */
  rotate_m4(mat, 'Z', -rotation);
  /* Translate. */
  translate_m4(mat, -0.5f, -0.5f, 0.0f);
  /* Convert to 3x2 */
  copy_v2_v2(r_uvmat[0], mat[0]);
  copy_v2_v2(r_uvmat[1], mat[1]);
  copy_v2_v2(r_uvmat[2], mat[3]);
}

/**
 * Creates a linked list of material pool containing all materials assigned for a given object.
 * We merge the material pools together if object does not contain a huge amount of materials.
 * Also return an offset to the first material of the object in the ubo.
 **/
GPENCIL_MaterialPool *gpencil_material_pool_create(GPENCIL_PrivateData *pd, Object *ob, int *ofs)
{
  GPENCIL_MaterialPool *matpool = pd->last_material_pool;

  /* TODO reuse matpool for objects with small material count. */
  if (true) {
    matpool = gpencil_material_pool_add(pd);
  }

  GPENCIL_MaterialPool *pool = matpool;
  for (int i = 0; i < ob->totcol; i++) {
    int mat_id = (i % GPENCIL_MATERIAL_BUFFER_LEN);
    if ((i > 0) && (mat_id == 0)) {
      pool->next = gpencil_material_pool_add(pd);
      pool = pool->next;
    }
    gpMaterial *mat_data = &pool->mat_data[mat_id];
    MaterialGPencilStyle *gp_style = BKE_material_gpencil_settings_get(ob, i + 1);

    if (gp_style->mode == GP_STYLE_MODE_LINE) {
      mat_data->flag = 0;
    }
    else {
      switch (gp_style->alignment_mode) {
        case GP_STYLE_FOLLOW_PATH:
          mat_data->flag = GP_STROKE_ALIGNMENT_STROKE;
          break;
        case GP_STYLE_FOLLOW_OBJ:
          mat_data->flag = GP_STROKE_ALIGNMENT_OBJECT;
          break;
        case GP_STYLE_FOLLOW_FIXED:
        default:
          mat_data->flag = GP_STROKE_ALIGNMENT_FIXED;
          break;
      }

      if (gp_style->mode == GP_STYLE_MODE_DOTS) {
        mat_data->flag |= GP_STROKE_DOTS;
      }
    }

    if ((gp_style->mode != GP_STYLE_MODE_LINE) || (gp_style->flag & GP_STYLE_DISABLE_STENCIL)) {
      mat_data->flag |= GP_STROKE_OVERLAP;
    }

    /* Stroke Style */
    if ((gp_style->stroke_style == GP_STYLE_STROKE_STYLE_TEXTURE) && (gp_style->sima)) {
      /* TODO finish. */
      bool premul;
      pool->tex_stroke[mat_id] = gpencil_image_texture_get(gp_style->sima, &premul);
      mat_data->flag |= pool->tex_stroke[mat_id] ? GP_STROKE_TEXTURE_USE : 0;
      mat_data->flag |= premul ? GP_STROKE_TEXTURE_PREMUL : 0;
      copy_v4_v4(mat_data->stroke_color, gp_style->stroke_rgba);
      mat_data->stroke_texture_mix = 1.0f - gp_style->mix_stroke_factor;
    }
    else /* if (gp_style->stroke_style == GP_STYLE_STROKE_STYLE_SOLID) */ {
      pool->tex_stroke[mat_id] = NULL;
      mat_data->flag &= ~GP_STROKE_TEXTURE_USE;
      copy_v4_v4(mat_data->stroke_color, gp_style->stroke_rgba);
      mat_data->stroke_texture_mix = 0.0f;
    }

    /* Fill Style */
    if ((gp_style->fill_style == GP_STYLE_FILL_STYLE_TEXTURE) && (gp_style->ima)) {
      bool use_clip = (gp_style->flag & GP_STYLE_COLOR_TEX_CLAMP) != 0;
      bool premul;
      pool->tex_fill[mat_id] = gpencil_image_texture_get(gp_style->ima, &premul);
      mat_data->flag |= pool->tex_fill[mat_id] ? GP_FILL_TEXTURE_USE : 0;
      mat_data->flag |= premul ? GP_FILL_TEXTURE_PREMUL : 0;
      mat_data->flag |= use_clip ? GP_FILL_TEXTURE_CLIP : 0;
      gpencil_uv_transform_get(gp_style->texture_offset,
                               gp_style->texture_scale,
                               gp_style->texture_angle,
                               mat_data->fill_uv_transform);
      copy_v4_v4(mat_data->fill_color, gp_style->fill_rgba);
      mat_data->fill_texture_mix = 1.0f - gp_style->mix_factor;
    }
    else if (gp_style->fill_style == GP_STYLE_FILL_STYLE_TEXTURE) {
      /* TODO implement gradient as a texture. */
      pool->tex_fill[mat_id] = NULL;
      mat_data->flag &= ~GP_FILL_TEXTURE_USE;
      copy_v4_v4(mat_data->fill_color, gp_style->fill_rgba);
      mat_data->fill_texture_mix = 0.0f;
    }
    else /* if (gp_style->fill_style == GP_STYLE_FILL_STYLE_SOLID) */ {
      pool->tex_fill[mat_id] = NULL;
      mat_data->flag &= ~GP_FILL_TEXTURE_USE;
      copy_v4_v4(mat_data->fill_color, gp_style->fill_rgba);
      mat_data->fill_texture_mix = 0.0f;
    }
  }

  *ofs = 0;

  return matpool;
}

void gpencil_material_resources_get(GPENCIL_MaterialPool *first_pool,
                                    int mat_id,
                                    GPUTexture **r_tex_stroke,
                                    GPUTexture **r_tex_fill,
                                    GPUUniformBuffer **r_ubo_mat)
{
  GPENCIL_MaterialPool *matpool = first_pool;
  int pool_id = mat_id / GPENCIL_MATERIAL_BUFFER_LEN;
  for (int i = 0; i < pool_id; i++) {
    matpool = matpool->next;
  }
  mat_id = mat_id % GPENCIL_MATERIAL_BUFFER_LEN;
  *r_ubo_mat = matpool->ubo;
  if (r_tex_fill) {
    *r_tex_fill = matpool->tex_fill[mat_id];
  }
  if (r_tex_stroke) {
    *r_tex_stroke = matpool->tex_stroke[mat_id];
  }
}

void gpencil_light_ambient_add(GPENCIL_LightPool *lightpool, const float color[3])
{
  if (lightpool->light_used >= GPENCIL_LIGHT_BUFFER_LEN) {
    return;
  }

  gpLight *gp_light = &lightpool->light_data[lightpool->light_used];
  gp_light->type = GP_LIGHT_TYPE_AMBIENT;
  copy_v3_v3(gp_light->color, color);
  lightpool->light_used++;

  if (lightpool->light_used < GPENCIL_LIGHT_BUFFER_LEN) {
    /* Tag light list end. */
    gp_light[1].color[0] = -1.0f;
  }
}

static float light_power_get(const Light *la)
{
  if (la->type == LA_AREA) {
    return 1.0f / (4.0f * M_PI);
  }
  else if (la->type == LA_SPOT || la->type == LA_LOCAL) {
    return 1.0f / (4.0f * M_PI * M_PI);
  }
  else {
    return 1.0f / M_PI;
  }
}

void gpencil_light_pool_populate(GPENCIL_LightPool *lightpool, Object *ob)
{
  Light *la = (Light *)ob->data;

  if (lightpool->light_used >= GPENCIL_LIGHT_BUFFER_LEN) {
    return;
  }

  gpLight *gp_light = &lightpool->light_data[lightpool->light_used];
  float(*mat)[4] = (float(*)[4])gp_light->right;

  if (la->type == LA_SPOT) {
    copy_m4_m4(mat, ob->imat);
    gp_light->type = GP_LIGHT_TYPE_SPOT;
    gp_light->spotsize = cosf(la->spotsize * 0.5f);
    gp_light->spotblend = (1.0f - gp_light->spotsize) * la->spotblend;
  }
  else if (la->type == LA_AREA) {
    /* Simulate area lights using a spot light. */
    normalize_m4_m4(mat, ob->obmat);
    invert_m4(mat);
    gp_light->type = GP_LIGHT_TYPE_SPOT;
    gp_light->spotsize = cosf(M_PI * 0.5f);
    gp_light->spotblend = (1.0f - gp_light->spotsize) * 1.0f;
  }
  else if (la->type == LA_SUN) {
    normalize_v3_v3(gp_light->forward, ob->obmat[2]);
    gp_light->type = GP_LIGHT_TYPE_SUN;
  }
  else {
    gp_light->type = GP_LIGHT_TYPE_POINT;
  }
  copy_v4_v4(gp_light->position, ob->obmat[3]);
  copy_v3_v3(gp_light->color, &la->r);
  mul_v3_fl(gp_light->color, la->energy * light_power_get(la));

  lightpool->light_used++;

  if (lightpool->light_used < GPENCIL_LIGHT_BUFFER_LEN) {
    /* Tag light list end. */
    gp_light[1].color[0] = -1.0f;
  }
}

/**
 * Creates a single pool containing all lights assigned (light linked) for a given object.
 **/
GPENCIL_LightPool *gpencil_light_pool_create(GPENCIL_PrivateData *pd, Object *UNUSED(ob))
{
  GPENCIL_LightPool *lightpool = pd->last_light_pool;

  if (lightpool == NULL) {
    lightpool = gpencil_light_pool_add(pd);
  }
  /* TODO */
  // gpencil_light_pool_populate(lightpool, ob);

  return lightpool;
}

void gpencil_material_pool_free(void *storage)
{
  GPENCIL_MaterialPool *matpool = (GPENCIL_MaterialPool *)storage;
  DRW_UBO_FREE_SAFE(matpool->ubo);
}

void gpencil_light_pool_free(void *storage)
{
  GPENCIL_LightPool *lightpool = (GPENCIL_LightPool *)storage;
  DRW_UBO_FREE_SAFE(lightpool->ubo);
}

static void gpencil_view_layer_data_free(void *storage)
{
  GPENCIL_ViewLayerData *vldata = (GPENCIL_ViewLayerData *)storage;

  BLI_memblock_destroy(vldata->gp_light_pool, gpencil_light_pool_free);
  BLI_memblock_destroy(vldata->gp_material_pool, gpencil_material_pool_free);
  BLI_memblock_destroy(vldata->gp_object_pool, NULL);
  BLI_memblock_destroy(vldata->gp_layer_pool, NULL);
  BLI_memblock_destroy(vldata->gp_vfx_pool, NULL);
}

GPENCIL_ViewLayerData *GPENCIL_view_layer_data_ensure(void)
{
  GPENCIL_ViewLayerData **vldata = (GPENCIL_ViewLayerData **)DRW_view_layer_engine_data_ensure(
      &draw_engine_gpencil_type, gpencil_view_layer_data_free);

  /* NOTE(fclem) Putting this stuff in viewlayer means it is shared by all viewports.
   * For now it is ok, but in the future, it could become a problem if we implement
   * the caching system. */
  if (*vldata == NULL) {
    *vldata = MEM_callocN(sizeof(**vldata), "GPENCIL_ViewLayerData");

    (*vldata)->gp_light_pool = BLI_memblock_create(sizeof(GPENCIL_LightPool));
    (*vldata)->gp_material_pool = BLI_memblock_create(sizeof(GPENCIL_MaterialPool));
    (*vldata)->gp_object_pool = BLI_memblock_create(sizeof(GPENCIL_tObject));
    (*vldata)->gp_layer_pool = BLI_memblock_create(sizeof(GPENCIL_tLayer));
    (*vldata)->gp_vfx_pool = BLI_memblock_create(sizeof(GPENCIL_tVfx));
  }

  return *vldata;
}