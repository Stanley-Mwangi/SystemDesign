/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (C) 2013-2015 Tampere University of Technology and others (see
 * COPYING file).
 *
 * Kvazaar is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

/*
 * \file
 */

#include <stdlib.h>

#include "quant-generic.h"
#include "strategyselector.h"
#include "encoder.h"
#include "transform.h"
#include "rdo.h"

#define QUANT_SHIFT 14
/**
* \brief quantize transformed coefficents
*
*/
void kvz_quant_generic(const encoder_state_t * const state, coeff_t *coef, coeff_t *q_coef, int32_t width,
  int32_t height, int8_t type, int8_t scan_idx, int8_t block_type)
{
  const encoder_control_t * const encoder = state->encoder_control;
  const uint32_t log2_block_size = kvz_g_convert_to_bit[width] + 2;
  const uint32_t * const scan = kvz_g_sig_last_scan[scan_idx][log2_block_size - 1];

  int32_t qp_scaled = kvz_get_scaled_qp(type, state->global->QP, (encoder->bitdepth - 8) * 6);
  const uint32_t log2_tr_size = kvz_g_convert_to_bit[width] + 2;
  const int32_t scalinglist_type = (block_type == CU_INTRA ? 0 : 3) + (int8_t)("\0\3\1\2"[type]);
  const int32_t *quant_coeff = encoder->scaling_list.quant_coeff[log2_tr_size - 2][scalinglist_type][qp_scaled % 6];
  const int32_t transform_shift = MAX_TR_DYNAMIC_RANGE - encoder->bitdepth - log2_tr_size; //!< Represents scaling through forward transform
  const int32_t q_bits = QUANT_SHIFT + qp_scaled / 6 + transform_shift;
  const int32_t add = ((state->global->slicetype == KVZ_SLICE_I) ? 171 : 85) << (q_bits - 9);
  const int32_t q_bits8 = q_bits - 8;

  uint32_t ac_sum = 0;

  for (int32_t n = 0; n < width * height; n++) {
    int32_t level;
    int32_t  sign;

    level = coef[n];
    sign = (level < 0 ? -1 : 1);

    level = ((int64_t)abs(level) * quant_coeff[n] + add) >> q_bits;
    ac_sum += level;

    level *= sign;
    q_coef[n] = (coeff_t)(CLIP(-32768, 32767, level));
  }

  if (!(encoder->sign_hiding && ac_sum >= 2)) return;

  int32_t delta_u[LCU_WIDTH*LCU_WIDTH >> 2];

  for (int32_t n = 0; n < width * height; n++) {
    int32_t level;
    level = coef[n];
    level = ((int64_t)abs(level) * quant_coeff[n] + add) >> q_bits;
    delta_u[n] = (int32_t)(((int64_t)abs(coef[n]) * quant_coeff[n] - (level << q_bits)) >> q_bits8);
  }

  if (ac_sum >= 2) {
#define SCAN_SET_SIZE 16
#define LOG2_SCAN_SET_SIZE 4
    int32_t n, last_cg = -1, abssum = 0, subset, subpos;
    for (subset = (width*height - 1) >> LOG2_SCAN_SET_SIZE; subset >= 0; subset--) {
      int32_t first_nz_pos_in_cg = SCAN_SET_SIZE, last_nz_pos_in_cg = -1;
      subpos = subset << LOG2_SCAN_SET_SIZE;
      abssum = 0;

      // Find last coeff pos
      for (n = SCAN_SET_SIZE - 1; n >= 0; n--)  {
        if (q_coef[scan[n + subpos]])  {
          last_nz_pos_in_cg = n;
          break;
        }
      }

      // First coeff pos
      for (n = 0; n <SCAN_SET_SIZE; n++) {
        if (q_coef[scan[n + subpos]]) {
          first_nz_pos_in_cg = n;
          break;
        }
      }

      // Sum all kvz_quant coeffs between first and last
      for (n = first_nz_pos_in_cg; n <= last_nz_pos_in_cg; n++) {
        abssum += q_coef[scan[n + subpos]];
      }

      if (last_nz_pos_in_cg >= 0 && last_cg == -1) {
        last_cg = 1;
      }

      if (last_nz_pos_in_cg - first_nz_pos_in_cg >= 4) {
        int32_t signbit = (q_coef[scan[subpos + first_nz_pos_in_cg]] > 0 ? 0 : 1);
        if (signbit != (abssum & 0x1)) { // compare signbit with sum_parity
          int32_t min_cost_inc = 0x7fffffff, min_pos = -1, cur_cost = 0x7fffffff;
          int16_t final_change = 0, cur_change = 0;
          for (n = (last_cg == 1 ? last_nz_pos_in_cg : SCAN_SET_SIZE - 1); n >= 0; n--) {
            uint32_t blkPos = scan[n + subpos];
            if (q_coef[blkPos] != 0) {
              if (delta_u[blkPos] > 0) {
                cur_cost = -delta_u[blkPos];
                cur_change = 1;
              }
              else if (n == first_nz_pos_in_cg && abs(q_coef[blkPos]) == 1) {
                cur_cost = 0x7fffffff;
              }
              else {
                cur_cost = delta_u[blkPos];
                cur_change = -1;
              }
            }
            else if (n < first_nz_pos_in_cg && ((coef[blkPos] >= 0) ? 0 : 1) != signbit) {
              cur_cost = 0x7fffffff;
            }
            else {
              cur_cost = -delta_u[blkPos];
              cur_change = 1;
            }

            if (cur_cost < min_cost_inc) {
              min_cost_inc = cur_cost;
              final_change = cur_change;
              min_pos = blkPos;
            }
          } // CG loop

          if (q_coef[min_pos] == 32767 || q_coef[min_pos] == -32768) {
            final_change = -1;
          }

          if (coef[min_pos] >= 0) q_coef[min_pos] += final_change;
          else q_coef[min_pos] -= final_change;
        } // Hide
      }
      if (last_cg == 1) last_cg = 0;
    }

#undef SCAN_SET_SIZE
#undef LOG2_SCAN_SET_SIZE
  }
}

