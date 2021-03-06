/* Copyright (c) 2017 Google Inc.
   Written by Andrew Allen */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mathops.h"
#include "os_support.h"
#include "opus_private.h"
#include "opus_defines.h"
#include "opus_projection.h"
#include "opus_multistream.h"
#include "stack_alloc.h"
#include "mapping_matrix.h"

#ifdef ENABLE_EXPERIMENTAL_AMBISONICS

struct OpusProjectionEncoder
{
  int mixing_matrix_size_in_bytes;
  int demixing_matrix_size_in_bytes;
  /* Encoder states go here */
};

static int get_order_plus_one_from_channels(int channels, int *order_plus_one)
{
  int order_plus_one_;
  int acn_channels;
  int nondiegetic_channels;

  /* Allowed numbers of channels:
   * (1 + n)^2 + 2j, for n = 0...14 and j = 0 or 1.
   */
  order_plus_one_ = isqrt32(channels);
  acn_channels = order_plus_one_ * order_plus_one_;
  nondiegetic_channels = channels - acn_channels;
  if (order_plus_one)
    *order_plus_one = order_plus_one_;

  if (order_plus_one_ < 1 || order_plus_one_ > 15 ||
      (nondiegetic_channels != 0 && nondiegetic_channels != 2))
    return OPUS_BAD_ARG;
  return OPUS_OK;
}

static int get_streams_from_channels(int channels, int mapping_family,
                                     int *streams, int *coupled_streams,
                                     int *order_plus_one)
{
  if (mapping_family == 253)
  {
    if (get_order_plus_one_from_channels(channels, order_plus_one) != OPUS_OK)
      return OPUS_BAD_ARG;
    if (streams)
      *streams = (channels + 1) / 2;
    if (coupled_streams)
      *coupled_streams = channels / 2;
    return OPUS_OK;
  }
  return OPUS_BAD_ARG;
}

static MappingMatrix *get_mixing_matrix(OpusProjectionEncoder *st)
{
  return (MappingMatrix *)((char*)st + align(sizeof(OpusProjectionEncoder)));
}

static MappingMatrix *get_demixing_matrix(OpusProjectionEncoder *st)
{
  return (MappingMatrix *)((char*)st + align(sizeof(OpusProjectionEncoder) +
    st->mixing_matrix_size_in_bytes));
}

static OpusMSEncoder *get_multistream_encoder(OpusProjectionEncoder *st)
{
  return (OpusMSEncoder *)((char*)st + align(sizeof(OpusProjectionEncoder) +
    st->mixing_matrix_size_in_bytes + st->demixing_matrix_size_in_bytes));
}

opus_int32 opus_projection_ambisonics_encoder_get_size(int channels,
                                                       int mapping_family)
{
  int nb_streams;
  int nb_coupled_streams;
  int order_plus_one;
  int matrix_rows;
  opus_int32 matrix_size;
  opus_int32 encoder_size;
  int ret;

  ret = get_streams_from_channels(channels, mapping_family, &nb_streams,
                                  &nb_coupled_streams, &order_plus_one);
  if (ret != OPUS_OK)
  {
    return 0;
  }

  matrix_rows = order_plus_one * order_plus_one + 2;
  matrix_size = mapping_matrix_get_size(matrix_rows, matrix_rows);
  encoder_size =
      opus_multistream_encoder_get_size(nb_streams, nb_coupled_streams);
  if (!encoder_size)
    return 0;
  return align(sizeof(OpusProjectionEncoder) + matrix_size + matrix_size + encoder_size);
}

