import ail_py_native as native
import enums
import collections
import numpy as np
import AImgExceptions


RawFileInfo = collections.namedtuple("RawFileInfo", "numChannels bytesPerChannel floatOrInt")

class AImg(object):
    
    def __init__(self, io_or_path):
        if type(io_or_path) == str:
            self.init_from_stream(open(io_or_path), True)
        else:
            self.init_from_stream(io_or_path, False)

    def init_from_stream(self, stream, close_after_decode):
        self._stream = stream
        self._close_after_decode = close_after_decode

        self._callbackData = native.getCallbackDataFromFileLikeObject(self._stream)
        
        errCode, self._imgCapsule, detectedFileFormat = native.open(self._callbackData)
        AImgExceptions.checkErrorCode(self._imgCapsule, errCode)

        self.detectedFileFormat = enums.AImgFileFormats[detectedFileFormat]

        errCode, self.width, self.height, rawNumChannels, rawBytesPerChannel, rawFloatOrInt, decodedImgFormat, colourProfileLen = native.getInfo(self._imgCapsule, self._callbackData)
        AImgExceptions.checkErrorCode(self._imgCapsule, errCode)
        self.rawFileInfo = RawFileInfo(rawNumChannels, rawBytesPerChannel, rawFloatOrInt)
        self.decodedImgFormat = enums.AImgFormats[decodedImgFormat]
        self._decodeDone = False

        self.colourProfile = None
        if colourProfileLen != 0:
            self.colourProfile = np.zeros(colourProfileLen, dtype = np.int8, order="C")
        
        errCode, self.profileName, colourProfileLen = native.getColourProfile(self._imgCapsule, self.colourProfile, colourProfileLen)

    def decode(self, destBuffer=None, forceImageFormat=enums.AImgFormats["INVALID_FORMAT"]):
        if self._decodeDone:
            raise IOError("instance has already been decoded")

        self._decodeDone = True

        decodeFormat = self.decodedImgFormat
        if forceImageFormat != enums.AImgFormats["INVALID_FORMAT"]:
            decodeFormat = forceImageFormat

        formatInfo = enums.getFormatInfo(decodeFormat)

        if destBuffer == None:
            destBuffer = np.zeros(shape=(self.height, self.width, formatInfo.numChannels), dtype = formatInfo.npType, order="C")

        if type(destBuffer) != np.ndarray:
            raise ValueError("destBuffer must be a numpy.ndarray instance")

        if destBuffer.nbytes < self.width * self.height * formatInfo.bytesPerChannel * formatInfo.numChannels:
            raise ValueError("destBuffer is too small")

        if not (destBuffer.flags.c_contiguous and destBuffer.flags.writeable and destBuffer.flags.aligned):
            raise ValueError("destBuffer does not meet flags requirements (c_contiguous & writeable & aligned)")

        errCode = native.decode(self._imgCapsule, destBuffer, forceImageFormat.val, self._callbackData)
        AImgExceptions.checkErrorCode(self._imgCapsule, errCode)

        if self._close_after_decode:
            self._stream.close()

        self._imgCapsule = None
        self._callbackData = None

        return destBuffer


def write(io_or_path, data, fileFormat, profileName, colourProfile, encodeOptions=None):
    if not (data.flags.c_contiguous and data.flags.aligned):
            raise ValueError("data does not meet flags requirements (c_contiguous & aligned)")

    if type(io_or_path) == str:
        stream = open(io_or_path, "wb")
    else:
        stream = io_or_path

    callbackData = native.getCallbackDataFromFileLikeObject(stream)

    fmt = enums.getFormatFromNumpyArray(data)
    height, width = data.shape[0:2]

    encodeOptionsTuple = ()
    if encodeOptions:
        encodeOptionsTuple = (encodeOptions,)
    
    colourProfileLen = 0
    if colourProfile is not None:
        colourProfileLen = len(colourProfile)

    errCode, imgCapsule = native.write(fileFormat.val, data, stream, width, height, fmt.val, fmt.val, profileName, colourProfile, colourProfileLen, encodeOptionsTuple)
    AImgExceptions.checkErrorCode(imgCapsule, errCode)