/**
* \brief Quantize residual and get both the reconstruction and coeffs.
*
* \param width  Transform width.
* \param color  Color.
* \param scan_order  Coefficient scan order.
* \param use_trskip  Whether transform skip is used.
* \param stride  Stride for ref_in, pred_in rec_out and coeff_out.
* \param ref_in  Reference pixels.
* \param pred_in  Predicted pixels.
* \param rec_out  Reconstructed pixels.
* \param coeff_out  Coefficients used for reconstruction of rec_out.
*
* \returns  Whether coeff_out contains any non-zero coefficients.
*/
int kvz_quantize_residual_generic(encoder_state_t *const state,
  const cu_info_t *const cur_cu, const int width, const color_t color,
  const coeff_scan_order_t scan_order, const int use_trskip,
  const int in_stride, const int out_stride,
  const kvz_pixel *const ref_in, const kvz_pixel *const pred_in,
  kvz_pixel *rec_out, coeff_t *coeff_out)
{
  // Temporary arrays to pass data to and from kvz_quant and transform functions.
  int16_t residual[TR_MAX_WIDTH * TR_MAX_WIDTH];
  coeff_t quant_coeff[TR_MAX_WIDTH * TR_MAX_WIDTH];
  coeff_t coeff[TR_MAX_WIDTH * TR_MAX_WIDTH];

  int has_coeffs = 0;

  assert(width <= TR_MAX_WIDTH);
  assert(width >= TR_MIN_WIDTH);

  // Get residual. (ref_in - pred_in -> residual)
  {
    int y, x;
    for (y = 0; y < width; ++y) {
      for (x = 0; x < width; ++x) {
        residual[x + y * width] = (int16_t)(ref_in[x + y * in_stride] - pred_in[x + y * in_stride]);
      }
    }
  }

  // Transform residual. (residual -> coeff)
  if (use_trskip) {
    kvz_transformskip(state->encoder_control, residual, coeff, width);
  }
  else {
    kvz_transform2d(state->encoder_control, residual, coeff, width, (color == COLOR_Y ? 0 : 65535));
  }

  // Quantize coeffs. (coeff -> quant_coeff)
  if (state->encoder_control->rdoq_enable) {
    int8_t tr_depth = cur_cu->tr_depth - cur_cu->depth;
    tr_depth += (cur_cu->part_size == SIZE_NxN ? 1 : 0);
    kvz_rdoq(state, coeff, quant_coeff, width, width, (color == COLOR_Y ? 0 : 2),
      scan_order, cur_cu->type, tr_depth);
  }
  else {
    kvz_quant(state, coeff, quant_coeff, width, width, (color == COLOR_Y ? 0 : 2),
      scan_order, cur_cu->type);
  }

  // Check if there are any non-zero coefficients.
  {
    int i;
    for (i = 0; i < width * width; ++i) {
      if (quant_coeff[i] != 0) {
        has_coeffs = 1;
        break;
      }
    }
  }

  // Copy coefficients to coeff_out.
  kvz_coefficients_blit(quant_coeff, coeff_out, width, width, width, out_stride);

  // Do the inverse quantization and transformation and the reconstruction to
  // rec_out.
  if (has_coeffs) {
    int y, x;

    // Get quantized residual. (quant_coeff -> coeff -> residual)
    kvz_dequant(state, quant_coeff, coeff, width, width, (color == COLOR_Y ? 0 : (color == COLOR_U ? 2 : 3)), cur_cu->type);
    if (use_trskip) {
      kvz_itransformskip(state->encoder_control, residual, coeff, width);
    }
    else {
      kvz_itransform2d(state->encoder_control, residual, coeff, width, (color == COLOR_Y ? 0 : 65535));
    }

    // Get quantized reconstruction. (residual + pred_in -> rec_out)
    for (y = 0; y < width; ++y) {
      for (x = 0; x < width; ++x) {
        int16_t val = residual[x + y * width] + pred_in[x + y * in_stride];
        rec_out[x + y * out_stride] = (kvz_pixel)CLIP(0, PIXEL_MAX, val);
      }
    }
  }
  else if (rec_out != pred_in) {
    // With no coeffs and rec_out == pred_int we skip copying the coefficients
    // because the reconstruction is just the prediction.
    int y, x;

    for (y = 0; y < width; ++y) {
      for (x = 0; x < width; ++x) {
        rec_out[x + y * out_stride] = pred_in[x + y * in_stride];
      }
    }
  }

  return has_coeffs;
}

