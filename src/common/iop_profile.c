/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/iop_profile.h"
#include "common/debug.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Note :
 * we do not use finite-math-only and fast-math because divisions by zero are not manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "fp-contract=fast", \
                      "tree-vectorize")
#endif

static void _transform_from_to_rgb_lab_lcms2(const float *const image_in, float *const image_out, const int width,
                                             const int height, const dt_colorspaces_color_profile_type_t type,
                                             const char *filename, const int intent, const int direction)
{
  const int ch = 4;
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *rgb_profile = NULL;
  cmsHPROFILE *lab_profile = NULL;

  if(type != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_WORK);
    if(profile) rgb_profile = profile->profile;
  }
  else
    rgb_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_WORK)->profile;
  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
        fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
                (char)(rgb_color_space>>24),
                (char)(rgb_color_space>>16),
                (char)(rgb_color_space>>8),
                (char)(rgb_color_space));
        rgb_profile = NULL;
    }
  }
  if(rgb_profile == NULL)
  {
    rgb_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_WORK)->profile;
    fprintf(stderr, _("unsupported working profile %s has been replaced by Rec2020 RGB!\n"), filename);
  }

  lab_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_LabA_FLT;

  if(direction == 1) // rgb --> lab
  {
    input_profile = rgb_profile;
    input_format = TYPE_RGBA_FLT;
    output_profile = lab_profile;
    output_format = TYPE_LabA_FLT;
  }
  else // lab -->rgb
  {
    input_profile = lab_profile;
    input_format = TYPE_LabA_FLT;
    output_profile = rgb_profile;
    output_format = TYPE_RGBA_FLT;
  }

  xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);
  if(xform)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(image_in, image_out, width, height, ch) \
    shared(xform) \
    schedule(static)
#endif
    for(int y = 0; y < height; y++)
    {
      const float *const in = image_in + y * width * ch;
      float *const out = image_out + y * width * ch;

      cmsDoTransform(xform, in, out, width);
    }
  }
  else
    fprintf(stderr, "[_transform_from_to_rgb_lab_lcms2] cannot create transform\n");

  if(xform) cmsDeleteTransform(xform);
}

static void _transform_rgb_to_rgb_lcms2(const float *const image_in, float *const image_out, const int width,
                                        const int height, const dt_colorspaces_color_profile_type_t type_from,
                                        const char *filename_from,
                                        const dt_colorspaces_color_profile_type_t type_to, const char *filename_to,
                                        const int intent)
{
  const int ch = 4;
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *from_rgb_profile = NULL;
  cmsHPROFILE *to_rgb_profile = NULL;

  if(type_from == DT_COLORSPACE_DISPLAY || type_to == DT_COLORSPACE_DISPLAY || type_from == DT_COLORSPACE_DISPLAY2
     || type_to == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  if(type_from != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile_from
        = dt_colorspaces_get_profile(type_from, filename_from, DT_PROFILE_DIRECTION_ANY);
    if(profile_from) from_rgb_profile = profile_from->profile;
  }
  else
  {
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid from profile\n");
  }

  if(type_to != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *profile_to
        = dt_colorspaces_get_profile(type_to, filename_to, DT_PROFILE_DIRECTION_ANY);
    if(profile_to) to_rgb_profile = profile_to->profile;
  }
  else
  {
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid to profile\n");
  }

  if(from_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(from_rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      from_rgb_profile = NULL;
    }
  }
  if(to_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(to_rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      to_rgb_profile = NULL;
    }
  }

  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  input_profile = from_rgb_profile;
  input_format = TYPE_RGBA_FLT;
  output_profile = to_rgb_profile;
  output_format = TYPE_RGBA_FLT;

  if(input_profile && output_profile)
    xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);

  if(type_from == DT_COLORSPACE_DISPLAY || type_to == DT_COLORSPACE_DISPLAY || type_from == DT_COLORSPACE_DISPLAY2
     || type_to == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  if(xform)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(image_in, image_out, width, height, ch) \
    shared(xform) \
    schedule(static)
#endif
    for(int y = 0; y < height; y++)
    {
      const float *const in = image_in + y * width * ch;
      float *const out = image_out + y * width * ch;

      cmsDoTransform(xform, in, out, width);
    }
  }
  else
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] cannot create transform\n");

  if(xform) cmsDeleteTransform(xform);
}

static void _transform_lcms2(struct dt_iop_module_t *self, const float *const image_in, float *const image_out,
                             const int width, const int height, const int cst_from, const int cst_to,
                             int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
  {
    printf("[_transform_lcms2] transfoming from RGB to Lab (%s %s)\n", self->op, self->multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, 1);
  }
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
  {
    printf("[_transform_lcms2] transfoming from Lab to RGB (%s %s)\n", self->op, self->multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, -1);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_lcms2] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

static inline void _transform_lcms2_rgb(const float *const image_in, float *const image_out, const int width,
                                        const int height,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  _transform_rgb_to_rgb_lcms2(image_in, image_out, width, height, profile_info_from->type,
                              profile_info_from->filename, profile_info_to->type, profile_info_to->filename,
                              profile_info_to->intent);
}


static inline int _init_unbounded_coeffs(float *const lutr, float *const lutg, float *const lutb,
    float *const unbounded_coeffsr, float *const unbounded_coeffsg, float *const unbounded_coeffsb, const int lutsize)
{
  int nonlinearlut = 0;
  float *lut[3] = { lutr, lutg, lutb };
  float *unbounded_coeffs[3] = { unbounded_coeffsr, unbounded_coeffsg, unbounded_coeffsb };

  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(lut[k][0] >= 0.0f)
    {
      const float x[4] DT_ALIGNED_PIXEL = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] DT_ALIGNED_PIXEL = { extrapolate_lut(lut[k], x[0], lutsize), extrapolate_lut(lut[k], x[1], lutsize), extrapolate_lut(lut[k], x[2], lutsize),
                                            extrapolate_lut(lut[k], x[3], lutsize) };
      dt_iop_estimate_exp(x, y, 4, unbounded_coeffs[k]);

      nonlinearlut++;
    }
    else
      unbounded_coeffs[k][0] = -1.0f;
  }

  return nonlinearlut;
}


