/*
 * "Real" compatible demuxer.
 * Copyright (c) 2000, 2001 Fabrice Bellard
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "avformat.h"
#include "riff.h"
#include "rm.h"

struct RMStream {
    AVPacket pkt;      ///< place to store merged video frame / reordered audio data
    int videobufsize;  ///< current assembled frame size
    int videobufpos;   ///< position for the next slice in the video buffer
    int curpic_num;    ///< picture number of current frame
    int cur_slice, slices;
    int64_t pktpos;    ///< first slice position in file
    /// Audio descrambling matrix parameters
    int64_t audiotimestamp; ///< Audio packet timestamp
    int sub_packet_cnt; // Subpacket counter, used while reading
    int sub_packet_size, sub_packet_h, coded_framesize; ///< Descrambling parameters from container
    int audio_framesize; /// Audio frame size from container
    int sub_packet_lengths[16]; /// Length of each subpacket
};

typedef struct {
    int nb_packets;
    int old_format;
    int current_stream;
    int remaining_len;
    int audio_stream_num; ///< Stream number for audio packets
    int audio_pkt_cnt; ///< Output packet counter
} RMDemuxContext;

static const unsigned char sipr_swaps[38][2] = {
    {  0, 63 }, {  1, 22 }, {  2, 44 }, {  3, 90 },
    {  5, 81 }, {  7, 31 }, {  8, 86 }, {  9, 58 },
    { 10, 36 }, { 12, 68 }, { 13, 39 }, { 14, 73 },
    { 15, 53 }, { 16, 69 }, { 17, 57 }, { 19, 88 },
    { 20, 34 }, { 21, 71 }, { 24, 46 }, { 25, 94 },
    { 26, 54 }, { 28, 75 }, { 29, 50 }, { 32, 70 },
    { 33, 92 }, { 35, 74 }, { 38, 85 }, { 40, 56 },
    { 42, 87 }, { 43, 65 }, { 45, 59 }, { 48, 79 },
    { 49, 93 }, { 51, 89 }, { 55, 95 }, { 61, 76 },
    { 67, 83 }, { 77, 80 }
};

const unsigned char ff_sipr_subpk_size[4] = { 29, 19, 37, 20 };

static inline void get_strl(AVIOContext *pb, char *buf, int buf_size, int len)
{
    int i;
    char *q, r;

    q = buf;
    for(i=0;i<len;i++) {
        r = avio_r8(pb);
        if (i < buf_size - 1)
            *q++ = r;
    }
    if (buf_size > 0) *q = '\0';
}

static void get_str8(AVIOContext *pb, char *buf, int buf_size)
{
    get_strl(pb, buf, buf_size, avio_r8(pb));
}

static int rm_read_extradata(AVIOContext *pb, AVCodecContext *avctx, unsigned size)
{
    if (size >= 1<<24)
        return -1;
    avctx->extradata = av_malloc(size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = avio_read(pb, avctx->extradata, size);
    memset(avctx->extradata + avctx->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    if (avctx->extradata_size != size)
        return AVERROR(EIO);
    return 0;
}

static void rm_read_metadata(AVFormatContext *s, int wide)
{
    char buf[1024];
    int i;
    for (i=0; i<FF_ARRAY_ELEMS(ff_rm_metadata); i++) {
        int len = wide ? avio_rb16(s->pb) : avio_r8(s->pb);
        get_strl(s->pb, buf, sizeof(buf), len);
        av_dict_set(&s->metadata, ff_rm_metadata[i], buf, 0);
    }
}

RMStream *ff_rm_alloc_rmstream (void)
{
    RMStream *rms = av_mallocz(sizeof(RMStream));
    rms->curpic_num = -1;
    return rms;
}

void ff_rm_free_rmstream (RMStream *rms)
{
    av_free_packet(&rms->pkt);
}

static int rm_read_audio_stream_info(AVFormatContext *s, AVIOContext *pb,
                                     AVStream *st, RMStream *ast, int read_all)
{
    char buf[256];
    uint32_t version;
    int ret;

    /* ra type header */
    version = avio_rb16(pb); /* version */
    if (version == 3) {
        int header_size = avio_rb16(pb);
        int64_t startpos = avio_tell(pb);
        avio_skip(pb, 14);
        rm_read_metadata(s, 0);
        if ((startpos + header_size) >= avio_tell(pb) + 2) {
            // fourcc (should always be "lpcJ")
            avio_r8(pb);
            get_str8(pb, buf, sizeof(buf));
        }
        // Skip extra header crap (this should never happen)
        if ((startpos + header_size) > avio_tell(pb))
            avio_skip(pb, header_size + startpos - avio_tell(pb));
        st->codec->sample_rate = 8000;
        st->codec->channels = 1;
        st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codec->codec_id = CODEC_ID_RA_144;
    } else {
        int flavor, sub_packet_h, coded_framesize, sub_packet_size;
        int codecdata_length;
        /* old version (4) */
        avio_skip(pb, 2); /* unused */
        avio_rb32(pb); /* .ra4 */
        avio_rb32(pb); /* data size */
        avio_rb16(pb); /* version2 */
        avio_rb32(pb); /* header size */
        flavor= avio_rb16(pb); /* add codec info / flavor */
        ast->coded_framesize = coded_framesize = avio_rb32(pb); /* coded frame size */
        avio_rb32(pb); /* ??? */
        avio_rb32(pb); /* ??? */
        avio_rb32(pb); /* ??? */
        ast->sub_packet_h = sub_packet_h = avio_rb16(pb); /* 1 */
        st->codec->block_align= avio_rb16(pb); /* frame size */
        ast->sub_packet_size = sub_packet_size = avio_rb16(pb); /* sub packet size */
        avio_rb16(pb); /* ??? */
        if (version == 5) {
            avio_rb16(pb); avio_rb16(pb); avio_rb16(pb);
        }
        st->codec->sample_rate = avio_rb16(pb);
        avio_rb32(pb);
        st->codec->channels = avio_rb16(pb);
        if (version == 5) {
            avio_rb32(pb);
            avio_read(pb, buf, 4);
            buf[4] = 0;
        } else {
            get_str8(pb, buf, sizeof(buf)); /* desc */
            get_str8(pb, buf, sizeof(buf)); /* desc */
        }
        st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codec->codec_tag  = AV_RL32(buf);
        st->codec->codec_id   = ff_codec_get_id(ff_rm_codec_tags,
                                                st->codec->codec_tag);
        switch (st->codec->codec_id) {
        case CODEC_ID_AC3:
            st->need_parsing = AVSTREAM_PARSE_FULL;
            break;
        case CODEC_ID_RA_288:
            st->codec->extradata_size= 0;
            ast->audio_framesize = st->codec->block_align;
            st->codec->block_align = coded_framesize;

            if(ast->audio_framesize >= UINT_MAX / sub_packet_h){
                av_log(s, AV_LOG_ERROR, "ast->audio_framesize * sub_packet_h too large\n");
                return -1;
            }

            av_new_packet(&ast->pkt, ast->audio_framesize * sub_packet_h);
            break;
        case CODEC_ID_COOK:
        case CODEC_ID_ATRAC3:
        case CODEC_ID_SIPR:
            avio_rb16(pb); avio_r8(pb);
            if (version == 5)
                avio_r8(pb);
            codecdata_length = avio_rb32(pb);
            if(codecdata_length + FF_INPUT_BUFFER_PADDING_SIZE <= (unsigned)codecdata_length){
                av_log(s, AV_LOG_ERROR, "codecdata_length too large\n");
                return -1;
            }

            ast->audio_framesize = st->codec->block_align;
            if (st->codec->codec_id == CODEC_ID_SIPR) {
                if (flavor > 3) {
                    av_log(s, AV_LOG_ERROR, "bad SIPR file flavor %d\n",
                           flavor);
                    return -1;
                }
                st->codec->block_align = ff_sipr_subpk_size[flavor];
            } else {
                if(sub_packet_size <= 0){
                    av_log(s, AV_LOG_ERROR, "sub_packet_size is invalid\n");
                    return -1;
                }
                st->codec->block_align = ast->sub_packet_size;
            }
            if ((ret = rm_read_extradata(pb, st->codec, codecdata_length)) < 0)
                return ret;

            if(ast->audio_framesize >= UINT_MAX / sub_packet_h){
                av_log(s, AV_LOG_ERROR, "rm->audio_framesize * sub_packet_h too large\n");
                return -1;
            }

            av_new_packet(&ast->pkt, ast->audio_framesize * sub_packet_h);
            break;
        case CODEC_ID_AAC:
            avio_rb16(pb); avio_r8(pb);
            if (version == 5)
                avio_r8(pb);
            codecdata_length = avio_rb32(pb);
            if(codecdata_length + FF_INPUT_BUFFER_PADDING_SIZE <= (unsigned)codecdata_length){
                av_log(s, AV_LOG_ERROR, "codecdata_length too large\n");
                return -1;
            }
            if (codecdata_length >= 1) {
                avio_r8(pb);
                if ((ret = rm_read_extradata(pb, st->codec, codecdata_length - 1)) < 0)
                    return ret;
            }
            break;
        default:
            av_strlcpy(st->codec->codec_name, buf, sizeof(st->codec->codec_name));
        }
        if (read_all) {
            avio_r8(pb);
            avio_r8(pb);
            avio_r8(pb);
            rm_read_metadata(s, 0);
        }
    }
    return 0;
}

