#include "ExternalOutput.h"
#include "../WebRtcConnection.h"
#include "../rtputils.h"
#include <cstdio>

#include <boost/cstdint.hpp>
#include <sys/time.h>
#include <arpa/inet.h>

namespace erizo {
#define FIR_INTERVAL_MS 4000

DEFINE_LOGGER(ExternalOutput, "media.ExternalOutput");

ExternalOutput::ExternalOutput(const std::string& outputUrl)
{
    ELOG_DEBUG("Created ExternalOutput to %s", outputUrl.c_str());

    // TODO these should really only be called once per application run
    av_register_all();
    avcodec_register_all();


    context_ = avformat_alloc_context();
    if (context_==NULL){
        ELOG_ERROR("Error allocating memory for IO context");
    } else {

        outputUrl.copy(context_->filename, sizeof(context_->filename),0);

        context_->oformat = av_guess_format(NULL,  context_->filename, NULL);
        if (!context_->oformat){
            ELOG_ERROR("Error guessing format %s", context_->filename);
        } else {
            context_->oformat->video_codec = AV_CODEC_ID_VP8;
            context_->oformat->audio_codec = AV_CODEC_ID_PCM_MULAW;
        }
    }

    videoCodec_ = NULL;
    audioCodec_ = NULL;
    video_stream_ = NULL;
    audio_stream_ = NULL;
    prevEstimatedFps_ = 0;
    warmupfpsCount_ = 0;
    writeheadres_=-1;
    unpackagedBufferpart_ = unpackagedBuffer_;
    initTime_ = 0;
    lastTime_ = 0;
    sinkfbSource_ = this;
    fbSink_ = NULL;
    gotUnpackagedFrame_ = 0;
    unpackagedSize_ = 0;
}

bool ExternalOutput::init(){
    inputProcessor_ = new InputProcessor();
    MediaInfo m;
    m.hasVideo = false;
    m.hasAudio = false;
    inputProcessor_->init(m, this);
    thread_ = boost::thread(&ExternalOutput::sendLoop, this);
    sending_ = true;
    ELOG_DEBUG("Initialized successfully");
    return true;
}


ExternalOutput::~ExternalOutput(){
    ELOG_DEBUG("ExternalOutput Destructing");

    delete inputProcessor_;
    inputProcessor_ = NULL;
    
    if (context_!=NULL){
        if (writeheadres_ >= 0)
            av_write_trailer(context_);
        if (avio_close >= 0)
            avio_close(context_->pb);
        avformat_free_context(context_);
        context_ = NULL;
    }

    if (video_stream_->codec != NULL){
        avcodec_close(video_stream_->codec);
        video_stream_->codec=NULL;
    }

    if (audio_stream_->codec != NULL){
        avcodec_close(audio_stream_->codec);
        audio_stream_->codec = NULL;
    }

    sending_ = false;
    cond_.notify_one();
    thread_.join();
    ELOG_DEBUG("ExternalOutput closed Successfully");
}

void ExternalOutput::receiveRawData(RawDataPacket& /*packet*/){
    return;
}


int ExternalOutput::writeAudioData(char* buf, int len){
    if (inputProcessor_!=NULL){
        if (videoCodec_ == NULL) {
            return 0;
        }
        rtpheader *head = (rtpheader*)buf;
        //We dont need any other payload at this time
        if(head->payloadtype != PCMU_8000_PT){
            return 0;
        }

        int ret = inputProcessor_->unpackageAudio(reinterpret_cast<unsigned char*>(buf), len,
                                                  unpackagedAudioBuffer_);
        if (ret <= 0)
            return ret;
        timeval time;
        gettimeofday(&time, NULL);
        unsigned long long millis = (time.tv_sec * 1000) + (time.tv_usec / 1000);
        if (millis -lastTime_ >FIR_INTERVAL_MS){
            this->sendFirPacket();
            lastTime_ = millis;
        }
        if (initTime_ == 0) {
            initTime_ = millis;
        }
        if (millis < initTime_){
            ELOG_WARN("initTime is smaller than currentTime, possible problems when recording ");
        }
        if (ret > UNPACKAGE_BUFFER_SIZE){
            ELOG_ERROR("Unpackaged Audio size too big %d", ret);
        }
        AVPacket avpkt;
        av_init_packet(&avpkt);
        avpkt.data = unpackagedAudioBuffer_;
        avpkt.size = ret;
        avpkt.pts = millis - initTime_;
        avpkt.stream_index = 1;
        av_write_frame(context_, &avpkt);
        av_free_packet(&avpkt);
        return ret;

    }
    return 0;
}

int ExternalOutput::writeVideoData(char* buf, int len){
    if (inputProcessor_!=NULL){
        rtpheader *head = (rtpheader*) buf;
        if (head->payloadtype == RED_90000_PT) {
            int totalLength = 12;

            if (head->extension) {
                totalLength += ntohs(head->extensionlength)*4 + 4; // RTP Extension header
            }
            int rtpHeaderLength = totalLength;
            redheader *redhead = (redheader*) (buf + totalLength);

            //redhead->payloadtype = remoteSdp_.inOutPTMap[redhead->payloadtype];
            if (redhead->payloadtype == VP8_90000_PT) {
                while (redhead->follow) {
                    totalLength += redhead->getLength() + 4; // RED header
                    redhead = (redheader*) (buf + totalLength);
                }
                // Parse RED packet to VP8 packet.
                // Copy RTP header
                memcpy(deliverMediaBuffer_, buf, rtpHeaderLength);
                // Copy payload data
                memcpy(deliverMediaBuffer_ + totalLength, buf + totalLength + 1, len - totalLength - 1);
                // Copy payload type
                rtpheader *mediahead = (rtpheader*) deliverMediaBuffer_;
                mediahead->payloadtype = redhead->payloadtype;
                buf = reinterpret_cast<char*>(deliverMediaBuffer_);
                len = len - 1 - totalLength + rtpHeaderLength;
            }
        }
        int estimatedFps=0;
        int ret = inputProcessor_->unpackageVideo(reinterpret_cast<unsigned char*>(buf), len,
                                                  unpackagedBufferpart_, &gotUnpackagedFrame_, &estimatedFps);

        if (ret < 0)
            return 0;

        if (videoCodec_ == NULL) {
            if ((estimatedFps!=0)&&((estimatedFps < prevEstimatedFps_*(1-0.2))||(estimatedFps > prevEstimatedFps_*(1+0.2)))){
                prevEstimatedFps_ = estimatedFps;
            }
            if (warmupfpsCount_++ == 20){
                if (prevEstimatedFps_==0){
                    warmupfpsCount_ = 0;
                    return 0;
                }
                if (!this->initContext()){
                    ELOG_ERROR("Context cannot be initialized properly, closing...");
                    return -1;
                }
            }
            return 0;
        }

        unpackagedSize_ += ret;
        unpackagedBufferpart_ += ret;
        if (unpackagedSize_ > UNPACKAGE_BUFFER_SIZE){
            ELOG_ERROR("Unpackaged size bigget than buffer %d", unpackagedSize_);
        }
        if (gotUnpackagedFrame_ && videoCodec_ != NULL) {
            timeval time;
            gettimeofday(&time, NULL);
            unsigned long long millis = (time.tv_sec * 1000) + (time.tv_usec / 1000);
            if (initTime_ == 0) {
                initTime_ = millis;
            }
            if (millis < initTime_)
            {
                ELOG_WARN("initTime is smaller than currentTime, possible problems when recording ");
            }
            unpackagedBufferpart_ -= unpackagedSize_;

            AVPacket avpkt;
            av_init_packet(&avpkt);
            avpkt.data = unpackagedBufferpart_;
            avpkt.size = unpackagedSize_;
            avpkt.pts = millis - initTime_;
            avpkt.stream_index = 0;
            av_write_frame(context_, &avpkt);
            av_free_packet(&avpkt);
            gotUnpackagedFrame_ = 0;
            unpackagedSize_ = 0;
            unpackagedBufferpart_ = unpackagedBuffer_;

        }
    }
    return 0;
}

int ExternalOutput::deliverAudioData_(char* buf, int len) {
    rtcpheader *head = reinterpret_cast<rtcpheader*>(buf);
    if (head->isRtcp()){
        return 0;
    }
    this->queueData(buf,len,AUDIO_PACKET);
    return 0;
}

int ExternalOutput::deliverVideoData_(char* buf, int len) {
    rtcpheader *head = reinterpret_cast<rtcpheader*>(buf);
    if (head->isRtcp()){
        return 0;
    }
    this->queueData(buf,len,VIDEO_PACKET);
    return 0;
}


bool ExternalOutput::initContext() {
    ELOG_DEBUG("Init Context");
    if (context_->oformat->video_codec != AV_CODEC_ID_NONE && videoCodec_ == NULL) {
        videoCodec_ = avcodec_find_encoder(context_->oformat->video_codec);
        ELOG_DEBUG("Found Codec %s", videoCodec_->name);
        ELOG_DEBUG("Initing context with fps: %d", (int)prevEstimatedFps_);
        if (videoCodec_==NULL){
            ELOG_ERROR("Could not find codec");
            return false;
        }
        video_stream_ = avformat_new_stream (context_, videoCodec_);
        video_stream_->id = 0;
        video_stream_->codec->codec_id = context_->oformat->video_codec;
        video_stream_->codec->width = 640;
        video_stream_->codec->height = 480;
        video_stream_->codec->time_base = (AVRational){1,(int)prevEstimatedFps_};
    video_stream_->codec->pix_fmt = PIX_FMT_YUV420P;
    if (context_->oformat->flags & AVFMT_GLOBALHEADER){
        video_stream_->codec->flags|=CODEC_FLAG_GLOBAL_HEADER;
    }
    context_->oformat->flags |= AVFMT_VARIABLE_FPS;
    ELOG_DEBUG("Init audio context");

    audioCodec_ = avcodec_find_encoder(context_->oformat->audio_codec);
    if (audioCodec_==NULL){
        ELOG_ERROR("Could not find audio codec");
        return false;
    }
    ELOG_DEBUG("Found Audio Codec %s", audioCodec_->name);
    audio_stream_ = avformat_new_stream (context_, audioCodec_);
    audio_stream_->id = 1;
    audio_stream_->codec->codec_id = context_->oformat->audio_codec;
    audio_stream_->codec->sample_rate = 8000;
    audio_stream_->codec->channels = 1;
    //      audioCodecCtx_->sample_fmt = AV_SAMPLE_FMT_S8;
    if (context_->oformat->flags & AVFMT_GLOBALHEADER){
        audio_stream_->codec->flags|=CODEC_FLAG_GLOBAL_HEADER;
    }

    context_->streams[0] = video_stream_;
    context_->streams[1] = audio_stream_;
    if (avio_open(&context_->pb, context_->filename, AVIO_FLAG_WRITE) < 0){
        ELOG_ERROR("Error opening output file");
        return false;
    }
    writeheadres_ = avformat_write_header(context_, NULL);
    if (writeheadres_<0){
        ELOG_ERROR("Error writing header");
        return false;
    }
    ELOG_DEBUG("AVFORMAT CONFIGURED");
}
return true;
}

void ExternalOutput::queueData(char* buffer, int length, packetType type){
    if (inputProcessor_==NULL) {
        return;
    }
    boost::mutex::scoped_lock lock(queueMutex_);
    if (type == VIDEO_PACKET){
        videoQueue_.pushPacket(buffer, length);
    }else{
        audioQueue_.pushPacket(buffer, length);
    }
    cond_.notify_one();
    
}

int ExternalOutput::sendFirPacket() {
    if (fbSink_ != NULL) {
        int pos = 0;
        uint8_t rtcpPacket[50];
        // add full intra request indicator
        uint8_t FMT = 4;
        rtcpPacket[pos++] = (uint8_t) 0x80 + FMT;
        rtcpPacket[pos++] = (uint8_t) 206;
        pos = 12;
        fbSink_->deliverFeedback((char*)rtcpPacket, pos);
        return pos;
    }

    return -1;
}

void ExternalOutput::sendLoop() {
    while (sending_ == true) {
        boost::unique_lock<boost::mutex> lock(queueMutex_);
        while ((audioQueue_.getSize() < 15)&&(videoQueue_.getSize() < 15)) {
            cond_.wait(lock);
            if (sending_ == false) {
                lock.unlock();
                return;
            }
        }
        if (audioQueue_.getSize()){
            boost::shared_ptr<dataPacket> audioP = audioQueue_.popPacket();
            this->writeAudioData(audioP->data, audioP->length);
        }
        if (videoQueue_.getSize()) {
            boost::shared_ptr<dataPacket> videoP = videoQueue_.popPacket();
            this->writeVideoData(videoP->data, videoP->length);

        }

        lock.unlock();
    }
}
}