static inline void _apply_tonecurves(const float *const image_in, float *const image_out,
                                     const int width, const int height,
                                     const float *const restrict lutr,
                                     const float *const restrict lutg,
                                     const float *const restrict lutb,
                                     const float *const restrict unbounded_coeffsr,
                                     const float *const restrict unbounded_coeffsg,
                                     const float *const restrict unbounded_coeffsb,
                                     const int lutsize)
{
  const int ch = 4;
  const float *const lut[3] = { lutr, lutg, lutb };
  const float *const unbounded_coeffs[3] = { unbounded_coeffsr, unbounded_coeffsg, unbounded_coeffsb };
  const size_t stride = (size_t)ch * width * height;

  // do we have any lut to apply, or is this a linear profile?
  if((lut[0][0] >= 0.0f) && (lut[1][0] >= 0.0f) && (lut[2][0] >= 0.0f))
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_in, image_out, lut, lutsize, unbounded_coeffs, ch) \
    schedule(static) collapse(2) aligned(image_in, image_out:64)
#endif
    for(size_t k = 0; k < stride; k += ch)
    {
      for(int c = 0; c < 3; c++)
      {
        image_out[k + c] = (image_in[k + c] < 1.0f) ? extrapolate_lut(lut[c], image_in[k + c], lutsize)
                                                    : eval_exp(unbounded_coeffs[c], image_in[k + c]);
      }
    }
  }
  else if((lut[0][0] >= 0.0f) || (lut[1][0] >= 0.0f) || (lut[2][0] >= 0.0f))
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_in, image_out, lut, lutsize, unbounded_coeffs, ch) \
    schedule(static) collapse(2) aligned(image_in, image_out:64)
#endif
    for(size_t k = 0; k < stride; k += ch)
    {
      for(int c = 0; c < 3; c++)
      {
        if(lut[c][0] >= 0.0f)
        {
          image_out[k + c] = (image_in[k + c] < 1.0f) ? extrapolate_lut(lut[c], image_in[k + c], lutsize)
                                                      : eval_exp(unbounded_coeffs[c], image_in[k + c]);
        }
      }
    }
  }
}


static inline void _transform_rgb_to_lab_matrix(const float *const restrict image_in, float *const restrict image_out,
                                                const int width, const int height,
                                                const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const float *const restrict matrix = profile_info->matrix_in;

  if(profile_info->nonlinearlut)
  {
    _apply_tonecurves(image_in, image_out, width, height, profile_info->lut_in[0], profile_info->lut_in[1],
                      profile_info->lut_in[2], profile_info->unbounded_coeffs_in[0],
                      profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2],
                      profile_info->lutsize);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(image_out, profile_info, stride, ch, matrix) \
    schedule(static) aligned(image_out:64) aligned(matrix:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      float *const in = image_out + y;
      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };
      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix);
      dt_XYZ_to_Lab(xyz, in);
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(image_in, image_out, profile_info, stride, ch, matrix) \
    schedule(static) aligned(image_in, image_out:64) aligned(matrix:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      const float *const in = image_in + y ;
      float *const out = image_out + y;

      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix);
      dt_XYZ_to_Lab(xyz, out);
    }
  }
}


static inline void _transform_lab_to_rgb_matrix(const float *const restrict image_in, float *const restrict image_out, const int width,
                                         const int height,
                                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const float *const restrict matrix = profile_info->matrix_out;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(image_in, image_out, stride, profile_info, ch, matrix) \
  schedule(static) aligned(image_in, image_out:64) aligned(matrix:16)
#endif
  for(size_t y = 0; y < stride; y += ch)
  {
    const float *const in = image_in + y;
    float *const out = image_out + y;

    float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

    dt_Lab_to_XYZ(in, xyz);
    _ioppr_xyz_to_linear_rgb_matrix(xyz, out, matrix);
  }

  if(profile_info->nonlinearlut)
  {
    _apply_tonecurves(image_out, image_out, width, height, profile_info->lut_out[0], profile_info->lut_out[1],
                      profile_info->lut_out[2], profile_info->unbounded_coeffs_out[0],
                      profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2],
                      profile_info->lutsize);
  }
}