int
ff_rm_read_mdpr_codecdata (AVFormatContext *s, AVIOContext *pb,
                           AVStream *st, RMStream *rst, int codec_data_size)
{
    unsigned int v;
    int size;
    int64_t codec_pos;
    int ret;

    av_set_pts_info(st, 64, 1, 1000);
    codec_pos = avio_tell(pb);
    v = avio_rb32(pb);
    if (v == MKTAG(0xfd, 'a', 'r', '.')) {
        /* ra type header */
        if (rm_read_audio_stream_info(s, pb, st, rst, 0))
            return -1;
    } else {
        int fps;
        if (avio_rl32(pb) != MKTAG('V', 'I', 'D', 'O')) {
        fail1:
            av_log(st->codec, AV_LOG_ERROR, "Unsupported video codec\n");
            goto skip;
        }
        st->codec->codec_tag = avio_rl32(pb);
        st->codec->codec_id  = ff_codec_get_id(ff_rm_codec_tags,
                                               st->codec->codec_tag);
//        av_log(s, AV_LOG_DEBUG, "%X %X\n", st->codec->codec_tag, MKTAG('R', 'V', '2', '0'));
        if (st->codec->codec_id == CODEC_ID_NONE)
            goto fail1;
        st->codec->width  = avio_rb16(pb);
        st->codec->height = avio_rb16(pb);
        avio_skip(pb, 2); // looks like bits per sample
        avio_skip(pb, 4); // always zero?
        st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
        st->need_parsing = AVSTREAM_PARSE_TIMESTAMPS;
        fps = avio_rb32(pb);

        if ((ret = rm_read_extradata(pb, st->codec, codec_data_size - (avio_tell(pb) - codec_pos))) < 0)
            return ret;

        av_reduce(&st->codec->time_base.num, &st->codec->time_base.den,
                  0x10000, fps, (1 << 30) - 1);
        st->avg_frame_rate.num = st->codec->time_base.den;
        st->avg_frame_rate.den = st->codec->time_base.num;
    }

skip:
    /* skip codec info */
    size = avio_tell(pb) - codec_pos;
    avio_skip(pb, codec_data_size - size);

    return 0;
}

