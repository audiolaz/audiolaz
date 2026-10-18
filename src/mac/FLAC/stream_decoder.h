/* macOS : no libFLAC on a Mac. The few declarations of libFLAC's stream decoder squeeze.c uses, with the same
 * names and meanings (BSD-licensed API, Xiph.Org), so that squeeze.c builds unchanged; the decoder itself is
 * engine/mac/flac.c (ROOM_FLAC_BUILTIN), or, without it, nothing (ROOM_PORTABLE: libFLAC looked for at run time, not
 * found on a Mac: PCM only). */
#ifndef AUDIOLAZ_MAC_FLAC_STREAM_DECODER_H
#define AUDIOLAZ_MAC_FLAC_STREAM_DECODER_H
#include <stddef.h>
#include <stdint.h>

typedef int FLAC__bool;
typedef uint8_t FLAC__byte;
typedef int32_t FLAC__int32;
typedef uint64_t FLAC__uint64;

typedef struct FLAC__StreamDecoder FLAC__StreamDecoder;

typedef enum { FLAC__STREAM_DECODER_INIT_STATUS_OK = 0, FLAC__STREAM_DECODER_INIT_STATUS_UNSUPPORTED_CONTAINER,
               FLAC__STREAM_DECODER_INIT_STATUS_INVALID_CALLBACKS, FLAC__STREAM_DECODER_INIT_STATUS_MEMORY_ALLOCATION_ERROR,
               FLAC__STREAM_DECODER_INIT_STATUS_ERROR_OPENING_FILE, FLAC__STREAM_DECODER_INIT_STATUS_ALREADY_INITIALIZED } FLAC__StreamDecoderInitStatus;
typedef enum { FLAC__STREAM_DECODER_READ_STATUS_CONTINUE, FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM,
               FLAC__STREAM_DECODER_READ_STATUS_ABORT } FLAC__StreamDecoderReadStatus;
typedef enum { FLAC__STREAM_DECODER_SEEK_STATUS_OK, FLAC__STREAM_DECODER_SEEK_STATUS_ERROR, FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED } FLAC__StreamDecoderSeekStatus;
typedef enum { FLAC__STREAM_DECODER_TELL_STATUS_OK, FLAC__STREAM_DECODER_TELL_STATUS_ERROR, FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED } FLAC__StreamDecoderTellStatus;
typedef enum { FLAC__STREAM_DECODER_LENGTH_STATUS_OK, FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR, FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED } FLAC__StreamDecoderLengthStatus;
typedef enum { FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE, FLAC__STREAM_DECODER_WRITE_STATUS_ABORT } FLAC__StreamDecoderWriteStatus;
typedef enum { FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC, FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER,
               FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH, FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM,
               FLAC__STREAM_DECODER_ERROR_STATUS_BAD_METADATA } FLAC__StreamDecoderErrorStatus;

typedef struct { uint32_t blocksize, sample_rate, channels, channel_assignment, bits_per_sample; int number_type;
                 union { uint32_t frame_number; FLAC__uint64 sample_number; } number; uint8_t crc; } FLAC__FrameHeader;
typedef struct { FLAC__FrameHeader header; } FLAC__Frame;

typedef enum { FLAC__METADATA_TYPE_STREAMINFO = 0 } FLAC__MetadataType;
typedef struct { uint32_t min_blocksize, max_blocksize, min_framesize, max_framesize, sample_rate, channels, bits_per_sample;
                 FLAC__uint64 total_samples; FLAC__byte md5sum[16]; } FLAC__StreamMetadata_StreamInfo;
typedef struct { FLAC__MetadataType type; FLAC__bool is_last; uint32_t length;
                 union { FLAC__StreamMetadata_StreamInfo stream_info; } data; } FLAC__StreamMetadata;

typedef FLAC__StreamDecoderReadStatus (*FLAC__StreamDecoderReadCallback)(const FLAC__StreamDecoder *, FLAC__byte buffer[], size_t *bytes, void *client_data);
typedef FLAC__StreamDecoderSeekStatus (*FLAC__StreamDecoderSeekCallback)(const FLAC__StreamDecoder *, FLAC__uint64 absolute_byte_offset, void *client_data);
typedef FLAC__StreamDecoderTellStatus (*FLAC__StreamDecoderTellCallback)(const FLAC__StreamDecoder *, FLAC__uint64 *absolute_byte_offset, void *client_data);
typedef FLAC__StreamDecoderLengthStatus (*FLAC__StreamDecoderLengthCallback)(const FLAC__StreamDecoder *, FLAC__uint64 *stream_length, void *client_data);
typedef FLAC__bool (*FLAC__StreamDecoderEofCallback)(const FLAC__StreamDecoder *, void *client_data);
typedef FLAC__StreamDecoderWriteStatus (*FLAC__StreamDecoderWriteCallback)(const FLAC__StreamDecoder *, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data);
typedef void (*FLAC__StreamDecoderMetadataCallback)(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *metadata, void *client_data);
typedef void (*FLAC__StreamDecoderErrorCallback)(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus status, void *client_data);

#ifdef ROOM_FLAC_BUILTIN
FLAC__StreamDecoder *FLAC__stream_decoder_new(void);
FLAC__StreamDecoderInitStatus FLAC__stream_decoder_init_stream(FLAC__StreamDecoder *, FLAC__StreamDecoderReadCallback, FLAC__StreamDecoderSeekCallback,
                                                               FLAC__StreamDecoderTellCallback, FLAC__StreamDecoderLengthCallback, FLAC__StreamDecoderEofCallback,
                                                               FLAC__StreamDecoderWriteCallback, FLAC__StreamDecoderMetadataCallback,
                                                               FLAC__StreamDecoderErrorCallback, void *client_data);
FLAC__bool FLAC__stream_decoder_process_until_end_of_stream(FLAC__StreamDecoder *);
FLAC__bool FLAC__stream_decoder_finish(FLAC__StreamDecoder *);
void FLAC__stream_decoder_delete(FLAC__StreamDecoder *);
extern const char *const FLAC__StreamDecoderErrorStatusString[];
#endif
#endif