static inline void _transform_matrix_rgb(const float *const restrict image_in, float *const restrict image_out, const int width,
                                  const int height, const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                  const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const float *const restrict matrix_in = profile_info_from->matrix_in;
  const float *const restrict matrix_out = profile_info_to->matrix_out;

  if(profile_info_from->nonlinearlut)
  {
    _apply_tonecurves(image_in, image_out, width, height, profile_info_from->lut_in[0],
                      profile_info_from->lut_in[1], profile_info_from->lut_in[2],
                      profile_info_from->unbounded_coeffs_in[0], profile_info_from->unbounded_coeffs_in[1],
                      profile_info_from->unbounded_coeffs_in[2], profile_info_from->lutsize);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_out, profile_info_from, profile_info_to, ch, matrix_in, matrix_out) \
    schedule(static) aligned(image_out:64) aligned(matrix_in, matrix_out:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      float *const in = image_out + y;

      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix_in);
      _ioppr_xyz_to_linear_rgb_matrix(xyz, in, matrix_out);
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(stride, image_in, image_out, profile_info_from, profile_info_to, ch, matrix_in, matrix_out) \
    schedule(static) aligned(image_in, image_out:64) aligned(matrix_in, matrix_out:16)
#endif
    for(size_t y = 0; y < stride; y += ch)
    {
      const float *const in = image_in + y;
      float *const out = image_out + y;

      float xyz[3] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f };

      _ioppr_linear_rgb_matrix_to_xyz(in, xyz, matrix_in);
      _ioppr_xyz_to_linear_rgb_matrix(xyz, out, matrix_out);
    }
  }

  if(profile_info_to->nonlinearlut)
  {
    _apply_tonecurves(image_out, image_out, width, height, profile_info_to->lut_out[0], profile_info_to->lut_out[1],
                      profile_info_to->lut_out[2], profile_info_to->unbounded_coeffs_out[0],
                      profile_info_to->unbounded_coeffs_out[1], profile_info_to->unbounded_coeffs_out[2],
                      profile_info_to->lutsize);
  }
}


static inline void _transform_matrix(struct dt_iop_module_t *self, const float *const restrict image_in, float *const restrict image_out,
                              const int width, const int height, const int cst_from, const int cst_to,
                              int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
  {
    _transform_rgb_to_lab_matrix(image_in, image_out, width, height, profile_info);
  }
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
  {
    _transform_lab_to_rgb_matrix(image_in, image_out, width, height, profile_info);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_matrix] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}


#define DT_IOPPR_LUT_SAMPLES 0x10000

void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int lutsize)
{
  profile_info->type = DT_COLORSPACE_NONE;
  profile_info->filename[0] = '\0';
  profile_info->intent = DT_INTENT_PERCEPTUAL;
  profile_info->matrix_in[0] = NAN;
  profile_info->matrix_out[0] = NAN;
  profile_info->unbounded_coeffs_in[0][0] = profile_info->unbounded_coeffs_in[1][0] = profile_info->unbounded_coeffs_in[2][0] = -1.0f;
  profile_info->unbounded_coeffs_out[0][0] = profile_info->unbounded_coeffs_out[1][0] = profile_info->unbounded_coeffs_out[2][0] = -1.0f;
  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.f;
  profile_info->lutsize = (lutsize > 0) ? lutsize: DT_IOPPR_LUT_SAMPLES;
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i] = dt_alloc_sse_ps(profile_info->lutsize);
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i] = dt_alloc_sse_ps(profile_info->lutsize);
    profile_info->lut_out[i][0] = -1.0f;
  }
}

#undef DT_IOPPR_LUT_SAMPLES

void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t *profile_info)
{
  for(int i = 0; i < 3; i++)
  {
    if(profile_info->lut_in[i]) dt_free_align(profile_info->lut_in[i]);
    if(profile_info->lut_out[i]) dt_free_align(profile_info->lut_out[i]);
  }
}

/** generate the info for the profile (type, filename) if matrix can be retrieved from lcms2
 * it can be called multiple time between init and cleanup
 * return 0 if OK, non zero otherwise
 */
