#ifdef HAVE_EXR

#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfChannelList.h>
#include <ImathBox.h>
#include <ImfIO.h>

#include <stdint.h>
#include <vector>
#include <cstring>
#include <exception>
#include <algorithm>

#include "AIL.h"
#include "AIL_internal.h"
#include "exr.h"

namespace AImg
{
    class CallbackIStream : public Imf::IStream
    {
    public:
        CallbackIStream(ReadCallback readCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData) : IStream("")
        {
            mReadCallback = readCallback;
            mTellCallback = tellCallback;
            mSeekCallback = seekCallback;
            mCallbackData = callbackData;
        }

        virtual bool read(char c[], int n)
        {
            return mReadCallback(mCallbackData, (uint8_t *)c, n) == n;
        }

        virtual uint64_t tellg()
        {
            return mTellCallback(mCallbackData);
        }

        virtual void seekg(uint64_t pos)
        {
            mSeekCallback(mCallbackData, (int32_t)pos);
        }

        virtual void clear()
        {
        }

        ReadCallback mReadCallback;
        TellCallback mTellCallback;
        SeekCallback mSeekCallback;
        void *mCallbackData;
    };

    class CallbackOStream : public Imf::OStream
    {
    public:
        CallbackOStream(WriteCallback writeCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData) : OStream("")
        {
            mWriteCallback = writeCallback;
            mTellCallback = tellCallback;
            mSeekCallback = seekCallback;
            mCallbackData = callbackData;
        }

        virtual void write(const char c[], int n)
        {
            mWriteCallback(mCallbackData, (const uint8_t *)c, n);
        }

        virtual uint64_t tellp()
        {
            return mTellCallback(mCallbackData);
        }

        virtual void seekp(uint64_t pos)
        {
            mSeekCallback(mCallbackData, (int32_t)pos);
        }

        virtual void clear()
        {
        }

        WriteCallback mWriteCallback;
        TellCallback mTellCallback;
        SeekCallback mSeekCallback;
        void *mCallbackData;
    };

    int32_t ExrImageLoader::initialise()
    {
        try
        {
            Imf::staticInitialize();

            return AImgErrorCode::AIMG_SUCCESS;
        }
        catch (const std::exception)
        {
            return AImgErrorCode::AIMG_LOAD_FAILED_INTERNAL;
        }
    }

    bool ExrImageLoader::canLoadImage(ReadCallback readCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData)
    {
        int32_t startingPos = tellCallback(callbackData);

        std::vector<uint8_t> header(4);
        readCallback(callbackData, &header[0], 4);

        seekCallback(callbackData, startingPos);

        return header[0] == 0x76 && header[1] == 0x2f && header[2] == 0x31 && header[3] == 0x01;
    }

    std::string ExrImageLoader::getFileExtension()
    {
        return "EXR";
    }

    int32_t ExrImageLoader::getAImgFileFormatValue()
    {
        return EXR_IMAGE_FORMAT;
    }

    bool isFormatSupportedByExr(int32_t format)
    {
        int32_t flags = format & AImgFormat::_16BITS;
        int32_t fl = format & AImgFormat::_32BITS;
        int32_t floatF = format & AImgFormat::FLOAT_FORMAT;

        bool isSupported = (fl || flags) && floatF;

        return isSupported;
    }

    AImgFormat getWriteFormatExr(int32_t inputFormat, int32_t outputFormat)
    {
        if (outputFormat != inputFormat
            && outputFormat != AImgFormat::INVALID_FORMAT
            && isFormatSupportedByExr(outputFormat))
        {
            return (AImgFormat)outputFormat;
        }

        if (!isFormatSupportedByExr(inputFormat))
        {
            // set inputBufFormat to the format with the same number of channels as inputFormat, but is 32F
            int32_t bytesPerChannelTmp, numChannelsTmp, floatOrIntTmp;
            AIGetFormatDetails(inputFormat, &numChannelsTmp, &bytesPerChannelTmp, &floatOrIntTmp);

            AImgFormat res;

            if (bytesPerChannelTmp > 2)
                res = AImgFormat::_32BITS;
            else
                res = AImgFormat::_16BITS;

            return (AImgFormat)(res | AImgFormat::FLOAT_FORMAT | (1 << numChannelsTmp));
        }

        return (AImgFormat)inputFormat;
    }