/** this function assumes that the demuxer has already seeked to the start
 * of the INDX chunk, and will bail out if not. */
static int rm_read_index(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    unsigned int size, n_pkts, str_id, next_off, n, pos, pts;
    AVStream *st;

    do {
        if (avio_rl32(pb) != MKTAG('I','N','D','X'))
            return -1;
        size     = avio_rb32(pb);
        if (size < 20)
            return -1;
        avio_skip(pb, 2);
        n_pkts   = avio_rb32(pb);
        str_id   = avio_rb16(pb);
        next_off = avio_rb32(pb);
        for (n = 0; n < s->nb_streams; n++)
            if (s->streams[n]->id == str_id) {
                st = s->streams[n];
                break;
            }
        if (n == s->nb_streams)
            goto skip;

        for (n = 0; n < n_pkts; n++) {
            avio_skip(pb, 2);
            pts = avio_rb32(pb);
            pos = avio_rb32(pb);
            avio_skip(pb, 4); /* packet no. */

            av_add_index_entry(st, pos, pts, 0, 0, AVINDEX_KEYFRAME);
        }

skip:
        if (next_off && avio_tell(pb) != next_off &&
            avio_seek(pb, next_off, SEEK_SET) < 0)
            return -1;
    } while (next_off);

    return 0;
}

