/*
 * Copyright 2013-2019 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "srslte/phy/utils/debug.h"

#include "srslte/phy/agc/agc.h"
#include "srslte/phy/utils/debug.h"
#include "srslte/phy/utils/vector.h"

int srslte_agc_init(srslte_agc_t* q, srslte_agc_mode_t mode)
{
  return srslte_agc_init_acc(q, mode, 0);
}

int srslte_agc_init_acc(srslte_agc_t* q, srslte_agc_mode_t mode, uint32_t nof_frames)
{
  bzero(q, sizeof(srslte_agc_t));
  q->mode        = mode;
  q->nof_frames  = nof_frames;
  q->max_gain_db = 90.0;
  q->min_gain_db = 0.0;
  if (nof_frames > 0) {
    q->y_tmp = srslte_vec_malloc(sizeof(float) * nof_frames);
    if (!q->y_tmp) {
      return SRSLTE_ERROR;
    }
  } else {
    q->y_tmp = NULL;
  }
  q->target = SRSLTE_AGC_DEFAULT_TARGET;
  srslte_agc_reset(q);
  return SRSLTE_SUCCESS;
}

int srslte_agc_init_uhd(srslte_agc_t*     q,
                        srslte_agc_mode_t mode,
                        uint32_t          nof_frames,
                        SRSLTE_AGC_CALLBACK(set_gain_callback),
                        void* uhd_handler)
{
  if (!srslte_agc_init_acc(q, mode, nof_frames)) {
    q->set_gain_callback = set_gain_callback;
    q->uhd_handler       = uhd_handler;
    return SRSLTE_SUCCESS;
  } else {
    return SRSLTE_ERROR;
  }
}

void srslte_agc_free(srslte_agc_t* q)
{
  if (q->y_tmp) {
    free(q->y_tmp);
  }
  bzero(q, sizeof(srslte_agc_t));
}

void srslte_agc_reset(srslte_agc_t* q)
{
  q->bandwidth = SRSLTE_AGC_DEFAULT_BW;
  q->lock      = false;
  q->gain      = srslte_convert_dB_to_power(q->default_gain_db);
  q->y_out     = 1.0;
  q->isfirst   = true;
  if (q->set_gain_callback && q->uhd_handler) {
    q->set_gain_callback(q->uhd_handler, q->default_gain_db);
  }
}

void srslte_agc_set_gain_range(srslte_agc_t* q, float min_gain_db, float max_gain_db)
{
  if (q) {
    q->min_gain_db = min_gain_db;
    q->max_gain_db = max_gain_db;
    q->default_gain_db = (max_gain_db + min_gain_db) / 2.0f;
  }
}

void srslte_agc_set_bandwidth(srslte_agc_t* q, float bandwidth)
{
  q->bandwidth = bandwidth;
}

void srslte_agc_set_target(srslte_agc_t* q, float target)
{
  q->target = target;
}

float srslte_agc_get_rssi(srslte_agc_t* q)
{
  return q->target / q->gain;
}

float srslte_agc_get_output_level(srslte_agc_t* q)
{
  return q->y_out;
}

float srslte_agc_get_gain(srslte_agc_t* q)
{
  return q->gain;
}

void srslte_agc_set_gain(srslte_agc_t* q, float init_gain_value_db)
{
  q->gain = srslte_convert_dB_to_power(init_gain_value_db);
}

void srslte_agc_lock(srslte_agc_t* q, bool enable)
{
  q->lock = enable;
}

void srslte_agc_process(srslte_agc_t* q, cf_t* signal, uint32_t len)
{
  if (!q->lock) {
    float gain_db     = srslte_convert_power_to_dB(q->gain);

    float y = 0;
    // Apply current gain to input signal
    if (!q->uhd_handler) {
      srslte_vec_sc_prod_cfc(signal, q->gain, signal, len);
    } else {
      if (gain_db < q->min_gain_db) {
        gain_db = q->min_gain_db + 5.0;
        INFO("Warning: Rx signal strength is too high. Forcing minimum Rx gain %.2fdB\n", gain_db);
      } else if (gain_db > q->max_gain_db) {
        gain_db = q->max_gain_db;
        INFO("Warning: Rx signal strength is too weak. Forcing maximum Rx gain %.2fdB\n", gain_db);
      } else if (isinf(gain_db) || isnan(gain_db)) {
        gain_db = q->default_gain_db;
        INFO("Warning: AGC went to an unknown state. Setting Rx gain to %.2fdB\n", gain_db);
      }

      // Set gain
      q->set_gain_callback(q->uhd_handler, gain_db);
      q->gain = srslte_convert_dB_to_power(gain_db);
    }
    float* t;
    switch (q->mode) {
      case SRSLTE_AGC_MODE_ENERGY:
        y = sqrtf(crealf(srslte_vec_dot_prod_conj_ccc(signal, signal, len)) / len);
        break;
      case SRSLTE_AGC_MODE_PEAK_AMPLITUDE:
        t = (float*)signal;
        y = t[srslte_vec_max_fi(t, 2 * len)]; // take only positive max to avoid abs() (should be similar)
        break;
      default:
        ERROR("Unsupported AGC mode\n");
        return;
    }

    if (q->nof_frames > 0) {
      q->y_tmp[q->frame_cnt++] = y;
      if (q->frame_cnt == q->nof_frames) {
        q->frame_cnt = 0;
        switch (q->mode) {
          case SRSLTE_AGC_MODE_ENERGY:
            y = srslte_vec_acc_ff(q->y_tmp, q->nof_frames) / q->nof_frames;
            break;
          case SRSLTE_AGC_MODE_PEAK_AMPLITUDE:
            y = q->y_tmp[srslte_vec_max_fi(q->y_tmp, q->nof_frames)];
            break;
          default:
            ERROR("Unsupported AGC mode\n");
            return;
        }
      }
    }

    if (q->isfirst) {
      q->y_out   = y;
      q->isfirst = false;
    } else {
      if (q->frame_cnt == 0) {
        q->y_out = (1 - q->bandwidth) * q->y_out + q->bandwidth * y;
        if (!q->lock) {
          q->gain *= q->target / q->y_out;
        }
        INFO("AGC gain: %.2f y_out=%.3f, y=%.3f target=%.1f\n", gain_db, q->y_out, y, q->target);
      }
    }
  }
}
