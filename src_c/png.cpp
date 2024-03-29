#ifdef HAVE_PNG

#include "AIL.h"
#include "png.h"
#include "AIL_internal.h"
#include <vector>
#include <png.h>
#include <string.h>
#include <cstring>
#include <iostream>

namespace AImg
{
    int32_t PNGImageLoader::initialise()
    {
        return AImgErrorCode::AIMG_SUCCESS;
    }

    bool PNGImageLoader::canLoadImage(ReadCallback readCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData)
    {
        int32_t startingPosition = tellCallback(callbackData);
        std::vector<uint8_t> header(8);
        readCallback(callbackData, &header[0], 8);

        seekCallback(callbackData, startingPosition);

        uint8_t png_signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

        return ((int32_t)(memcmp(&header[0], &png_signature[0], 8))) == 0;
    }

    void png_custom_read_data(png_struct* png_ptr, png_byte* data, png_size_t length)
    {
        CallbackData callbackData = *((CallbackData *)png_get_io_ptr(png_ptr));

        callbackData.readCallback(callbackData.callbackData, data, (int32_t)length);
    }

    void png_custom_write_data(png_struct* png_ptr, png_byte* data, png_size_t length)
    {
        CallbackData callbackData = *((CallbackData *)png_get_io_ptr(png_ptr));

        callbackData.writeCallback(callbackData.callbackData, data, (int32_t)length);
    }

    std::string PNGImageLoader::getFileExtension()
    {
        return "PNG";
    }

    int32_t PNGImageLoader::getAImgFileFormatValue()
    {
        return PNG_IMAGE_FORMAT;
    }

    void flush_data_noop_func(png_struct* png_ptr)
    {
        AIL_UNUSED_PARAM(png_ptr);
    }

    bool isFormatSupportedByPng(int32_t format)
    {
        bool isNotFloatFormat = !(format & AImgFormat::FLOAT_FORMAT);
        bool is8Or16Bit = (format & AImgFormat::_8BITS || format & AImgFormat::_16BITS);
        bool isNotRG8U = format != AImgFormat::RG8U;
        bool isNotRG16U = format != AImgFormat::RG16U;
        bool isSupported = isNotFloatFormat
            && is8Or16Bit
            && isNotRG8U
            && isNotRG16U;

        return isSupported;
    }

    AImgFormat getWhatFormatWillBeWrittenForDataPNG(int32_t inputFormat, int32_t outputFormat)
    {
        if (isFormatSupportedByPng(outputFormat))
        {
            return (AImgFormat)outputFormat;
        }

        int32_t numChannels, bytesPerChannel, floatOrInt;
        AIGetFormatDetails(inputFormat, &numChannels, &bytesPerChannel, &floatOrInt);

        if (floatOrInt == AImgFloatOrIntType::FITYPE_FLOAT)
        {
            AImgFormat outFormat = (AImgFormat)(AImgFormat::_16BITS | (AImgFormat::R << (numChannels - 1))); // convert to 16U version with same channelNum

            if (outFormat == AImgFormat::RG16U)
                return AImgFormat::RGB16U;

            return outFormat;
        }

        if (inputFormat == AImgFormat::RG8U)
            return AImgFormat::RGB8U;

        if (inputFormat == AImgFormat::RG16U)
            return AImgFormat::RGB16U;

        if(inputFormat & AImgFormat::_8BITS)
            return (AImgFormat)inputFormat;

        if(inputFormat & AImgFormat::_16BITS)
            return (AImgFormat)inputFormat;

        return AImgFormat::INVALID_FORMAT;
    }

    class PNGFile : public AImgBase
    {       
        public:
            CallbackData * data = nullptr;
            png_info * png_info_ptr = nullptr;
            png_struct * png_read_ptr = nullptr;
            uint32_t width;
            uint32_t height;
            uint8_t colour_type;
            uint8_t bit_depth;
            uint8_t numChannels;
            // Convenient name for referring to the profile
            char * profileName = NULL;
            // The only compression method defined is method 0
            int32_t compressionMethod = 0;
            uint8_t * compressedProfile = NULL;
            uint32_t compressedProfileLen = 0;

            PNGFile()
            {
                data = new CallbackData();
            }

            virtual ~PNGFile()
            {
                if(data)
                    delete data;

                if(png_info_ptr)
                {
                    png_destroy_read_struct(&png_read_ptr, &png_info_ptr, (png_infopp)NULL);
                    png_destroy_info_struct(png_read_ptr, &png_info_ptr);
                }
            }