static int rm_read_header_old(AVFormatContext *s)
{
    RMDemuxContext *rm = s->priv_data;
    AVStream *st;

    rm->old_format = 1;
    st = av_new_stream(s, 0);
    if (!st)
        return -1;
    st->priv_data = ff_rm_alloc_rmstream();
    return rm_read_audio_stream_info(s, s->pb, st, st->priv_data, 1);
}

static int rm_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    RMDemuxContext *rm = s->priv_data;
    AVStream *st;
    AVIOContext *pb = s->pb;
    unsigned int tag;
    int tag_size;
    unsigned int start_time, duration;
    unsigned int data_off = 0, indx_off = 0;
    char buf[128];
    int flags = 0;

    tag = avio_rl32(pb);
    if (tag == MKTAG('.', 'r', 'a', 0xfd)) {
        /* very old .ra format */
        return rm_read_header_old(s);
    } else if (tag != MKTAG('.', 'R', 'M', 'F')) {
        return AVERROR(EIO);
    }

    avio_rb32(pb); /* header size */
    avio_rb16(pb);
    avio_rb32(pb);
    avio_rb32(pb); /* number of headers */

    for(;;) {
        if (pb->eof_reached)
            return -1;
        tag = avio_rl32(pb);
        tag_size = avio_rb32(pb);
        avio_rb16(pb);
        av_dlog(s, "tag=%c%c%c%c (%08x) size=%d\n",
                (tag      ) & 0xff,
                (tag >>  8) & 0xff,
                (tag >> 16) & 0xff,
                (tag >> 24) & 0xff,
                tag,
                tag_size);
        if (tag_size < 10 && tag != MKTAG('D', 'A', 'T', 'A'))
            return -1;
        switch(tag) {
        case MKTAG('P', 'R', 'O', 'P'):
            /* file header */
            avio_rb32(pb); /* max bit rate */
            avio_rb32(pb); /* avg bit rate */
            avio_rb32(pb); /* max packet size */
            avio_rb32(pb); /* avg packet size */
            avio_rb32(pb); /* nb packets */
            avio_rb32(pb); /* duration */
            avio_rb32(pb); /* preroll */
            indx_off = avio_rb32(pb); /* index offset */
            data_off = avio_rb32(pb); /* data offset */
            avio_rb16(pb); /* nb streams */
            flags = avio_rb16(pb); /* flags */
            break;
        case MKTAG('C', 'O', 'N', 'T'):
            rm_read_metadata(s, 1);
            break;
        case MKTAG('M', 'D', 'P', 'R'):
            st = av_new_stream(s, 0);
            if (!st)
                return AVERROR(ENOMEM);
            st->id = avio_rb16(pb);
            avio_rb32(pb); /* max bit rate */
            st->codec->bit_rate = avio_rb32(pb); /* bit rate */
            avio_rb32(pb); /* max packet size */
            avio_rb32(pb); /* avg packet size */
            start_time = avio_rb32(pb); /* start time */
            avio_rb32(pb); /* preroll */
            duration = avio_rb32(pb); /* duration */
            st->start_time = start_time;
            st->duration = duration;
            get_str8(pb, buf, sizeof(buf)); /* desc */
            get_str8(pb, buf, sizeof(buf)); /* mimetype */
            st->codec->codec_type = AVMEDIA_TYPE_DATA;
            st->priv_data = ff_rm_alloc_rmstream();
            if (ff_rm_read_mdpr_codecdata(s, s->pb, st, st->priv_data,
                                          avio_rb32(pb)) < 0)
                return -1;
            break;
        case MKTAG('D', 'A', 'T', 'A'):
            goto header_end;
        default:
            /* unknown tag: skip it */
            avio_skip(pb, tag_size - 10);
            break;
        }
    }
 header_end:
    rm->nb_packets = avio_rb32(pb); /* number of packets */
    if (!rm->nb_packets && (flags & 4))
        rm->nb_packets = 3600 * 25;
    avio_rb32(pb); /* next data header */

    if (!data_off)
        data_off = avio_tell(pb) - 18;
    if (indx_off && pb->seekable && !(s->flags & AVFMT_FLAG_IGNIDX) &&
        avio_seek(pb, indx_off, SEEK_SET) >= 0) {
        rm_read_index(s);
        avio_seek(pb, data_off + 18, SEEK_SET);
    }

    return 0;
}