int opus_projection_ambisonics_encoder_init(OpusProjectionEncoder *st, opus_int32 Fs,
                                            int channels, int mapping_family,
                                            int *streams, int *coupled_streams,
                                            int application)
{
  MappingMatrix *mixing_matrix;
  MappingMatrix *demixing_matrix;
  OpusMSEncoder *ms_encoder;
  int nb_streams;
  int nb_coupled_streams;
  int i;
  int ret;
  unsigned char mapping[255];

  if (get_streams_from_channels(channels, mapping_family,
                                &nb_streams, &nb_coupled_streams, NULL)
      != OPUS_OK)
    return OPUS_BAD_ARG;

  if (streams == NULL || coupled_streams == NULL) {
    return OPUS_BAD_ARG;
  }
  *streams = nb_streams;
  *coupled_streams = nb_coupled_streams;

  if (mapping_family == 253)
  {
    int order_plus_one;
    if (get_order_plus_one_from_channels(channels, &order_plus_one) != OPUS_OK)
      return OPUS_BAD_ARG;

    /* Assign mixing matrix based on available pre-computed matrices. */
    mixing_matrix = get_mixing_matrix(st);
    if (order_plus_one == 2)
    {
      mapping_matrix_init(mixing_matrix, mapping_matrix_foa_mixing.rows,
        mapping_matrix_foa_mixing.cols, mapping_matrix_foa_mixing.gain,
        mapping_matrix_foa_mixing_data, 36 * sizeof(opus_int16));
    }
    else if (order_plus_one == 3)
    {
      mapping_matrix_init(mixing_matrix, mapping_matrix_soa_mixing.rows,
        mapping_matrix_soa_mixing.cols, mapping_matrix_soa_mixing.gain,
        mapping_matrix_soa_mixing_data, 121 * sizeof(opus_int16));
    }
    else if (order_plus_one == 4)
    {
      mapping_matrix_init(mixing_matrix, mapping_matrix_toa_mixing.rows,
        mapping_matrix_toa_mixing.cols, mapping_matrix_toa_mixing.gain,
        mapping_matrix_toa_mixing_data, 324 * sizeof(opus_int16));
    }
    st->mixing_matrix_size_in_bytes = mapping_matrix_get_size(
      mixing_matrix->rows, mixing_matrix->cols);

    /* Assign demixing matrix based on available pre-computed matrices. */
    demixing_matrix = get_demixing_matrix(st);
    if (order_plus_one == 2)
    {
      mapping_matrix_init(demixing_matrix, mapping_matrix_foa_demixing.rows,
        mapping_matrix_foa_demixing.cols, mapping_matrix_foa_demixing.gain,
        mapping_matrix_foa_demixing_data, 36 * sizeof(opus_int16));
    }
    else if (order_plus_one == 3)
    {
      mapping_matrix_init(demixing_matrix, mapping_matrix_soa_demixing.rows,
        mapping_matrix_soa_demixing.cols, mapping_matrix_soa_demixing.gain,
        mapping_matrix_soa_demixing_data, 121 * sizeof(opus_int16));
    }
    else if (order_plus_one == 4)
    {
      mapping_matrix_init(demixing_matrix, mapping_matrix_toa_demixing.rows,
        mapping_matrix_toa_demixing.cols, mapping_matrix_toa_demixing.gain,
        mapping_matrix_toa_demixing_data, 324 * sizeof(opus_int16));
    }
    st->demixing_matrix_size_in_bytes = mapping_matrix_get_size(
      demixing_matrix->rows, demixing_matrix->cols);
  }
  else
    return OPUS_UNIMPLEMENTED;

  /* Ensure matrices are large enough for desired coding scheme. */
  if (nb_streams + nb_coupled_streams > mixing_matrix->rows ||
      channels > mixing_matrix->cols ||
      channels > demixing_matrix->rows ||
      nb_streams + nb_coupled_streams > demixing_matrix->cols)
    return OPUS_BAD_ARG;

  /* Set trivial mapping so each input channel pairs with a matrix column. */
  for (i = 0; i < channels; i++)
  {
    mapping[i] = i;
  }

  /* Initialize multistream encoder with provided settings. */
  ms_encoder = get_multistream_encoder(st);
  ret = opus_multistream_encoder_init(ms_encoder, Fs, channels, nb_streams,
                                      nb_coupled_streams, mapping, application);
  return ret;
}

OpusProjectionEncoder *opus_projection_ambisonics_encoder_create(
    opus_int32 Fs, int channels, int mapping_family, int *streams,
    int *coupled_streams, int application, int *error)
{
  int size;
  int ret;
  OpusProjectionEncoder *st;

  /* Allocate space for the projection encoder. */
  size = opus_projection_ambisonics_encoder_get_size(channels, mapping_family);
  if (!size) {
    if (error)
      *error = OPUS_ALLOC_FAIL;
    return NULL;
  }
  st = (OpusProjectionEncoder *)opus_alloc(size);
  if (!st)
  {
    if (error)
      *error = OPUS_ALLOC_FAIL;
    return NULL;
  }

  /* Initialize projection encoder with provided settings. */
  ret = opus_projection_ambisonics_encoder_init(st, Fs, channels,
     mapping_family, streams, coupled_streams, application);
  if (ret != OPUS_OK)
  {
    opus_free(st);
    st = NULL;
  }
  if (error)
    *error = ret;
  return st;
}