            int32_t openImage(ReadCallback readCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData)
            {
                data->readCallback = readCallback;
                data->tellCallback = tellCallback;
                data->seekCallback = seekCallback;
                data->callbackData = callbackData;

                png_read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
                png_set_option(png_read_ptr, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_OFF);
                png_info_ptr = png_create_info_struct(png_read_ptr);

                png_set_read_fn(png_read_ptr, (void *)(data), png_custom_read_data);
                png_read_info(png_read_ptr, png_info_ptr);

                width = png_get_image_width(png_read_ptr, png_info_ptr);
                height = png_get_image_height(png_read_ptr, png_info_ptr);
                bit_depth = png_get_bit_depth(png_read_ptr, png_info_ptr);
                numChannels = png_get_channels(png_read_ptr, png_info_ptr);
                colour_type = png_get_color_type(png_read_ptr, png_info_ptr);

                // see http://www.libpng.org/pub/png/book/chapter13.html (retrieved 9/Nov/2016), section 13.7
                if (colour_type == PNG_COLOR_TYPE_PALETTE)
                {
                    png_set_expand(png_read_ptr);
                    bit_depth = 8;
                    numChannels = 3;
                }
                if (colour_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
                {
                    png_set_expand(png_read_ptr);
                    bit_depth = 8;
                }

                bool hasTransparency = png_get_valid(png_read_ptr, png_info_ptr, PNG_INFO_tRNS);
                bool isGrayWithAlpha = colour_type == PNG_COLOR_TYPE_GRAY_ALPHA;

                bool numChannelsChanged = false;

                if (hasTransparency)
                {
                    png_set_expand(png_read_ptr);

                    numChannels++;
                }

                if ((hasTransparency && colour_type == PNG_COLOR_TYPE_GRAY) || isGrayWithAlpha)
                {
                    // expand grayscale images with transparency to 4-channel RGBA because if we don't then we end up
                    // with gray in R and alpha in G, and converting that up to RGBA will not yield the correct results
                    png_set_gray_to_rgb(png_read_ptr);

                    numChannels = 4;
                    numChannelsChanged = true;
                }

                if (!numChannelsChanged)
                {
                    // Don't load color profile if the number of color channels changes. It would be invalid

                    if (!(png_get_iCCP(png_read_ptr, png_info_ptr, &profileName, &compressionMethod, &compressedProfile, &compressedProfileLen) & PNG_INFO_iCCP))
                    {
                        compressedProfile = NULL;
                        profileName = NULL;
                    }
                }

                return AImgErrorCode::AIMG_SUCCESS;
            }

            virtual int32_t getImageInfo(int32_t *width, int32_t *height, int32_t *numChannels, int32_t *bytesPerChannel, int32_t *floatOrInt, int32_t *decodedImgFormat, uint32_t *colourProfileLen)
            {
                *width = this->width;
                *height = this->height;
                *numChannels = this->numChannels;
                if(colourProfileLen != NULL)
                {   
                    *colourProfileLen = this->compressedProfileLen;
                }

                if (bit_depth / 8 == 0)
                    *bytesPerChannel = -1;
                else
                    *bytesPerChannel = bit_depth/8;

                *floatOrInt = AImgFloatOrIntType::FITYPE_INT;

                *decodedImgFormat = getDecodeFormat();
                return AImgErrorCode::AIMG_SUCCESS;
            }
            
            
            virtual int32_t getColourProfile(char *profileName, uint8_t *colourProfile, uint32_t *colourProfileLen)
            {
                if(colourProfile != NULL)
                {
                    if(this->compressedProfile != NULL)
                    {
                        memcpy(colourProfile, this->compressedProfile, this->compressedProfileLen);
                    }
                    *colourProfileLen = this->compressedProfileLen;
                }
                if(profileName != NULL)
                {
                    if(this->profileName != NULL)
                    {
                        std::strcpy(profileName, this->profileName);
                    }
                }

                return AImgErrorCode::AIMG_SUCCESS;
            }

            int32_t getDecodeFormat()
            {
                if (bit_depth == 8)
                {
                    if (numChannels == 1)
                        return AImgFormat::R8U;
                    else if (numChannels == 2)
                        return AImgFormat::RG8U;
                    else if (numChannels == 3)
                        return AImgFormat::RGB8U;
                    else if (numChannels == 4)
                        return AImgFormat::RGBA8U;
                }

                else if (bit_depth == 16)
                {
                    if (numChannels == 1)
                        return AImgFormat::R16U;
                    else if (numChannels == 2)
                        return AImgFormat::RG16U;
                    else if (numChannels == 3)
                        return AImgFormat::RGB16U;
                    else if (numChannels == 4)
                        return AImgFormat::RGBA16U;
                }

                return AImgFormat::INVALID_FORMAT;

            }

            virtual int32_t decodeImage(void *realDestBuffer, int32_t forceImageFormat)
            {

                #if AIL_BYTEORDER == AIL_LIL_ENDIAN
                if (bit_depth > 8)
                   png_set_swap(png_read_ptr);
                #endif

                // This sets a restore point for libpng if reading fails internally
                // Crazy old C exceptions without exceptions
                if (setjmp(png_jmpbuf(png_read_ptr)))
                {
                    mErrorDetails = "[PNGImageLoader::PNGFile::decodeImage] Failed to read file";
                    return AImgErrorCode::AIMG_LOAD_FAILED_INTERNAL;
                }

                void* destBuffer = realDestBuffer;

                int32_t decodeFormat = getDecodeFormat();


                std::vector<uint8_t> convertTmpBuffer(0);
                if (forceImageFormat != AImgFormat::INVALID_FORMAT && forceImageFormat != decodeFormat)
                {
                    int32_t numChannels, bytesPerChannel, floatOrInt;
                    AIGetFormatDetails(decodeFormat, &numChannels, &bytesPerChannel, &floatOrInt);

                    convertTmpBuffer.resize(width * height * bytesPerChannel * numChannels);
                    destBuffer = &convertTmpBuffer[0];
                }

                std::vector<void*> ptrs(height);

                for (uint32_t y = 0; y < height; y++)
                    ptrs[y] = (void *)((size_t)destBuffer + (y*width * (bit_depth/8) * numChannels));


                png_read_image(png_read_ptr, (png_bytepp)&ptrs[0]);

                if (forceImageFormat != AImgFormat::INVALID_FORMAT && forceImageFormat != decodeFormat)
                {
                    int32_t err = AImgConvertFormat(destBuffer, realDestBuffer, width, height, decodeFormat, forceImageFormat);
                    if(err != AImgErrorCode::AIMG_SUCCESS)
                        return err;
                }

                return AImgErrorCode::AIMG_SUCCESS;
            }

            int32_t writeImage(void *data, int32_t width, int32_t height, int32_t inputFormat, int32_t outputFormat,
                const char *profileName, uint8_t *colourProfile, uint32_t colourProfileLen,
                WriteCallback writeCallback, TellCallback tellCallback, SeekCallback seekCallback, void *callbackData, void* encodingOptions)
            {
                AIL_UNUSED_PARAM(tellCallback);
                AIL_UNUSED_PARAM(seekCallback);

                png_struct * png_write_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
                png_set_option(png_write_ptr, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_OFF);
                png_info * png_info_ptr = png_create_info_struct(png_write_ptr);

                if(encodingOptions != NULL)
                {
                    PngEncodingOptions* realOptions = (PngEncodingOptions*)encodingOptions;

                    png_set_compression_level(png_write_ptr, realOptions->compressionLevel);
                    png_set_filter(png_write_ptr, 0, realOptions->filter);
                }

                CallbackData * callbackDataStruct = new CallbackData();

                callbackDataStruct->writeCallback = writeCallback;
                callbackDataStruct->callbackData = callbackData;

                png_set_write_fn(png_write_ptr, (void *)callbackDataStruct, png_custom_write_data, flush_data_noop_func);

                int32_t writeFormat = getWhatFormatWillBeWrittenForDataPNG(inputFormat, outputFormat);

                std::vector<uint8_t> convertBuffer(0);

                if (writeFormat != inputFormat)
                {
                    int32_t numChannels, bytesPerChannel, floatOrInt;
                    AIGetFormatDetails(writeFormat, &numChannels, &bytesPerChannel, &floatOrInt) ;
                    convertBuffer.resize(width * height * numChannels * bytesPerChannel);

                    int32_t convertError = AImgConvertFormat(data, &convertBuffer[0], width, height, inputFormat, writeFormat);

                    if (convertError != AImgErrorCode::AIMG_SUCCESS)
                        return convertError;
                    data = &convertBuffer[0];

                    int outChannels = numChannels;
                    AIGetFormatDetails(inputFormat, &numChannels, &bytesPerChannel, &floatOrInt);

                    if (numChannels != outChannels
                        && (numChannels != 4 || outChannels != 3)
                        && (numChannels != 3 || outChannels != 4))
                    {
                        // Don't save color profile if the number of color channels changed
                        colourProfile = NULL;
                    }
                }

                png_byte colour_type;
                png_byte bit_depth;

                switch (writeFormat)
                {
                    case R8U:
                        colour_type = PNG_COLOR_TYPE_GRAY;
                        bit_depth = 8;
                        break;
                    case RG8U:
                        colour_type = PNG_COLOR_TYPE_RGB;
                        bit_depth = 8;
                        break;
                    case RGB8U:
                        colour_type = PNG_COLOR_TYPE_RGB;
                        bit_depth = 8;
                        break;
                    case RGBA8U:
                        colour_type = PNG_COLOR_TYPE_RGB_ALPHA;
                        bit_depth = 8;
                        break;
                    case R16U:
                        colour_type = PNG_COLOR_TYPE_GRAY;
                        bit_depth = 16;
                        break;
                    case RG16U:
                        colour_type = PNG_COLOR_TYPE_RGB;
                        bit_depth = 16;
                        break;
                    case RGB16U:
                        colour_type = PNG_COLOR_TYPE_RGB;
                        bit_depth = 16;
                        break;
                    case RGBA16U:
                        colour_type = PNG_COLOR_TYPE_RGB_ALPHA;
                        bit_depth = 16;
                        break;
                }

                if (setjmp(png_jmpbuf(png_write_ptr)))
                {
                    mErrorDetails = "[AImg::PNGImageLoader::PNGFile::writeImage] Failed to write PNG header";
                    return AImgErrorCode::AIMG_WRITE_FAILED_EXTERNAL;
                }

                int32_t numChannels, bytesPerChannel, floatOrInt;
                AIGetFormatDetails(writeFormat, &numChannels, &bytesPerChannel, &floatOrInt);

                size_t step = width * numChannels * bytesPerChannel;

                png_bytepp ptrs = (png_bytepp)malloc(sizeof(png_bytep) * height);

                ptrs[0] = (png_bytep)data;
                for (int32_t y = 1; y < height; y++)
                {
                    ptrs[y] = ptrs[y - 1] + step;
                }

                png_set_IHDR(png_write_ptr, png_info_ptr, width, height, bit_depth, colour_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

                if(colourProfile != NULL)
                {
                    png_set_iCCP(png_write_ptr, png_info_ptr, profileName, 0, colourProfile, colourProfileLen);
                }
                png_write_info(png_write_ptr, png_info_ptr);

                #if AIL_BYTEORDER == AIL_LIL_ENDIAN
                if (bit_depth > 8)
                   png_set_swap(png_write_ptr);
                #endif

                if (setjmp(png_jmpbuf(png_write_ptr)))
                {
                    mErrorDetails = "[AImg::PNGImageLoader::PNGFile::writeImage] Failed to write file";
                    return AImgErrorCode::AIMG_WRITE_FAILED_EXTERNAL;
                }
                png_write_image(png_write_ptr, (png_bytepp)ptrs);

                if (setjmp(png_jmpbuf(png_write_ptr)))
                {
                    mErrorDetails = "[AImg::PNGImageLoader::PNGFile::writeImage] Failed to finalize write";
                    return AImgErrorCode::AIMG_WRITE_FAILED_EXTERNAL;
                }

                png_write_end(png_write_ptr, png_info_ptr);

                free(ptrs);
                png_destroy_write_struct(&png_write_ptr, &png_info_ptr);
                png_destroy_info_struct(png_write_ptr, &png_info_ptr);
                delete callbackDataStruct;
                return AImgErrorCode::AIMG_SUCCESS;
            }

            int32_t verifyEncodeOptions(void* encodeOptions)
            {
                if(encodeOptions != NULL)
                {
                    if(*((int*)encodeOptions) != AImgFileFormat::PNG_IMAGE_FORMAT)
                    {
                        mErrorDetails = "[AImg::PNGImageLoader::PNGFile::verifyEncodeOptions] Args for another format encoder type passed to png encoder, or incorrectly initialised args struct passed.";
                        return AImgErrorCode::AIMG_INVALID_ENCODE_ARGS;
                    }

                    auto options = (PngEncodingOptions*)encodeOptions;

                    if(options->compressionLevel < 0 || options->compressionLevel > 9)
                    {
                        mErrorDetails = "[AImg::PNGImageLoader::PNGFile::verifyEncodeOptions] Invalid compression level specified, must be in inclusive range (0-9)";
                        return AImgErrorCode::AIMG_INVALID_ENCODE_ARGS;
                    }

                    if((options->filter & PNG_ALL_FILTERS) != options->filter)
                    {
                        mErrorDetails = "[AImg::PNGImageLoader::PNGFile::verifyEncodeOptions] Invalid filter flags specified";
                        return AImgErrorCode::AIMG_INVALID_ENCODE_ARGS;
                    }
                }

                return AImgErrorCode::AIMG_SUCCESS;
            }
    };


    AImgFormat PNGImageLoader::getWhatFormatWillBeWrittenForData(int32_t inputFormat, int32_t outputFormat)
    {
        return getWhatFormatWillBeWrittenForDataPNG(inputFormat, outputFormat);
    }

    bool PNGImageLoader::isFormatSupported(int32_t format)
    {
        return isFormatSupportedByPng(format);
    }

    AImgBase* PNGImageLoader::getAImg()
    {
        return new PNGFile();
    }
}

#endif // HAVE_PNG