static int get_num(AVIOContext *pb, int *len)
{
    int n, n1;

    n = avio_rb16(pb);
    (*len)-=2;
    n &= 0x7FFF;
    if (n >= 0x4000) {
        return n - 0x4000;
    } else {
        n1 = avio_rb16(pb);
        (*len)-=2;
        return (n << 16) | n1;
    }
}

/* multiple of 20 bytes for ra144 (ugly) */
#define RAW_PACKET_SIZE 1000

static int sync(AVFormatContext *s, int64_t *timestamp, int *flags, int *stream_index, int64_t *pos){
    RMDemuxContext *rm = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *st;
    uint32_t state=0xFFFFFFFF;

    while(!pb->eof_reached){
        int len, num, i;
        *pos= avio_tell(pb) - 3;
        if(rm->remaining_len > 0){
            num= rm->current_stream;
            len= rm->remaining_len;
            *timestamp = AV_NOPTS_VALUE;
            *flags= 0;
        }else{
            state= (state<<8) + avio_r8(pb);

            if(state == MKBETAG('I', 'N', 'D', 'X')){
                int n_pkts, expected_len;
                len = avio_rb32(pb);
                avio_skip(pb, 2);
                n_pkts = avio_rb32(pb);
                expected_len = 20 + n_pkts * 14;
                if (len == 20)
                    /* some files don't add index entries to chunk size... */
                    len = expected_len;
                else if (len != expected_len)
                    av_log(s, AV_LOG_WARNING,
                           "Index size %d (%d pkts) is wrong, should be %d.\n",
                           len, n_pkts, expected_len);
                len -= 14; // we already read part of the index header
                if(len<0)
                    continue;
                goto skip;
            } else if (state == MKBETAG('D','A','T','A')) {
                av_log(s, AV_LOG_WARNING,
                       "DATA tag in middle of chunk, file may be broken.\n");
            }

            if(state > (unsigned)0xFFFF || state <= 12)
                continue;
            len=state - 12;
            state= 0xFFFFFFFF;

            num = avio_rb16(pb);
            *timestamp = avio_rb32(pb);
            avio_r8(pb); /* reserved */
            *flags = avio_r8(pb); /* flags */
        }
        for(i=0;i<s->nb_streams;i++) {
            st = s->streams[i];
            if (num == st->id)
                break;
        }
        if (i == s->nb_streams) {
skip:
            /* skip packet if unknown number */
            avio_skip(pb, len);
            rm->remaining_len = 0;
            continue;
        }
        *stream_index= i;

        return len;
    }
    return -1;
}