static int dt_ioppr_generate_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int type, const char *filename, const int intent)
{
  int err_code = 0;
  cmsHPROFILE *rgb_profile = NULL;

  profile_info->matrix_in[0] = NAN;
  profile_info->matrix_out[0] = NAN;
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i][0] = -1.0f;
  }

  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.1842f;

  profile_info->type = type;
  g_strlcpy(profile_info->filename, filename, sizeof(profile_info->filename));
  profile_info->intent = intent;

  if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *profile
      = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_ANY);
  if(profile) rgb_profile = profile->profile;

  if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  // we only allow rgb profiles
  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space>>24),
              (char)(rgb_color_space>>16),
              (char)(rgb_color_space>>8),
              (char)(rgb_color_space));
      rgb_profile = NULL;
    }
  }

  // get the matrix
  if(rgb_profile)
  {
    if(dt_colorspaces_get_matrix_from_input_profile(rgb_profile, profile_info->matrix_in,
        profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
        profile_info->lutsize, profile_info->intent) ||
        dt_colorspaces_get_matrix_from_output_profile(rgb_profile, profile_info->matrix_out,
            profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
            profile_info->lutsize, profile_info->intent))
    {
      profile_info->matrix_in[0] = NAN;
      profile_info->matrix_out[0] = NAN;
      for(int i = 0; i < 3; i++)
      {
        profile_info->lut_in[i][0] = -1.0f;
        profile_info->lut_out[i][0] = -1.0f;
      }
    }
    else if(isnan(profile_info->matrix_in[0]) || isnan(profile_info->matrix_out[0]))
    {
      profile_info->matrix_in[0] = NAN;
      profile_info->matrix_out[0] = NAN;
      for(int i = 0; i < 3; i++)
      {
        profile_info->lut_in[i][0] = -1.0f;
        profile_info->lut_out[i][0] = -1.0f;
      }
    }
  }

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]))
  {
    profile_info->nonlinearlut = _init_unbounded_coeffs(profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
        profile_info->unbounded_coeffs_in[0], profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2], profile_info->lutsize);
    _init_unbounded_coeffs(profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
        profile_info->unbounded_coeffs_out[0], profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2], profile_info->lutsize);
  }

  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]) && profile_info->nonlinearlut)
  {
    const float rgb[3] = { 0.1842f, 0.1842f, 0.1842f };
    profile_info->grey = dt_ioppr_get_rgb_matrix_luminance(rgb, profile_info->matrix_in, profile_info->lut_in, profile_info->unbounded_coeffs_in, profile_info->lutsize, profile_info->nonlinearlut);
  }

  return err_code;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename)
{
  dt_iop_order_iccprofile_info_t *profile_info = NULL;

  GList *profiles = g_list_first(dev->allprofile_info);
  while(profiles)
  {
    dt_iop_order_iccprofile_info_t *prof = (dt_iop_order_iccprofile_info_t *)(profiles->data);
    if(prof->type == profile_type && strcmp(prof->filename, profile_filename) == 0)
    {
      profile_info = prof;
      break;
    }
    profiles = g_list_next(profiles);
  }

  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev, const int profile_type, const char *profile_filename, const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_get_profile_info_from_list(dev, profile_type, profile_filename);
  if(profile_info == NULL)
  {
    profile_info = malloc(sizeof(dt_iop_order_iccprofile_info_t));
    dt_ioppr_init_profile_info(profile_info, 0);
    const int err = dt_ioppr_generate_profile_info(profile_info, profile_type, profile_filename, intent);
    if(err == 0)
    {
      dev->allprofile_info = g_list_append(dev->allprofile_info, profile_info);
    }
    else
    {
      free(profile_info);
      profile_info = NULL;
    }
  }
  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module, GList *iop_list)
{
  dt_iop_order_iccprofile_info_t *profile = NULL;

  // first check if the module is between colorin and colorout
  gboolean in_between = FALSE;

  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    // we reach the module, that's it
    if(strcmp(mod->op, module->op) == 0) break;

    // if we reach colorout means that the module is after it
    if(strcmp(mod->op, "colorout") == 0)
    {
      in_between = FALSE;
      break;
    }

    // we reach colorin, so far we're good
    if(strcmp(mod->op, "colorin") == 0)
    {
      in_between = TRUE;
      break;
    }

    modules = g_list_next(modules);
  }

  if(in_between)
  {
    dt_colorspaces_color_profile_type_t type = DT_COLORSPACE_NONE;
    char *filename = NULL;
    dt_develop_t *dev = module->dev;

    dt_ioppr_get_work_profile_type(dev, &type, &filename);
    if(filename) profile = dt_ioppr_add_profile_info_to_list(dev, type, filename, DT_INTENT_PERCEPTUAL);
  }

  return profile;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
    const int type, const char *filename, const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_add_profile_info_to_list(dev, type, filename, intent);

  if(profile_info == NULL || isnan(profile_info->matrix_in[0]) || isnan(profile_info->matrix_out[0]))
  {
    fprintf(stderr, "[dt_ioppr_set_pipe_work_profile_info] unsupported working profile %i %s, it will be replaced with linear rec2020\n", type, filename);
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", intent);
  }
  pipe->dsc.work_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_histogram_profile_info(struct dt_develop_t *dev)
{
  dt_colorspaces_color_profile_type_t histogram_profile_type;
  char *histogram_profile_filename;
  dt_ioppr_get_histogram_profile_type(&histogram_profile_type, &histogram_profile_filename);
  return dt_ioppr_add_profile_info_to_list(dev, histogram_profile_type, histogram_profile_filename,
                                           DT_INTENT_PERCEPTUAL);
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->dsc.work_profile_info;
}

// returns a pointer to the filename of the work profile instead of the actual string data
// pointer must not be stored
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev, int *profile_type, char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorin_so = NULL;
  dt_iop_module_t *colorin = NULL;
  GList *modules = g_list_first(darktable.iop);
  while(modules)
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorin"))
    {
      colorin_so = module_so;
      break;
    }
    modules = g_list_next(modules);
  }
  if(colorin_so && colorin_so->get_p)
  {
    modules = g_list_first(dev->iop);
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorin"))
      {
        colorin = module;
        break;
      }
      modules = g_list_next(modules);
    }
  }
  if(colorin)
  {
    dt_colorspaces_color_profile_type_t *_type = colorin_so->get_p(colorin->params, "type_work");
    char *_filename = colorin_so->get_p(colorin->params, "filename_work");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't get colorin parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't find colorin iop\n");
}

void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev, int *profile_type, char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorout_so = NULL;
  dt_iop_module_t *colorout = NULL;
  GList *modules = g_list_last(darktable.iop);
  while(modules)
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorout"))
    {
      colorout_so = module_so;
      break;
    }
    modules = g_list_previous(modules);
  }
  if(colorout_so && colorout_so->get_p)
  {
    modules = g_list_last(dev->iop);
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorout"))
      {
        colorout = module;
        break;
      }
      modules = g_list_previous(modules);
    }
  }
  if(colorout)
  {
    dt_colorspaces_color_profile_type_t *_type = colorout_so->get_p(colorout->params, "type");
    char *_filename = colorout_so->get_p(colorout->params, "filename");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't get colorout parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't find colorout iop\n");
}

void dt_ioppr_get_histogram_profile_type(int *profile_type, char **profile_filename)
{
  const dt_colorspaces_color_mode_t mode = darktable.color_profiles->mode;

  // if in gamut check use soft proof
  if(mode != DT_PROFILE_NORMAL || darktable.color_profiles->histogram_type == DT_COLORSPACE_SOFTPROOF)
  {
    *profile_type = darktable.color_profiles->softproof_type;
    *profile_filename = darktable.color_profiles->softproof_filename;
  }
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_WORK)
  {
    dt_ioppr_get_work_profile_type(darktable.develop, profile_type, profile_filename);
  }
  else if(darktable.color_profiles->histogram_type == DT_COLORSPACE_EXPORT)
  {
    dt_ioppr_get_export_profile_type(darktable.develop, profile_type, profile_filename);
  }
  else
  {
    *profile_type = darktable.color_profiles->histogram_type;
    *profile_filename = darktable.color_profiles->histogram_filename;
  }
}


