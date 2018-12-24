#pragma once

#include "moviecapture.h"

class OutputStream;

class FFMPEGCapture : public MovieCapture
{
 public:
    FFMPEGCapture();
    ~FFMPEGCapture() override;

    bool start(const std::string&, int, int, float) override;
    bool end() override;
    bool captureFrame() override;

    int getFrameCount() const override;
    int getWidth() const override;
    int getHeight() const override;
    float getFrameRate() const override;

    void setAspectRatio(int, int) override {};
    void setQuality(float) override {};
    void recordingStatus(bool) override {};

 private:
    OutputStream *os{ nullptr };
};