static int rm_assemble_video_frame(AVFormatContext *s, AVIOContext *pb,
                                   RMDemuxContext *rm, RMStream *vst,
                                   AVPacket *pkt, int len, int *pseq,
                                   int64_t *timestamp)
{
    int hdr, seq, pic_num, len2, pos;
    int type;

    hdr = avio_r8(pb); len--;
    type = hdr >> 6;

    if(type != 3){  // not frame as a part of packet
        seq = avio_r8(pb); len--;
    }
    if(type != 1){  // not whole frame
        len2 = get_num(pb, &len);
        pos  = get_num(pb, &len);
        pic_num = avio_r8(pb); len--;
    }
    if(len<0)
        return -1;
    rm->remaining_len = len;
    if(type&1){     // frame, not slice
        if(type == 3){  // frame as a part of packet
            len= len2;
            *timestamp = pos;
        }
        if(rm->remaining_len < len)
            return -1;
        rm->remaining_len -= len;
        if(av_new_packet(pkt, len + 9) < 0)
            return AVERROR(EIO);
        pkt->data[0] = 0;
        AV_WL32(pkt->data + 1, 1);
        AV_WL32(pkt->data + 5, 0);
        avio_read(pb, pkt->data + 9, len);
        return 0;
    }
    //now we have to deal with single slice

    *pseq = seq;
    if((seq & 0x7F) == 1 || vst->curpic_num != pic_num){
        vst->slices = ((hdr & 0x3F) << 1) + 1;
        vst->videobufsize = len2 + 8*vst->slices + 1;
        av_free_packet(&vst->pkt); //FIXME this should be output.
        if(av_new_packet(&vst->pkt, vst->videobufsize) < 0)
            return AVERROR(ENOMEM);
        vst->videobufpos = 8*vst->slices + 1;
        vst->cur_slice = 0;
        vst->curpic_num = pic_num;
        vst->pktpos = avio_tell(pb);
    }
    if(type == 2)
        len = FFMIN(len, pos);

    if(++vst->cur_slice > vst->slices)
        return 1;
    AV_WL32(vst->pkt.data - 7 + 8*vst->cur_slice, 1);
    AV_WL32(vst->pkt.data - 3 + 8*vst->cur_slice, vst->videobufpos - 8*vst->slices - 1);
    if(vst->videobufpos + len > vst->videobufsize)
        return 1;
    if (avio_read(pb, vst->pkt.data + vst->videobufpos, len) != len)
        return AVERROR(EIO);
    vst->videobufpos += len;
    rm->remaining_len-= len;

    if(type == 2 || (vst->videobufpos) == vst->videobufsize){
        vst->pkt.data[0] = vst->cur_slice-1;
        *pkt= vst->pkt;
        vst->pkt.data= NULL;
        vst->pkt.size= 0;
        if(vst->slices != vst->cur_slice) //FIXME find out how to set slices correct from the begin
            memmove(pkt->data + 1 + 8*vst->cur_slice, pkt->data + 1 + 8*vst->slices,
                vst->videobufpos - 1 - 8*vst->slices);
        pkt->size = vst->videobufpos + 8*(vst->cur_slice - vst->slices);
        pkt->pts = AV_NOPTS_VALUE;
        pkt->pos = vst->pktpos;
        vst->slices = 0;
        return 0;
    }

    return 1;
}

static inline void
rm_ac3_swap_bytes (AVStream *st, AVPacket *pkt)
{
    uint8_t *ptr;
    int j;

    if (st->codec->codec_id == CODEC_ID_AC3) {
        ptr = pkt->data;
        for (j=0;j<pkt->size;j+=2) {
            FFSWAP(int, ptr[0], ptr[1]);
            ptr += 2;
        }
    }
}

/**
 * Perform 4-bit block reordering for SIPR data.
 * @todo This can be optimized, e.g. use memcpy() if data blocks are aligned
 */
void ff_rm_reorder_sipr_data(uint8_t *buf, int sub_packet_h, int framesize)
{
    int n, bs = sub_packet_h * framesize * 2 / 96; // nibbles per subpacket

    for (n = 0; n < 38; n++) {
        int j;
        int i = bs * sipr_swaps[n][0];
        int o = bs * sipr_swaps[n][1];

        /* swap 4bit-nibbles of block 'i' with 'o' */
        for (j = 0; j < bs; j++, i++, o++) {
            int x = (buf[i >> 1] >> (4 * (i & 1))) & 0xF,
                y = (buf[o >> 1] >> (4 * (o & 1))) & 0xF;

            buf[o >> 1] = (x << (4 * (o & 1))) |
                (buf[o >> 1] & (0xF << (4 * !(o & 1))));
            buf[i >> 1] = (y << (4 * (i & 1))) |
                (buf[i >> 1] & (0xF << (4 * !(i & 1))));
        }
    }
}