#if defined(__SSE2__x) // FIXME: this is slower than the C version
static __m128 _ioppr_linear_rgb_matrix_to_xyz_sse(const __m128 rgb, const dt_iop_order_iccprofile_info_t *const profile_info)
{
/*  for(int c = 0; c < 3; c++)
  {
    xyz[c] = 0.0f;
    for(int i = 0; i < 3; i++)
    {
      xyz[c] += profile_info->matrix_in[3 * c + i] * rgb[i];
    }
  }*/
  const __m128 m0 = _mm_set_ps(0.0f, profile_info->matrix_in[6], profile_info->matrix_in[3], profile_info->matrix_in[0]);
  const __m128 m1 = _mm_set_ps(0.0f, profile_info->matrix_in[7], profile_info->matrix_in[4], profile_info->matrix_in[1]);
  const __m128 m2 = _mm_set_ps(0.0f, profile_info->matrix_in[8], profile_info->matrix_in[5], profile_info->matrix_in[2]);

  __m128 xyz
      = _mm_add_ps(_mm_mul_ps(m0, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0))),
                   _mm_add_ps(_mm_mul_ps(m1, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1))),
                              _mm_mul_ps(m2, _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2)))));
  return xyz;
}

static __m128 _ioppr_xyz_to_linear_rgb_matrix_sse(const __m128 xyz, const dt_iop_order_iccprofile_info_t *const profile_info)
{
/*  for(int c = 0; c < 3; c++)
  {
    rgb[c] = 0.0f;
    for(int i = 0; i < 3; i++)
    {
      rgb[c] += profile_info->matrix_out[3 * c + i] * xyz[i];
    }
  }*/
  const __m128 m0 = _mm_set_ps(0.0f, profile_info->matrix_out[6], profile_info->matrix_out[3], profile_info->matrix_out[0]);
  const __m128 m1 = _mm_set_ps(0.0f, profile_info->matrix_out[7], profile_info->matrix_out[4], profile_info->matrix_out[1]);
  const __m128 m2 = _mm_set_ps(0.0f, profile_info->matrix_out[8], profile_info->matrix_out[5], profile_info->matrix_out[2]);

  __m128 rgb
      = _mm_add_ps(_mm_mul_ps(m0, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(0, 0, 0, 0))),
                   _mm_add_ps(_mm_mul_ps(m1, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(1, 1, 1, 1))),
                              _mm_mul_ps(m2, _mm_shuffle_ps(xyz, xyz, _MM_SHUFFLE(2, 2, 2, 2)))));
  return rgb;
}

static void _transform_rgb_to_lab_matrix_sse(float *const image, const int width, const int height, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height;

  _apply_tonecurves(image, width, height, profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
      profile_info->unbounded_coeffs_in[0], profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2], profile_info->lutsize);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t y = 0; y < stride; y++)
  {
    float *const in = image + y * ch;

    __m128 xyz = { 0.0f };
    __m128 rgb = _mm_load_ps(in);

    xyz = _ioppr_linear_rgb_matrix_to_xyz_sse(rgb, profile_info);

    rgb = dt_XYZ_to_Lab_sse2(xyz);
    const float a = in[3];
    _mm_stream_ps(in, rgb);
    in[3] = a;
  }
}

static void _transform_lab_to_rgb_matrix_sse(float *const image, const int width, const int height, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t y = 0; y < stride; y++)
  {
    float *const in = image + y * ch;

    __m128 xyz = { 0.0f };
    __m128 lab = _mm_load_ps(in);

    xyz = dt_Lab_to_XYZ_sse2(lab);
    lab = _ioppr_xyz_to_linear_rgb_matrix_sse(xyz, profile_info);
    const float a = in[3];
    _mm_stream_ps(in, lab);
    in[3] = a;
  }

  _apply_tonecurves(image, width, height, profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
      profile_info->unbounded_coeffs_out[0], profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2], profile_info->lutsize);
}

// FIXME: this is slower than the C version
static void _transform_matrix_sse(struct dt_iop_module_t *self, float *const image, const int width, const int height,
    const int cst_from, const int cst_to, int *converted_cst, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
  {
    _transform_rgb_to_lab_matrix_sse(image, width, height, profile_info);
  }
  else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
  {
    _transform_lab_to_rgb_matrix_sse(image, width, height, profile_info);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[_transform_matrix_sse] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

static void _transform_matrix_rgb_sse(float *const image, const int width, const int height,
                                      const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                      const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height;

  _apply_tonecurves(image, width, height, profile_info_from->lut_in[0], profile_info_from->lut_in[1],
                    profile_info_from->lut_in[2], profile_info_from->unbounded_coeffs_in[0],
                    profile_info_from->unbounded_coeffs_in[1], profile_info_from->unbounded_coeffs_in[2],
                    profile_info_from->lutsize);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t y = 0; y < stride; y++)
  {
    float *const in = image + y * ch;

    __m128 xyz = { 0.0f };
    __m128 rgb = _mm_load_ps(in);

    xyz = _ioppr_linear_rgb_matrix_to_xyz_sse(rgb, profile_info_from);
    rgb = _ioppr_xyz_to_linear_rgb_matrix_sse(xyz, profile_info_to);

    const float a = in[3];
    _mm_stream_ps(in, rgb);
    in[3] = a;
  }

  _apply_tonecurves(image, width, height, profile_info_to->lut_out[0], profile_info_to->lut_out[1],
                    profile_info_to->lut_out[2], profile_info_to->unbounded_coeffs_out[0],
                    profile_info_to->unbounded_coeffs_out[1], profile_info_to->unbounded_coeffs_out[2],
                    profile_info_to->lutsize);
}
#endif

void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self, const float *const image_in,
                                         float *const image_out, const int width, const int height,
                                         const int cst_from, const int cst_to, int *converted_cst,
                                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }
  if(profile_info == NULL)
  {
    *converted_cst = cst_from;
    return;
  }
  if(profile_info->type == DT_COLORSPACE_NONE)
  {
    *converted_cst = cst_from;
    return;
  }

  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  // matrix should be never NAN, this is only to test it against lcms2!
  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]))
  {
    // FIXME: sse is slower than the C version
    // if(darktable.codepath.OPENMP_SIMD)
    _transform_matrix(self, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);
    /*
    #if defined(__SSE2__)
        else if(darktable.codepath.SSE2)
          _transform_matrix_sse(self, image, width, height, cst_from, cst_to, converted_cst, profile_info);
    #endif
        else
          dt_unreachable_codepath();
    */
    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f CPU) [%s %s]\n",
          (cst_from == iop_cs_rgb) ? "RGB": "Lab", (cst_to == iop_cs_rgb) ? "RGB": "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }
  else
  {
    _transform_lcms2(self, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f lcms2) [%s %s]\n",
          (cst_from == iop_cs_rgb) ? "RGB": "Lab", (cst_to == iop_cs_rgb) ? "RGB": "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }

  if(*converted_cst == cst_from)
    fprintf(stderr, "[dt_ioppr_transform_image_colorspace] invalid conversion from %i to %i\n", cst_from, cst_to);
}