    class ExrFile : public AImgBase
    {
    public:
        CallbackIStream *data = nullptr;
        Imf::InputFile *file = nullptr;
        Imath::Box2i dw;

        virtual ~ExrFile()
        {
            if (data)
                delete data;
            if (file)
                delete file;
        }

        int32_t getDecodeFormat()
        {
            bool useHalfFloat = true;

            // yes, I am actually pretty sure this is the only way to get a channel count...
            int32_t channelNum = 0;
            const Imf::ChannelList &channels = file->header().channels();
            for (Imf::ChannelList::ConstIterator it = channels.begin(); it != channels.end(); ++it)
            {
                channelNum++;

                // If any channels are UINT or FLOAT, then decode the whole thing as FLOAT
                // Otherwise, if everything is HALF, then we can decode as HALF
                if (it.channel().type != Imf::PixelType::HALF)
                    useHalfFloat = false;
            }

            if (channelNum <= 0)
                return AImgFormat::INVALID_FORMAT;

            // start at the 1-channel version + offset to get the correct channel count format
            int32_t format = (useHalfFloat ? AImgFormat::_16BITS : AImgFormat::_32BITS) | AImgFormat::FLOAT_FORMAT;
            format |= (AImgFormat::R << (std::min(channelNum, 4) - 1)); // min because we just truncate channels if we have >4 (exrs can have any number of channels)

            return format;
        }

        virtual int32_t getImageInfo(int32_t *width, int32_t *height, int32_t *numChannels, int32_t *bytesPerChannel, int32_t *floatOrInt, int32_t *decodedImgFormat, uint32_t *colourProfileLen)
        {
            *width = dw.max.x - dw.min.x + 1;
            *height = dw.max.y - dw.min.y + 1;
            *decodedImgFormat = getDecodeFormat();
            if (colourProfileLen != NULL)
            {
                *colourProfileLen = 0;
            }

            *numChannels = 0;

            Imf::PixelType lastChannelType;
            bool allChannelsSame = true;

            bool isFirstChannel = true;

            const Imf::ChannelList &channels = file->header().channels();
            for (Imf::ChannelList::ConstIterator it = channels.begin(); it != channels.end(); ++it)
            {
                (*numChannels)++;

                if (isFirstChannel)
                {
                    isFirstChannel = false;
                    lastChannelType = it.channel().type;
                }

                if (it.channel().type != lastChannelType)
                    allChannelsSame = false;
            }

            if (!allChannelsSame)
            {
                *bytesPerChannel = -1;
                *floatOrInt = AImgFloatOrIntType::FITYPE_UNKNOWN;
            }
            else
            {
                if (lastChannelType == Imf::PixelType::UINT)
                {
                    *bytesPerChannel = 4;
                    *floatOrInt = AImgFloatOrIntType::FITYPE_INT;
                }
                if (lastChannelType == Imf::PixelType::FLOAT)
                {
                    *bytesPerChannel = 4;
                    *floatOrInt = AImgFloatOrIntType::FITYPE_FLOAT;
                }
                else if (lastChannelType == Imf::PixelType::HALF)
                {
                    *bytesPerChannel = 2;
                    *floatOrInt = AImgFloatOrIntType::FITYPE_FLOAT;
                }
                else
                {
                    mErrorDetails = "[AImg::EXRImageLoader::EXRFile::] Invalid channel type in exr file";
                    return AImgErrorCode::AIMG_LOAD_FAILED_INTERNAL;
                }
            }

            return AImgErrorCode::AIMG_SUCCESS;
        }

        virtual int32_t getColourProfile(char *profileName, uint8_t *colourProfile, uint32_t *colourProfileLen)
        {
            if (colourProfile != NULL)
            {
                *colourProfileLen = 0;
            }
            if (profileName != NULL)
            {
                std::strcpy(profileName, "no_profile");
            }

            return AImgErrorCode::AIMG_SUCCESS;
        }