int
ff_rm_parse_packet (AVFormatContext *s, AVIOContext *pb,
                    AVStream *st, RMStream *ast, int len, AVPacket *pkt,
                    int *seq, int flags, int64_t timestamp)
{
    RMDemuxContext *rm = s->priv_data;

    if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        rm->current_stream= st->id;
        if(rm_assemble_video_frame(s, pb, rm, ast, pkt, len, seq, &timestamp))
            return -1; //got partial frame
    } else if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
        if ((st->codec->codec_id == CODEC_ID_RA_288) ||
            (st->codec->codec_id == CODEC_ID_COOK) ||
            (st->codec->codec_id == CODEC_ID_ATRAC3) ||
            (st->codec->codec_id == CODEC_ID_SIPR)) {
            int x;
            int sps = ast->sub_packet_size;
            int cfs = ast->coded_framesize;
            int h = ast->sub_packet_h;
            int y = ast->sub_packet_cnt;
            int w = ast->audio_framesize;

            if (flags & 2)
                y = ast->sub_packet_cnt = 0;
            if (!y)
                ast->audiotimestamp = timestamp;

            switch(st->codec->codec_id) {
                case CODEC_ID_RA_288:
                    for (x = 0; x < h/2; x++)
                        avio_read(pb, ast->pkt.data+x*2*w+y*cfs, cfs);
                    break;
                case CODEC_ID_ATRAC3:
                case CODEC_ID_COOK:
                    for (x = 0; x < w/sps; x++)
                        avio_read(pb, ast->pkt.data+sps*(h*x+((h+1)/2)*(y&1)+(y>>1)), sps);
                    break;
                case CODEC_ID_SIPR:
                    avio_read(pb, ast->pkt.data + y * w, w);
                    break;
            }

            if (++(ast->sub_packet_cnt) < h)
                return -1;
            if (st->codec->codec_id == CODEC_ID_SIPR)
                ff_rm_reorder_sipr_data(ast->pkt.data, h, w);

             ast->sub_packet_cnt = 0;
             rm->audio_stream_num = st->index;
             rm->audio_pkt_cnt = h * w / st->codec->block_align;
        } else if (st->codec->codec_id == CODEC_ID_AAC) {
            int x;
            rm->audio_stream_num = st->index;
            ast->sub_packet_cnt = (avio_rb16(pb) & 0xf0) >> 4;
            if (ast->sub_packet_cnt) {
                for (x = 0; x < ast->sub_packet_cnt; x++)
                    ast->sub_packet_lengths[x] = avio_rb16(pb);
                rm->audio_pkt_cnt = ast->sub_packet_cnt;
                ast->audiotimestamp = timestamp;
            } else
                return -1;
        } else {
            av_get_packet(pb, pkt, len);
            rm_ac3_swap_bytes(st, pkt);
        }
    } else
        av_get_packet(pb, pkt, len);

    pkt->stream_index = st->index;

#if 0
    if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        if(st->codec->codec_id == CODEC_ID_RV20){
            int seq= 128*(pkt->data[2]&0x7F) + (pkt->data[3]>>1);
            av_log(s, AV_LOG_DEBUG, "%d %"PRId64" %d\n", *timestamp, *timestamp*512LL/25, seq);

            seq |= (timestamp&~0x3FFF);
            if(seq - timestamp >  0x2000) seq -= 0x4000;
            if(seq - timestamp < -0x2000) seq += 0x4000;
        }
    }
#endif

    pkt->pts = timestamp;
    if (flags & 2)
        pkt->flags |= AV_PKT_FLAG_KEY;

    return st->codec->codec_type == AVMEDIA_TYPE_AUDIO ? rm->audio_pkt_cnt : 0;
}

int
ff_rm_retrieve_cache (AVFormatContext *s, AVIOContext *pb,
                      AVStream *st, RMStream *ast, AVPacket *pkt)
{
    RMDemuxContext *rm = s->priv_data;

    assert (rm->audio_pkt_cnt > 0);

    if (st->codec->codec_id == CODEC_ID_AAC)
        av_get_packet(pb, pkt, ast->sub_packet_lengths[ast->sub_packet_cnt - rm->audio_pkt_cnt]);
    else {
        av_new_packet(pkt, st->codec->block_align);
        memcpy(pkt->data, ast->pkt.data + st->codec->block_align * //FIXME avoid this
               (ast->sub_packet_h * ast->audio_framesize / st->codec->block_align - rm->audio_pkt_cnt),
               st->codec->block_align);
    }
    rm->audio_pkt_cnt--;
    if ((pkt->pts = ast->audiotimestamp) != AV_NOPTS_VALUE) {
        ast->audiotimestamp = AV_NOPTS_VALUE;
        pkt->flags = AV_PKT_FLAG_KEY;
    } else
        pkt->flags = 0;
    pkt->stream_index = st->index;

    return rm->audio_pkt_cnt;
}