void dt_ioppr_transform_image_colorspace_rgb(const float *const restrict image_in, float *const restrict image_out, const int width,
                                             const int height,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                             const char *message)
{
  if(profile_info_from->type == DT_COLORSPACE_NONE || profile_info_to->type == DT_COLORSPACE_NONE)
  {
    return;
  }
  if(profile_info_from->type == profile_info_to->type
     && strcmp(profile_info_from->filename, profile_info_to->filename) == 0)
  {
    if(image_in != image_out)
      memcpy(image_out, image_in, width * height * 4 * sizeof(float));

    return;
  }

  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  if(!isnan(profile_info_from->matrix_in[0]) && !isnan(profile_info_from->matrix_out[0])
     && !isnan(profile_info_to->matrix_in[0]) && !isnan(profile_info_to->matrix_out[0]))
  {
    // FIXME: sse is slower than the C version
    // if(darktable.codepath.OPENMP_SIMD)
    _transform_matrix_rgb(image_in, image_out, width, height, profile_info_from, profile_info_to);
    /*
    #if defined(__SSE2__)
        else if(darktable.codepath.SSE2)
          _transform_matrix_rgb_sse(self, image, width, height, profile_info_from, profile_info_to);
    #endif
        else
          dt_unreachable_codepath();
    */
    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f CPU) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
  else
  {
    _transform_lcms2_rgb(image_in, image_out, width, height, profile_info_from, profile_info_to);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f lcms2) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
}

#ifdef HAVE_OPENCL
dt_colorspaces_cl_global_t *dt_colorspaces_init_cl_global()
{
  dt_colorspaces_cl_global_t *g = (dt_colorspaces_cl_global_t *)malloc(sizeof(dt_colorspaces_cl_global_t));

  const int program = 23; // colorspaces.cl, from programs.conf
  g->kernel_colorspaces_transform_lab_to_rgb_matrix = dt_opencl_create_kernel(program, "colorspaces_transform_lab_to_rgb_matrix");
  g->kernel_colorspaces_transform_rgb_matrix_to_lab = dt_opencl_create_kernel(program, "colorspaces_transform_rgb_matrix_to_lab");
  g->kernel_colorspaces_transform_rgb_matrix_to_rgb
      = dt_opencl_create_kernel(program, "colorspaces_transform_rgb_matrix_to_rgb");
  return g;
}

void dt_colorspaces_free_cl_global(dt_colorspaces_cl_global_t *g)
{
  if(!g) return;

  // destroy kernels
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_lab_to_rgb_matrix);
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_rgb_matrix_to_lab);
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_rgb_matrix_to_rgb);

  free(g);
}

void dt_ioppr_get_profile_info_cl(const dt_iop_order_iccprofile_info_t *const profile_info, dt_colorspaces_iccprofile_info_cl_t *profile_info_cl)
{
  for(int i = 0; i < 9; i++)
  {
    profile_info_cl->matrix_in[i] = profile_info->matrix_in[i];
    profile_info_cl->matrix_out[i] = profile_info->matrix_out[i];
  }
  profile_info_cl->lutsize = profile_info->lutsize;
  for(int i = 0; i < 3; i++)
  {
    for(int j = 0; j < 3; j++)
    {
      profile_info_cl->unbounded_coeffs_in[i][j] = profile_info->unbounded_coeffs_in[i][j];
      profile_info_cl->unbounded_coeffs_out[i][j] = profile_info->unbounded_coeffs_out[i][j];
    }
  }
  profile_info_cl->nonlinearlut = profile_info->nonlinearlut;
  profile_info_cl->grey = profile_info->grey;
}

cl_float *dt_ioppr_get_trc_cl(const dt_iop_order_iccprofile_info_t *const profile_info)
{
  cl_float *trc = malloc(profile_info->lutsize * 6 * sizeof(cl_float));
  if(trc)
  {
    int x = 0;
    for(int c = 0; c < 3; c++)
      for(int y = 0; y < profile_info->lutsize; y++, x++)
        trc[x] = profile_info->lut_in[c][y];
    for(int c = 0; c < 3; c++)
      for(int y = 0; y < profile_info->lutsize; y++, x++)
        trc[x] = profile_info->lut_out[c][y];
  }
  return trc;
}

