#include "ffmpegcapture.h"

#define __STDC_CONSTANT_MACROS
extern "C"
{
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
}

#include <iostream>
#include <cmath>
#include <fmt/printf.h>

#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */


// a wrapper around a single output AVStream
class OutputStream
{
 public:
    OutputStream() = default;
    ~OutputStream();

    bool init(const std::string& fn);
    bool addStream(int w, int h, int fps);
    bool openVideo();
    bool start();
    bool writeVideoFrame(bool = false);
    void finish();

 private:
    int writePacket();

    AVStream        *st    { nullptr };
    AVFrame         *frame { nullptr };
    AVCodecContext  *enc   { nullptr };
    AVFormatContext *oc    { nullptr };
    AVCodec         *vc    { nullptr };
    AVPacket        *pkt   { nullptr };

    /* pts of the next frame that will be generated */
    int64_t         next_pts { 0 };

    std::string     filename;

    bool            capturing{ false };

    friend class FFMPEGCapture;
};

bool OutputStream::init(const std::string& _filename)
{
    filename = _filename;

    /* allocate the output media context */
    avformat_alloc_output_context2(&oc, nullptr, nullptr, filename.c_str());
    return oc != nullptr;
}

#ifdef av_ts2str
#undef av_ts2str
#endif

std::string av_ts2str(int64_t ts)
{
    char s[AV_TS_MAX_STRING_SIZE];
    av_ts_make_string(s, ts);
    return s;
}

#ifdef av_ts2timestr
#undef av_ts2timestr
#endif

std::string av_ts2timestr(int64_t ts, AVRational *tb)
{
    char s[AV_TS_MAX_STRING_SIZE];
    av_ts_make_time_string(s, ts, tb);
    return s;
}

static void log_packet(const AVFormatContext *oc, const AVPacket *pkt)
{
    AVRational *time_base = &oc->streams[pkt->stream_index]->time_base;

    fmt::printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

int OutputStream::writePacket()
{
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
    pkt->stream_index = st->index;

    /* Write the compressed frame to the media file. */
    log_packet(oc, pkt);
    return av_interleaved_write_frame(oc, pkt);
}

/* Add an output stream. */
bool OutputStream::addStream(int width, int height, int frameRate)
{
    /* find the encoder */
    vc = avcodec_find_encoder(oc->oformat->video_codec);
    if (vc == nullptr)
        return false;

    st = avformat_new_stream(oc, nullptr);
    if (st == nullptr)
        return false;
    st->id = oc->nb_streams-1;

    enc = avcodec_alloc_context3(vc);
    if (enc == nullptr)
        return false;

    enc->codec_id = oc->oformat->video_codec;

    enc->bit_rate  = 400000;
    /* Resolution must be a multiple of two. */
    enc->width     = width;
    enc->height    = height;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    st->time_base  = (AVRational){ 1, frameRate };
    enc->time_base = st->time_base;

    enc->gop_size  = 12; /* emit one intra frame every twelve frames at most */
    enc->pix_fmt   = STREAM_PIX_FMT;

    if (enc->codec_id == AV_CODEC_ID_MPEG1VIDEO)
    {
        /* Needed to avoid using macroblocks in which some coeffs overflow.
        * This does not happen with normal video, it just happens here as
        * the motion of the chroma plane does not match the luma plane. */
        enc->mb_decision = 2;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return true;
}

bool OutputStream::start()
{
    av_dump_format(oc, 0, filename.c_str(), 1);

    /* open the output file, if needed */
    if ((oc->oformat->flags & AVFMT_NOFILE) == 0)
    {
        if (avio_open(&oc->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0)
            return false;
    }

    /* Write the stream header, if any. */
    if (avformat_write_header(oc, nullptr) < 0)
        return false;

    pkt = av_packet_alloc();
    if (pkt == nullptr)
        return false;

    return true;
}

bool OutputStream::openVideo()
{

    /* open the codec */
    if (avcodec_open2(enc, vc, nullptr) < 0)
        return false;

    /* allocate and init a re-usable frame */
    frame = av_frame_alloc();
    if (frame == nullptr)
        return false;

    frame->format = enc->pix_fmt;
    frame->width  = enc->width;
    frame->height = enc->height;

    /* allocate the buffers for the frame data */
    if (av_frame_get_buffer(frame, 32) < 0)
        return false;


    /* copy the stream parameters to the muxer */
    if (avcodec_parameters_from_context(st->codecpar, enc) < 0)
        return false;

    return true;
}

/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, int frame_index,
                           int width, int height)
{
    int x, y, i;

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
bool OutputStream::writeVideoFrame(bool finalize)
{
    AVFrame *frame = finalize ? nullptr : this->frame;

    /* check if we want to generate more frames */
    if (!finalize)
    {
        /* when we pass a frame to the encoder, it may keep a reference to it
        * internally; make sure we do not overwrite it here */
        if (av_frame_make_writable(frame) < 0)
            return false;

        fill_yuv_image(frame, next_pts, enc->width, enc->height);

        frame->pts = next_pts++;
    }

    av_init_packet(pkt);

    /* encode the image */
    if (avcodec_send_frame(enc, frame) < 0)
        return false;

    for (int ret = 0;;)
    {
        ret = avcodec_receive_packet(enc, pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;

        if (ret >= 0)
        {
            ret = writePacket();
            av_packet_unref(pkt);
        }

        if (ret < 0)
            return false;
    }

    return true;
}

void OutputStream::finish()
{
    writeVideoFrame(true);

    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(oc);

    if (!(oc->oformat->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&oc->pb);
}

OutputStream::~OutputStream()
{
    avcodec_free_context(&enc);
    av_frame_free(&frame);
    avformat_free_context(oc);
    av_packet_free(&pkt);
}


FFMPEGCapture::FFMPEGCapture()
{
    os = new OutputStream;
}

FFMPEGCapture::~FFMPEGCapture()
{
    delete os;
}

int FFMPEGCapture::getFrameCount() const
{
    return os->next_pts;
}

int FFMPEGCapture::getWidth() const
{
    return os->enc->width;
}

int FFMPEGCapture::getHeight() const
{
    return os->enc->height;
}

float FFMPEGCapture::getFrameRate() const
{
    return (float) os->st->time_base.num / os->st->time_base.den;
}

bool FFMPEGCapture::start(const std::string& filename, int width, int height, float fps)
{
    if (!os->init(filename))
        return false;

    if (!os->addStream(width, height, round(fps + 0.5f)))
        return false;

    if (!os->openVideo())
        return false;

    if (!os->start())
        return false;

    os->capturing = true; // XXX

    return true;
}

bool FFMPEGCapture::end()
{
    if (!os->capturing)
        return false;

    os->finish();

    return true;
}

bool FFMPEGCapture::captureFrame()
{
    if (!os->capturing)
        return false;

    if (!os->writeVideoFrame())
        return false;

    return true;
}
