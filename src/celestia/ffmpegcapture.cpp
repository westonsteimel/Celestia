#include "ffmpegcapture.h"

#define __STDC_CONSTANT_MACROS
extern "C"
{
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <iostream>
#include <GL/glew.h>
#include <fmt/printf.h>


using namespace std;

// a wrapper around a single output AVStream
class OutputStream
{
 public:
    OutputStream() = default;
    ~OutputStream();

    bool init(const std::string& fn);
    bool addStream(int w, int h, float fps);
    bool openVideo();
    bool start();
    bool writeVideoFrame(bool = false);
    void finish();

 private:
    int writePacket();

    AVStream        *st       { nullptr };
    AVFrame         *frame    { nullptr };
    AVFrame         *tmpfr    { nullptr };
    AVCodecContext  *enc      { nullptr };
    AVFormatContext *oc       { nullptr };
    AVCodec         *vc       { nullptr };
    AVPacket        *pkt      { nullptr };
    SwsContext      *swsc     { nullptr };

    /* pts of the next frame that will be generated */
    int64_t         next_pts  { 0 };

    std::string     filename;
    float           fps       { 0 } ;
    bool            capturing { false };

 public:
#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)) // ffmpeg < 4.0
    static bool     registered;
#endif
    friend class FFMPEGCapture;
};

#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)) // ffmpeg < 4.0
bool OutputStream::registered = false;
#endif

bool OutputStream::init(const std::string& _filename)
{
    filename = _filename;

#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)) // ffmpeg < 4.0
    if (!OutputStream::registered)
    {
        av_register_all();
        OutputStream::registered = true;
    }
#endif

    /* allocate the output media context */
    avformat_alloc_output_context2(&oc, nullptr, nullptr, filename.c_str());
    if (oc == nullptr)
        avformat_alloc_output_context2(&oc, nullptr, "mpeg", filename.c_str());

    if (oc != nullptr)
        fmt::printf("Format codec: %s\n", oc->oformat->long_name);

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
bool OutputStream::addStream(int width, int height, float fps)
{
    this->fps = fps;

    /* find the encoder */
    vc = avcodec_find_encoder(oc->oformat->video_codec);
    if (vc == nullptr)
    {
        cout << "Video codec isn't found\n";
        return false;
    }

    st = avformat_new_stream(oc, nullptr);
    if (st == nullptr)
    {
        cout << "Unable to alloc a new stream\n";
        return false;
    }
    st->id = oc->nb_streams-1;

    enc = avcodec_alloc_context3(vc);
    if (enc == nullptr)
    {
        cout << "Unable to alloc a new context\n";
        return false;
    }

    enc->codec_id = oc->oformat->video_codec; // TODO: make selectable

    enc->bit_rate  = 400000; // TODO: make selectable
    /* Resolution must be a multiple of two. */
    enc->width     = width;
    enc->height    = height;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    if (fabs(fps - (29.97)) < 1e-5)
        st->time_base = { 100, 2997 };
    else if (fabs(fps - (23.97)) < 1e-5)
        st->time_base = { 100, 2397 };
    else
        st->time_base = { 1, (int) fps };

    enc->time_base = st->time_base;
    enc->framerate = { st->time_base.den, st->time_base.num };
    enc->gop_size  = 12; /* emit one intra frame every twelve frames at most */
    enc->pix_fmt   = AV_PIX_FMT_YUV420P; // FIXME

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
        {
            cout << "File open error\n";
            return false;
        }
    }

    /* Write the stream header, if any. */
    if (avformat_write_header(oc, nullptr) < 0)
    {
        cout << "Failed to write header\n";
        return false;
    }

    if ((pkt = av_packet_alloc()) == nullptr)
    {
        cout << "Failed to allocate a packet\n";
        return false;
    }

    return true;
}

bool OutputStream::openVideo()
{

    /* open the codec */
    if (avcodec_open2(enc, vc, nullptr) < 0)
    {
        cout << "Failed to open the codec\n";
        return false;
    }

    /* allocate and init a re-usable frame */
    if ((frame = av_frame_alloc()) == nullptr)
    {
        cout << "Failed to allocate dest frame\n";
        return false;
    }

    frame->format = enc->pix_fmt;
    frame->width  = enc->width;
    frame->height = enc->height;

    /* allocate the buffers for the frame data */
    if (av_frame_get_buffer(frame, 32) < 0)
    {
        cout << "Failed to allocate dest frame buffer\n";
        return false;
    }

    if (enc->pix_fmt != AV_PIX_FMT_RGB24)
    {
        // as we only grab a RGB24 picture, we must convert it
        // to the codec pixel format if needed
        swsc = sws_getContext(enc->width, enc->height, AV_PIX_FMT_RGB24,
                              enc->width, enc->height, enc->pix_fmt,
                              SWS_BITEXACT, nullptr, nullptr, nullptr);
        if (swsc == nullptr)
        {
            cout << "Failed to allocate SWS context\n";
            return false;
        }

        /* allocate and init a temporary frame */
        if((tmpfr = av_frame_alloc()) == nullptr)
        {
            cout << "Failed to allocate temp frame\n";
            return false;
        }

        tmpfr->format = AV_PIX_FMT_RGB24;
        tmpfr->width  = enc->width;
        tmpfr->height = enc->height;

        /* allocate the buffers for the frame data */
        if (av_frame_get_buffer(tmpfr, 32) < 0)
        {
            cout << "Failed to allocate temp frame buffer\n";
            return false;
        }
    }

    /* copy the stream parameters to the muxer */
    if (avcodec_parameters_from_context(st->codecpar, enc) < 0)
    {
        cout << "Failed to copy the stream parameters to the muxer";
        return false;
    }

    return true;
}

static void captureImage(AVFrame *pict, int width, int height)
{
    // Get the dimensions of the current viewport
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    int x = viewport[0] + (viewport[2] - width) / 2;
    int y = viewport[1] + (viewport[3] - height) / 2;
    glReadPixels(x, y, width, height,
                 GL_RGB, GL_UNSIGNED_BYTE,
                 pict->data[0]);

    // Read image is vertically flipped
    int realWidth = width * 3; // 3 bytes per pixel
    uint8_t tempLine[realWidth];
    uint8_t *fb = pict->data[0];
    for (int i = 0, p = realWidth * (height - 1); i < p; i += realWidth, p -= realWidth)
    {
        memcpy(tempLine, &fb[i],   realWidth);
        memcpy(&fb[i],   &fb[p],   realWidth);
        memcpy(&fb[p],   tempLine, realWidth);
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
        // when we pass a frame to the encoder, it may keep a reference to it
        // internally; make sure we do not overwrite it here
        if (av_frame_make_writable(frame) < 0)
        {
            cout << "Failed to make the frame writable\n";
            return false;
        }

        if (enc->pix_fmt != AV_PIX_FMT_RGB24)
        {
            captureImage(tmpfr, enc->width, enc->height);

            sws_scale(swsc, tmpfr->data, tmpfr->linesize, 0, enc->height,
                      frame->data, frame->linesize);
        }
        else
        {
            captureImage(frame, enc->width, enc->height);
        }

        frame->pts = next_pts++;
    }

    av_init_packet(pkt);

    /* encode the image */
    if (avcodec_send_frame(enc, frame) < 0)
    {
        cout << "Failed to send the frame\n";
        return false;
    }

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
        {
            cout << "Failed to receive/unref the packet\n";
            return false;
        }
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
    if (tmpfr != nullptr)
        av_frame_free(&tmpfr);
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
    return os->fps;
}

bool FFMPEGCapture::start(const std::string& filename, int width, int height, float fps)
{
    if (!os->init(filename))
        return false;

    if (!os->addStream(width, height, fps))
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

    os->capturing = false;

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
