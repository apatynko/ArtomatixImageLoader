#ifndef ARTOMATIX_JPEG_H
#define ARTOMATIX_JPEG_H

#include "ImageLoaderBase.h"

namespace AImg
{
    class JPEGImageLoader : public ImageLoaderBase
    {
        public:

        virtual AImgBase* getAImg();

        virtual int32_t initialise();
        virtual bool canLoadImage(ReadCallback readCallback, TellCallback tellCallback, SeekCallback seekCallback, void* callbackData);
        virtual std::string getFileExtension();
        virtual int32_t getAImgFileFormatValue();

        virtual bool isFormatSupported(int32_t format);

        virtual AImgFormat getWhatFormatWillBeWrittenForData(int32_t inputFormat, int32_t outputFormat);
    };
}

#endif //ARTOMATIX_JPEG_H
