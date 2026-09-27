// Image decoding is not part of these tests: no decoder, no conversions.
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include "Epub/converters/ImageDecoderFactory.h"

class JpegToFramebufferConverter {};
class PngToFramebufferConverter {};
std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder;
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
bool PngToBmpConverter::pngFileToBmpStream(HalFile&, Print&, bool) { return false; }
bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile&, Print&, bool) { return false; }
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