        virtual int32_t decodeImage(void *realDestBuffer, int32_t forceImageFormat)
        {
            try
            {
                int32_t width = dw.max.x - dw.min.x + 1;
                int32_t height = dw.max.y - dw.min.y + 1;

                int32_t decodeFormat = getDecodeFormat();

                int32_t decodeFormatNumChannels, decodeFormatBytesPerChannel, decodeFormatFloatOrInt;
                AIGetFormatDetails(decodeFormat, &decodeFormatNumChannels, &decodeFormatBytesPerChannel, &decodeFormatFloatOrInt);

                if (decodeFormatBytesPerChannel != 2 && decodeFormatBytesPerChannel != 4)
                {
                    mErrorDetails = "[AImg::EXRImageLoader::EXRFile::] invalid decodeFormatBytesPerChannel";
                    return AImgErrorCode::AIMG_LOAD_FAILED_INTERNAL;
                }

                char *destBuffer = (char *)realDestBuffer;

                std::vector<uint8_t> convertTmpBuffer(0);
                if (forceImageFormat != AImgFormat::INVALID_FORMAT && forceImageFormat != decodeFormat)
                {
                    convertTmpBuffer.resize(width * height * decodeFormatBytesPerChannel * decodeFormatNumChannels);
                    destBuffer = (char *)convertTmpBuffer.data();
                }

                std::vector<std::string> allChannelNames;
                bool isRgba = true;

                const Imf::ChannelList &channels = file->header().channels();
                for (Imf::ChannelList::ConstIterator it = channels.begin(); it != channels.end(); ++it)
                {
                    std::string name = it.name();
                    allChannelNames.push_back(it.name());
                    if (name != "R" && name != "G" && name != "B" && name != "A")
                        isRgba = false;
                }

                std::vector<std::string> usedChannelNames;

                // ensure RGBA byte order, when loading an rgba image
                if (isRgba)
                {
                    if (std::find(allChannelNames.begin(), allChannelNames.end(), "R") != allChannelNames.end())
                        usedChannelNames.push_back("R");
                    if (std::find(allChannelNames.begin(), allChannelNames.end(), "G") != allChannelNames.end())
                        usedChannelNames.push_back("G");
                    if (std::find(allChannelNames.begin(), allChannelNames.end(), "B") != allChannelNames.end())
                        usedChannelNames.push_back("B");
                    if (std::find(allChannelNames.begin(), allChannelNames.end(), "A") != allChannelNames.end())
                        usedChannelNames.push_back("A");
                }
                // otherwise just whack em in in order
                else
                {
                    for (uint32_t i = 0; i < allChannelNames.size(); i++)
                    {
                        if (usedChannelNames.size() >= 4)
                            break;

                        if (std::find(usedChannelNames.begin(), usedChannelNames.end(), allChannelNames[i]) == usedChannelNames.end())
                            usedChannelNames.push_back(allChannelNames[i]);
                    }
                }

                Imf::FrameBuffer frameBuffer;
                auto displayWindow = file->header().displayWindow();

                auto fbMaxW = std::max(dw.max.x, displayWindow.max.x) + 1;

                auto channelType = decodeFormatBytesPerChannel == 4 ? Imf::FLOAT : Imf::HALF;
                for (uint32_t i = 0; i < usedChannelNames.size(); i++)
                {
                    auto slice = Imf::Slice(channelType,
                        destBuffer + i * decodeFormatBytesPerChannel,
                        usedChannelNames.size() * decodeFormatBytesPerChannel,
                        fbMaxW * usedChannelNames.size() * decodeFormatBytesPerChannel,
                        1,
                        1,
                        0.0);

                    frameBuffer.insert(usedChannelNames[i], slice);
                }

                file->setFrameBuffer(frameBuffer);
                auto dataWindow = file->header().dataWindow();
                file->readPixels(dataWindow.min.y, dataWindow.max.y);

                if (forceImageFormat != AImgFormat::INVALID_FORMAT && forceImageFormat != decodeFormat)
                {
                    int32_t err = AImgConvertFormat(destBuffer, realDestBuffer, width, height, decodeFormat, forceImageFormat);
                    if (err != AImgErrorCode::AIMG_SUCCESS)
                        return err;
                }

                return AImgErrorCode::AIMG_SUCCESS;
            }
            catch (const std::exception &e)
            {
                mErrorDetails = std::string("[AImg::EXRImageLoader::EXRFile::] ") + e.what();
                return AImgErrorCode::AIMG_LOAD_FAILED_INTERNAL;
            }
        }

