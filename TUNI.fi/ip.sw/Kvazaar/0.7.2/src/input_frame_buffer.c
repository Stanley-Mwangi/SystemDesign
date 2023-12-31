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

#include "input_frame_buffer.h"
#include "encoderstate.h"
#include <assert.h>

void kvz_init_input_frame_buffer(input_frame_buffer_t *input_buffer)
{
  FILL(input_buffer->pic_buffer, 0);
  FILL(input_buffer->pts_buffer, 0);
  input_buffer->num_in = 0;
  input_buffer->num_out = 0;
  input_buffer->delay = 0;
  input_buffer->gop_skipped = 0;
}

/**
 * \brief Pass an input frame to the encoder state.
 *
 * Sets the source image of the encoder state if there is a suitable image
 * available.
 *
 * The caller must not modify img_in after calling this function.
 *
 * \param buf     an input frame buffer
 * \param state   a main encoder state
 * \param img_in  input frame or NULL
 * \return        1 if the source image was set, 0 if not
 */
int kvz_encoder_feed_frame(input_frame_buffer_t *buf,
                           encoder_state_t *const state,
                           kvz_picture *const img_in)
{
  const encoder_control_t* const encoder = state->encoder_control;
  const kvz_config* const cfg = encoder->cfg;

  const int gop_buf_size = 3 * cfg->gop_len;

  assert(state->global->frame >= 0);

  videoframe_t *frame = state->tile->frame;
  assert(frame->source == NULL);
  assert(frame->rec    != NULL);

  if (cfg->gop_len == 0) {
    // GOP disabled, just return the input frame.

    if (img_in == NULL) return 0;

    img_in->dts = img_in->pts;
    frame->source   = kvz_image_copy_ref(img_in);
    frame->rec->pts = img_in->pts;
    frame->rec->dts = img_in->dts;
    state->global->gop_offset = 0;
    return 1;
  }

  if (img_in != NULL) {
    // Index of the next input picture, in range [-1, +inf). Values
    // i and j refer to the same indices in buf->pic_buffer iff
    // i === j (mod gop_buf_size).
    int64_t idx_in = buf->num_in - 1;

    // Index in buf->pic_buffer and buf->pts_buffer.
    int buf_idx = (idx_in + gop_buf_size) % gop_buf_size;

    // Save the input image in the buffer.
    assert(buf_idx >= 0 && buf_idx < gop_buf_size);
    assert(buf->pic_buffer[buf_idx] == NULL);
    buf->pic_buffer[buf_idx] = kvz_image_copy_ref(img_in);
    buf->pts_buffer[buf_idx] = img_in->pts;
    buf->num_in++;

    if ((int)buf->num_in < cfg->gop_len) {
      // Not enough frames to start output.
      return 0;

    } else if ((int)buf->num_in == cfg->gop_len) {
      // Now we known the PTSs that are needed to compute the delay.
      buf->delay = buf->pts_buffer[gop_buf_size - 1] - img_in->pts;
    }
  }

  if (buf->num_out == buf->num_in) {
    // All frames returned.
    return 0;
  }

  if (img_in == NULL && (int)buf->num_in < cfg->gop_len) {
    // End of the sequence but we have less than a single GOP of frames. Use
    // the difference between the PTSs of the first and the last frame as the
    // delay.
    int first_pic_idx = gop_buf_size - 1;
    int last_pic_idx  = (buf->num_in - 2 + gop_buf_size) % gop_buf_size;
    buf->delay = buf->pts_buffer[first_pic_idx] - buf->pts_buffer[last_pic_idx];
  }

  // Index of the next output picture, in range [-1, +inf). Values
  // i and j refer to the same indices in buf->pic_buffer iff
  // i === j (mod gop_buf_size).
  int64_t idx_out;

  // DTS of the output picture.
  int64_t dts_out;

  // Number of the next output picture in the GOP.
  int gop_offset;

  if (buf->num_out == 0) {
    // Output the first frame.
    idx_out = -1;
    dts_out = buf->pts_buffer[gop_buf_size - 1] + buf->delay;
    gop_offset = 0;

  } else {
    gop_offset = (buf->num_out - 1) % cfg->gop_len;

    // Index of the first picture in the GOP that is being output.
    int gop_start_idx = buf->num_out - 1 - gop_offset;

    // Skip pictures until we find an available one.
    gop_offset += buf->gop_skipped;
    for (;;) {
      assert(gop_offset < cfg->gop_len);

      idx_out = gop_start_idx + cfg->gop[gop_offset].poc_offset - 1;
      if (idx_out < (int)buf->num_in - 1) {
        // An available picture found.
        break;
      }
      buf->gop_skipped++;
      gop_offset++;
    }

    if ((int)buf->num_out < cfg->gop_len - 1) {
      // This picture needs a DTS that is less than the PTS of the first
      // frame so the delay must be applied.
      int dts_idx = buf->num_out - 1;
      dts_out = buf->pts_buffer[dts_idx % gop_buf_size] + buf->delay;
    } else {
      int dts_idx = buf->num_out - (cfg->gop_len - 1);
      dts_out = buf->pts_buffer[dts_idx % gop_buf_size];
    }
  }

  // Index in buf->pic_buffer and buf->pts_buffer.
  int buf_idx = (idx_out + gop_buf_size) % gop_buf_size;

  assert(buf->pic_buffer[buf_idx] != NULL);
  frame->source      = buf->pic_buffer[buf_idx];
  frame->rec->pts    = frame->source->pts;
  frame->source->dts = dts_out;
  frame->rec->dts    = dts_out;
  buf->pic_buffer[buf_idx] = NULL;
  state->global->gop_offset = gop_offset;

  buf->num_out++;
  return 1;
}