static int rm_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    RMDemuxContext *rm = s->priv_data;
    AVStream *st;
    int i, len, res, seq = 1;
    int64_t timestamp, pos;
    int flags;

    for (;;) {
        if (rm->audio_pkt_cnt) {
            // If there are queued audio packet return them first
            st = s->streams[rm->audio_stream_num];
            ff_rm_retrieve_cache(s, s->pb, st, st->priv_data, pkt);
            flags = 0;
        } else {
            if (rm->old_format) {
                RMStream *ast;

                st = s->streams[0];
                ast = st->priv_data;
                timestamp = AV_NOPTS_VALUE;
                len = !ast->audio_framesize ? RAW_PACKET_SIZE :
                    ast->coded_framesize * ast->sub_packet_h / 2;
                flags = (seq++ == 1) ? 2 : 0;
                pos = avio_tell(s->pb);
            } else {
                len=sync(s, &timestamp, &flags, &i, &pos);
                if (len > 0)
                    st = s->streams[i];
            }

            if(len<0 || s->pb->eof_reached)
                return AVERROR(EIO);

            res = ff_rm_parse_packet (s, s->pb, st, st->priv_data, len, pkt,
                                      &seq, flags, timestamp);
            if((flags&2) && (seq&0x7F) == 1)
                av_add_index_entry(st, pos, timestamp, 0, 0, AVINDEX_KEYFRAME);
            if (res)
                continue;
        }

        if(  (st->discard >= AVDISCARD_NONKEY && !(flags&2))
           || st->discard >= AVDISCARD_ALL){
            av_free_packet(pkt);
        } else
            break;
    }

    return 0;
}

static int rm_read_close(AVFormatContext *s)
{
    int i;

    for (i=0;i<s->nb_streams;i++)
        ff_rm_free_rmstream(s->streams[i]->priv_data);

    return 0;
}

static int rm_probe(AVProbeData *p)
{
    /* check file header */
    if ((p->buf[0] == '.' && p->buf[1] == 'R' &&
         p->buf[2] == 'M' && p->buf[3] == 'F' &&
         p->buf[4] == 0 && p->buf[5] == 0) ||
        (p->buf[0] == '.' && p->buf[1] == 'r' &&
         p->buf[2] == 'a' && p->buf[3] == 0xfd))
        return AVPROBE_SCORE_MAX;
    else
        return 0;
}

static int64_t rm_read_dts(AVFormatContext *s, int stream_index,
                               int64_t *ppos, int64_t pos_limit)
{
    RMDemuxContext *rm = s->priv_data;
    int64_t pos, dts;
    int stream_index2, flags, len, h;

    pos = *ppos;

    if(rm->old_format)
        return AV_NOPTS_VALUE;

    avio_seek(s->pb, pos, SEEK_SET);
    rm->remaining_len=0;
    for(;;){
        int seq=1;
        AVStream *st;

        len=sync(s, &dts, &flags, &stream_index2, &pos);
        if(len<0)
            return AV_NOPTS_VALUE;

        st = s->streams[stream_index2];
        if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            h= avio_r8(s->pb); len--;
            if(!(h & 0x40)){
                seq = avio_r8(s->pb); len--;
            }
        }

        if((flags&2) && (seq&0x7F) == 1){
//            av_log(s, AV_LOG_DEBUG, "%d %d-%d %"PRId64" %d\n", flags, stream_index2, stream_index, dts, seq);
            av_add_index_entry(st, pos, dts, 0, 0, AVINDEX_KEYFRAME);
            if(stream_index2 == stream_index)
                break;
        }

        avio_skip(s->pb, len);
    }
    *ppos = pos;
    return dts;
}

AVInputFormat ff_rm_demuxer = {
    .name           = "rm",
    .long_name      = NULL_IF_CONFIG_SMALL("RealMedia format"),
    .priv_data_size = sizeof(RMDemuxContext),
    .read_probe     = rm_probe,
    .read_header    = rm_read_header,
    .read_packet    = rm_read_packet,
    .read_close     = rm_read_close,
    .read_timestamp = rm_read_dts,
};

AVInputFormat ff_rdt_demuxer = {
    .name           = "rdt",
    .long_name      = NULL_IF_CONFIG_SMALL("RDT demuxer"),
    .priv_data_size = sizeof(RMDemuxContext),
    .read_close     = rm_read_close,
};