/**
 * \brief inverse quantize transformed and quantized coefficents
 *
 */
void kvz_dequant_generic(const encoder_state_t * const state, coeff_t *q_coef, coeff_t *coef, int32_t width, int32_t height,int8_t type, int8_t block_type)
{
  const encoder_control_t * const encoder = state->encoder_control;
  int32_t shift,add,coeff_q;
  int32_t n;
  int32_t transform_shift = 15 - encoder->bitdepth - (kvz_g_convert_to_bit[ width ] + 2);

  int32_t qp_scaled = kvz_get_scaled_qp(type, state->global->QP, (encoder->bitdepth-8)*6);

  shift = 20 - QUANT_SHIFT - transform_shift;

  if (encoder->scaling_list.enable)
  {
    uint32_t log2_tr_size = kvz_g_convert_to_bit[ width ] + 2;
    int32_t scalinglist_type = (block_type == CU_INTRA ? 0 : 3) + (int8_t)("\0\3\1\2"[type]);

    const int32_t *dequant_coef = encoder->scaling_list.de_quant_coeff[log2_tr_size-2][scalinglist_type][qp_scaled%6];
    shift += 4;

    if (shift >qp_scaled / 6) {
      add = 1 << (shift - qp_scaled/6 - 1);

      for (n = 0; n < width * height; n++) {
        coeff_q = ((q_coef[n] * dequant_coef[n]) + add ) >> (shift -  qp_scaled/6);
        coef[n] = (coeff_t)CLIP(-32768,32767,coeff_q);
      }
    } else {
      for (n = 0; n < width * height; n++) {
        // Clip to avoid possible overflow in following shift left operation
        coeff_q   = CLIP(-32768, 32767, q_coef[n] * dequant_coef[n]);
        coef[n] = (coeff_t)CLIP(-32768, 32767, coeff_q << (qp_scaled/6 - shift));
      }
    }
  } else {
    int32_t scale = kvz_g_inv_quant_scales[qp_scaled%6] << (qp_scaled/6);
    add = 1 << (shift-1);

    for (n = 0; n < width*height; n++) {
      coeff_q   = (q_coef[n] * scale + add) >> shift;
      coef[n] = (coeff_t)CLIP(-32768, 32767, coeff_q);
    }
  }
}

int kvz_strategy_register_quant_generic(void* opaque, uint8_t bitdepth)
{
  bool success = true;

  success &= kvz_strategyselector_register(opaque, "quant", "generic", 0, (void*)&kvz_quant_generic);
  success &= kvz_strategyselector_register(opaque, "quantize_residual", "generic", 0, (void*)&kvz_quantize_residual_generic);
  success &= kvz_strategyselector_register(opaque, "dequant", "generic", 0, (void*)&kvz_dequant_generic);

  return success;
}