cl_int dt_ioppr_build_iccprofile_params_cl(const dt_iop_order_iccprofile_info_t *const profile_info,
                                           const int devid, dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                           cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                           cl_mem *_dev_profile_lut)
{
  cl_int err = CL_SUCCESS;

  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = calloc(1, sizeof(dt_colorspaces_iccprofile_info_cl_t));
  cl_float *profile_lut_cl = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;

  if(profile_info)
  {
    dt_ioppr_get_profile_info_cl(profile_info, profile_info_cl);
    profile_lut_cl = dt_ioppr_get_trc_cl(profile_info);

    dev_profile_info = dt_opencl_copy_host_to_device_constant(devid, sizeof(*profile_info_cl), profile_info_cl);
    if(dev_profile_info == NULL)
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    dev_profile_lut = dt_opencl_copy_host_to_device(devid, profile_lut_cl, 256, 256 * 6, sizeof(float));
    if(dev_profile_lut == NULL)
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
  }
  else
  {
    profile_lut_cl = malloc(1 * 6 * sizeof(cl_float));

    dev_profile_lut = dt_opencl_copy_host_to_device(devid, profile_lut_cl, 1, 1 * 6, sizeof(float));
    if(dev_profile_lut == NULL)
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 7\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
  }

cleanup:
  *_profile_info_cl = profile_info_cl;
  *_profile_lut_cl = profile_lut_cl;
  *_dev_profile_info = dev_profile_info;
  *_dev_profile_lut = dev_profile_lut;

  return err;
}

void dt_ioppr_free_iccprofile_params_cl(dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                        cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                        cl_mem *_dev_profile_lut)
{
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = *_profile_info_cl;
  cl_float *profile_lut_cl = *_profile_lut_cl;
  cl_mem dev_profile_info = *_dev_profile_info;
  cl_mem dev_profile_lut = *_dev_profile_lut;

  if(profile_info_cl) free(profile_info_cl);
  if(dev_profile_info) dt_opencl_release_mem_object(dev_profile_info);
  if(dev_profile_lut) dt_opencl_release_mem_object(dev_profile_lut);
  if(profile_lut_cl) free(profile_lut_cl);

  *_profile_info_cl = NULL;
  *_profile_lut_cl = NULL;
  *_dev_profile_info = NULL;
  *_dev_profile_lut = NULL;
}

int dt_ioppr_transform_image_colorspace_cl(struct dt_iop_module_t *self, const int devid, cl_mem dev_img_in,
                                           cl_mem dev_img_out, const int width, const int height,
                                           const int cst_from, const int cst_to, int *converted_cst,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  cl_int err = CL_SUCCESS;

  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return TRUE;
  }
  if(profile_info == NULL)
  {
    *converted_cst = cst_from;
    return FALSE;
  }
  if(profile_info->type == DT_COLORSPACE_NONE)
  {
    *converted_cst = cst_from;
    return FALSE;
  }

  const int ch = 4;
  float *src_buffer = NULL;
  int in_place = (dev_img_in == dev_img_out);

  int kernel_transform = 0;
  cl_mem dev_tmp = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_cl;
  cl_float *lut_cl = NULL;

  *converted_cst = cst_from;

  // if we have a matrix use opencl
  if(!isnan(profile_info->matrix_in[0]) && !isnan(profile_info->matrix_out[0]))
  {
    dt_times_t start_time = { 0 }, end_time = { 0 };
    if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };

    if(cst_from == iop_cs_rgb && cst_to == iop_cs_Lab)
    {
      kernel_transform = darktable.opencl->colorspaces->kernel_colorspaces_transform_rgb_matrix_to_lab;
    }
    else if(cst_from == iop_cs_Lab && cst_to == iop_cs_rgb)
    {
      kernel_transform = darktable.opencl->colorspaces->kernel_colorspaces_transform_lab_to_rgb_matrix;
    }
    else
    {
      err = CL_INVALID_KERNEL;
      *converted_cst = cst_from;
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] invalid conversion from %i to %i\n", cst_from, cst_to);
      goto cleanup;
    }

    dt_ioppr_get_profile_info_cl(profile_info, &profile_info_cl);
    lut_cl = dt_ioppr_get_trc_cl(profile_info);

    if(in_place)
    {
      dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
      if(dev_tmp == NULL)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 4\n");
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        goto cleanup;
      }

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_tmp, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error on copy image for color transformation\n");
        goto cleanup;
      }
    }
    else
    {
      dev_tmp = dev_img_in;
    }

    dev_profile_info = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_cl), &profile_info_cl);
    if(dev_profile_info == NULL)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut = dt_opencl_copy_host_to_device(devid, lut_cl, 256, 256 * 6, sizeof(float));
    if(dev_lut == NULL)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel_transform, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 1, sizeof(cl_mem), (void *)&dev_img_out);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 4, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 5, sizeof(cl_mem), (void *)&dev_lut);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_transform, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error %i enqueue kernel for color transformation\n", err);
      goto cleanup;
    }

    *converted_cst = cst_to;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f GPU) [%s %s]\n",
          (cst_from == iop_cs_rgb) ? "RGB": "Lab", (cst_to == iop_cs_rgb) ? "RGB": "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }
  else
  {
    // no matrix, call lcms2
    src_buffer = dt_alloc_align(64, width * height * ch * sizeof(float));
    if(src_buffer == NULL)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 1\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    err = dt_opencl_copy_device_to_host(devid, src_buffer, dev_img_in, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 2\n");
      goto cleanup;
    }

    // just call the CPU version for now
    dt_ioppr_transform_image_colorspace(self, src_buffer, src_buffer, width, height, cst_from, cst_to,
                                        converted_cst, profile_info);

    err = dt_opencl_write_host_to_device(devid, src_buffer, dev_img_out, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 3\n");
      goto cleanup;
    }
  }