int opus_projection_encode(OpusProjectionEncoder *st, const opus_int16 *pcm,
                           int frame_size, unsigned char *data,
                           opus_int32 max_data_bytes)
{
#ifdef NONTHREADSAFE_PSEUDOSTACK
  celt_fatal("Unable to use opus_projection_encode() when NONTHREADSAFE_PSEUDOSTACK is defined.");
#endif
  MappingMatrix *matrix;
  OpusMSEncoder *ms_encoder;
  int ret;
  VARDECL(opus_int16, buf);
  ALLOC_STACK;

  matrix = get_mixing_matrix(st);
  ms_encoder = get_multistream_encoder(st);
  ALLOC(buf, (ms_encoder->layout.nb_streams + ms_encoder->layout.nb_coupled_streams) *
    frame_size, opus_int16);
  mapping_matrix_multiply_short(matrix, pcm,
    ms_encoder->layout.nb_channels, buf,
    ms_encoder->layout.nb_streams + ms_encoder->layout.nb_coupled_streams,
    frame_size);
  ret = opus_multistream_encode(ms_encoder, buf, frame_size, data, max_data_bytes);
  RESTORE_STACK;
  return ret;
}

#ifndef DISABLE_FLOAT_API
int opus_projection_encode_float(OpusProjectionEncoder *st, const float *pcm,
                                 int frame_size, unsigned char *data,
                                 opus_int32 max_data_bytes)
{
#ifdef NONTHREADSAFE_PSEUDOSTACK
  celt_fatal("Unable to use opus_projection_encode_float() when NONTHREADSAFE_PSEUDOSTACK is defined.");
#endif
  MappingMatrix *matrix;
  OpusMSEncoder *ms_encoder;
  int ret;
  VARDECL(float, buf);
  ALLOC_STACK;

  matrix = get_mixing_matrix(st);
  ms_encoder = get_multistream_encoder(st);
  ALLOC(buf, (ms_encoder->layout.nb_streams + ms_encoder->layout.nb_coupled_streams) *
    frame_size, float);
  mapping_matrix_multiply_float(matrix, pcm,
    ms_encoder->layout.nb_channels, buf,
    ms_encoder->layout.nb_streams + ms_encoder->layout.nb_coupled_streams,
    frame_size);
  ret = opus_multistream_encode_float(ms_encoder, buf, frame_size, data, max_data_bytes);
  RESTORE_STACK;
  return ret;
}
#endif

void opus_projection_encoder_destroy(OpusProjectionEncoder *st)
{
  opus_free(st);
}

int opus_projection_encoder_ctl(OpusProjectionEncoder *st, int request, ...)
{
  MappingMatrix *demixing_matrix;
  OpusMSEncoder *ms_encoder;
  int ret = OPUS_OK;

  ms_encoder = get_multistream_encoder(st);
  demixing_matrix = get_demixing_matrix(st);

  va_list ap;
  va_start(ap, request);
  switch(request)
  {
  case OPUS_PROJECTION_GET_DEMIXING_MATRIX_SIZE_REQUEST:
  {
    opus_int32 *value = va_arg(ap, opus_int32*);
    if (!value)
    {
      goto bad_arg;
    }
    *value =
      ms_encoder->layout.nb_channels * (ms_encoder->layout.nb_streams
      + ms_encoder->layout.nb_coupled_streams) * sizeof(opus_int16);
  }
  break;
  case OPUS_PROJECTION_GET_DEMIXING_MATRIX_GAIN_REQUEST:
  {
    opus_int32 *value = va_arg(ap, opus_int32*);
    if (!value)
    {
      goto bad_arg;
    }
    *value = demixing_matrix->gain;
  }
  break;
  case OPUS_PROJECTION_GET_DEMIXING_MATRIX_REQUEST:
  {
    int i;
    int nb_input_streams;
    int nb_output_streams;
    unsigned char *external_char;
    opus_int16 *internal_short;
    opus_int32 external_size;
    opus_int32 internal_size;

    /* (I/O is in relation to the decoder's perspective). */
    nb_input_streams = ms_encoder->layout.nb_streams +
      ms_encoder->layout.nb_coupled_streams;
    nb_output_streams = ms_encoder->layout.nb_channels;

    external_char = va_arg(ap, unsigned char *);
    external_size = va_arg(ap, opus_uint32);
    if (!external_char)
    {
      goto bad_arg;
    }
    internal_short = mapping_matrix_get_data(demixing_matrix);
    internal_size = nb_input_streams * nb_output_streams * sizeof(opus_int16);
    if (external_size != internal_size)
    {
      goto bad_arg;
    }

    /* Copy demixing matrix subset to output destination. */
    for (i = 0; i < nb_input_streams * nb_output_streams; i++)
    {
      external_char[2*i] = (unsigned char)internal_short[i];
      external_char[2*i+1] = (unsigned char)(internal_short[i] >> 8);
    }
  }
  break;
  default:
  {
    ret = opus_multistream_encoder_ctl_va_list(ms_encoder, request, ap);
  }
  break;
  }
  va_end(ap);
  return ret;

bad_arg:
  va_end(ap);
  return OPUS_BAD_ARG;
}

#endif /* ENABLE_EXPERIMENTAL_AMBISONICS */