        virtual int32_t openImage(ReadCallback readCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData)
        {
            try
            {
                data = new CallbackIStream(readCallback, tellCallback, seekCallback, callbackData);
                file = new Imf::InputFile(*data);
                dw = file->header().displayWindow();
                auto header = file->header();

                return AImgErrorCode::AIMG_SUCCESS;
            }
            catch (const std::exception &e)
            {
                mErrorDetails = std::string("[AImg::EXRImageLoader::EXRFile::] ") + e.what();
                return AImgErrorCode::AIMG_LOAD_FAILED_EXTERNAL;
            }
        }

        int32_t writeImage(void *data, int32_t width, int32_t height, int32_t inputFormat, int32_t outputFormat, const char *profileName, uint8_t *colourProfile, uint32_t colourProfileLen,
            WriteCallback writeCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData, void *encodingOptions)
        {
            AIL_UNUSED_PARAM(encodingOptions);

            try
            {
                std::vector<uint8_t> reformattedDataTmp(0);

                void *inputBuf = data;
                AImgFormat inputBufFormat = getWriteFormatExr(inputFormat, outputFormat);

                bool needConversion = inputBufFormat != inputFormat;

                // need 32F or 16F data, so convert if necessary
                if (needConversion)
                {
                    // resize reformattedDataTmp to fit the converted image data
                    int32_t bytesPerChannelTmp, numChannelsTmp, floatOrIntTmp;
                    AIGetFormatDetails(inputBufFormat, &numChannelsTmp, &bytesPerChannelTmp, &floatOrIntTmp);
                    reformattedDataTmp.resize(numChannelsTmp * bytesPerChannelTmp * width * height);

                    AImgConvertFormat(data, &reformattedDataTmp[0], width, height, inputFormat, inputBufFormat);
                    inputBuf = &reformattedDataTmp[0];
                }

                int32_t bytesPerChannel, numChannels, floatOrInt;
                AIGetFormatDetails(inputBufFormat, &numChannels, &bytesPerChannel, &floatOrInt);

                const char *RGBAChannelNames[] = { "R", "G", "B", "A" };
                const char *GreyScaleChannelName = "Y";

                Imf::Header header(width, height);

                for (int32_t i = 0; i < numChannels; i++)
                {
                    const char *channelName;
                    if (numChannels == 1)
                        channelName = GreyScaleChannelName;
                    else
                        channelName = RGBAChannelNames[i];

                    header.channels().insert(channelName, Imf::Channel((bytesPerChannel == 4) ? Imf::FLOAT : Imf::HALF));
                }

                Imf::FrameBuffer frameBuffer;

                for (int32_t i = 0; i < numChannels; i++)
                {
                    const char *channelName;
                    if (numChannels == 1)
                        channelName = GreyScaleChannelName;
                    else
                        channelName = RGBAChannelNames[i];

                    frameBuffer.insert(
                        channelName,
                        Imf::Slice(
                        (bytesPerChannel == 4) ? Imf::FLOAT : Imf::HALF,
                            &((char *)inputBuf)[bytesPerChannel * i],
                            bytesPerChannel * numChannels,
                            bytesPerChannel * width * numChannels,
                            1, 1,
                            0.0));
                }

                CallbackOStream ostream(writeCallback, tellCallback, seekCallback, callbackData);
                Imf::OutputFile file(ostream, header);
                file.setFrameBuffer(frameBuffer);
                file.writePixels(height);

                return AImgErrorCode::AIMG_SUCCESS;
            }
            catch (const std::exception &e)
            {
                mErrorDetails = std::string("[AImg::EXRImageLoader::EXRFile::] ") + e.what();
                return AImgErrorCode::AIMG_WRITE_FAILED_EXTERNAL;
            }
        }
    };

    AImgBase *ExrImageLoader::getAImg()
    {
        return new ExrFile();
    }

    bool ExrImageLoader::isFormatSupported(int32_t format)
    {
        return isFormatSupportedByExr(format);
    }

    AImgFormat ExrImageLoader::getWhatFormatWillBeWrittenForData(int32_t inputFormat, int32_t outputFormat)
    {
        return getWriteFormatExr(inputFormat, outputFormat);
    }
}

#endif // HAVE_EXR