cleanup:
  if(src_buffer) dt_free_align(src_buffer);
  if(dev_tmp && in_place) dt_opencl_release_mem_object(dev_tmp);
  if(dev_profile_info) dt_opencl_release_mem_object(dev_profile_info);
  if(dev_lut) dt_opencl_release_mem_object(dev_lut);
  if(lut_cl) free(lut_cl);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}

int dt_ioppr_transform_image_colorspace_rgb_cl(const int devid, cl_mem dev_img_in, cl_mem dev_img_out,
                                               const int width, const int height,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                               const char *message)
{
  cl_int err = CL_SUCCESS;

  if(profile_info_from->type == DT_COLORSPACE_NONE || profile_info_to->type == DT_COLORSPACE_NONE)
  {
    return FALSE;
  }
  if(profile_info_from->type == profile_info_to->type
     && strcmp(profile_info_from->filename, profile_info_to->filename) == 0)
  {
    if(dev_img_in != dev_img_out)
    {
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_img_out, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_rgb_cl] error on copy image for color transformation\n");
        return FALSE;
      }
    }

    return TRUE;
  }

  const int ch = 4;
  float *src_buffer_in = NULL;
  float *src_buffer_out = NULL;
  int in_place = (dev_img_in == dev_img_out);

  int kernel_transform = 0;
  cl_mem dev_tmp = NULL;

  cl_mem dev_profile_info_from = NULL;
  cl_mem dev_lut_from = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_from_cl;
  cl_float *lut_from_cl = NULL;

  cl_mem dev_profile_info_to = NULL;
  cl_mem dev_lut_to = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_to_cl;
  cl_float *lut_to_cl = NULL;

  // if we have a matrix use opencl
  if(!isnan(profile_info_from->matrix_in[0]) && !isnan(profile_info_from->matrix_out[0])
     && !isnan(profile_info_to->matrix_in[0]) && !isnan(profile_info_to->matrix_out[0]))
  {
    dt_times_t start_time = { 0 }, end_time = { 0 };
    if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };

    kernel_transform = darktable.opencl->colorspaces->kernel_colorspaces_transform_rgb_matrix_to_rgb;

    dt_ioppr_get_profile_info_cl(profile_info_from, &profile_info_from_cl);
    lut_from_cl = dt_ioppr_get_trc_cl(profile_info_from);

    dt_ioppr_get_profile_info_cl(profile_info_to, &profile_info_to_cl);
    lut_to_cl = dt_ioppr_get_trc_cl(profile_info_to);

    if(in_place)
    {
      dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
      if(dev_tmp == NULL)
      {
        fprintf(
            stderr,
            "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 4\n");
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        goto cleanup;
      }

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_tmp, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_rgb_cl] error on copy image for color transformation\n");
        goto cleanup;
      }
    }
    else
    {
      dev_tmp = dev_img_in;
    }

    dev_profile_info_from
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_from_cl), &profile_info_from_cl);
    if(dev_profile_info_from == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut_from = dt_opencl_copy_host_to_device(devid, lut_from_cl, 256, 256 * 6, sizeof(float));
    if(dev_lut_from == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    dev_profile_info_to
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_to_cl), &profile_info_to_cl);
    if(dev_profile_info_to == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 7\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut_to = dt_opencl_copy_host_to_device(devid, lut_to_cl, 256, 256 * 6, sizeof(float));
    if(dev_lut_to == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 8\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

    dt_opencl_set_kernel_arg(devid, kernel_transform, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 1, sizeof(cl_mem), (void *)&dev_img_out);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 4, sizeof(cl_mem), (void *)&dev_profile_info_from);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 5, sizeof(cl_mem), (void *)&dev_lut_from);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 6, sizeof(cl_mem), (void *)&dev_profile_info_to);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 7, sizeof(cl_mem), (void *)&dev_lut_to);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_transform, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error %i enqueue kernel for color transformation\n",
              err);
      goto cleanup;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f GPU) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
  else
  {
    // no matrix, call lcms2
    src_buffer_in = dt_alloc_align(64, width * height * ch * sizeof(float));
    src_buffer_out = dt_alloc_align(64, width * height * ch * sizeof(float));
    if(src_buffer_in == NULL || src_buffer_out == NULL)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 1\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    err = dt_opencl_copy_device_to_host(devid, src_buffer_in, dev_img_in, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 2\n");
      goto cleanup;
    }

    // just call the CPU version for now
    dt_ioppr_transform_image_colorspace_rgb(src_buffer_in, src_buffer_out, width, height, profile_info_from,
                                            profile_info_to, message);

    err = dt_opencl_write_host_to_device(devid, src_buffer_out, dev_img_out, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 3\n");
      goto cleanup;
    }
  }

cleanup:
  if(src_buffer_in) dt_free_align(src_buffer_in);
  if(src_buffer_out) dt_free_align(src_buffer_out);
  if(dev_tmp && in_place) dt_opencl_release_mem_object(dev_tmp);

  if(dev_profile_info_from) dt_opencl_release_mem_object(dev_profile_info_from);
  if(dev_lut_from) dt_opencl_release_mem_object(dev_lut_from);
  if(lut_from_cl) free(lut_from_cl);

  if(dev_profile_info_to) dt_opencl_release_mem_object(dev_profile_info_to);
  if(dev_lut_to) dt_opencl_release_mem_object(dev_lut_to);
  if(lut_to_cl) free(lut_to_cl);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

#undef DT_IOP_ORDER_PROFILE