/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021 Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2019, 2021 Jonathan Hale <squareys@googlemail.com>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNETCION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "BasisImporter.h"

#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/ScopeGuard.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/ConfigurationValue.h>
#include <Corrade/Utility/Debug.h>
#include <Corrade/Utility/DebugStl.h>
#include <Corrade/Utility/String.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/Trade/ImageData.h>

#include <basisu_transcoder.h>

namespace Magnum { namespace Trade { namespace {

/* Map BasisImporter::TargetFormat to (Compressed)PixelFormat. See the
   TargetFormat enum for details. */
PixelFormat pixelFormat(BasisImporter::TargetFormat type, bool isSrgb) {
    switch(type) {
        case BasisImporter::TargetFormat::RGBA8:
            return isSrgb ? PixelFormat::RGBA8Srgb : PixelFormat::RGBA8Unorm;
        default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
    }
}

CompressedPixelFormat compressedPixelFormat(BasisImporter::TargetFormat type, bool isSrgb) {
    switch(type) {
        case BasisImporter::TargetFormat::Etc1RGB:
            return isSrgb ? CompressedPixelFormat::Etc2RGB8Srgb : CompressedPixelFormat::Etc2RGB8Unorm;
        case BasisImporter::TargetFormat::Etc2RGBA:
            return isSrgb ? CompressedPixelFormat::Etc2RGBA8Srgb : CompressedPixelFormat::Etc2RGBA8Unorm;
        case BasisImporter::TargetFormat::Bc1RGB:
            return isSrgb ? CompressedPixelFormat::Bc1RGBSrgb : CompressedPixelFormat::Bc1RGBUnorm;
        case BasisImporter::TargetFormat::Bc3RGBA:
            return isSrgb ? CompressedPixelFormat::Bc3RGBASrgb : CompressedPixelFormat::Bc3RGBAUnorm;
        /** @todo use bc7/bc4/bc5 based on channel count? needs a bit from
            https://github.com/BinomialLLC/basis_universal/issues/66 */
        case BasisImporter::TargetFormat::Bc4R:
            return CompressedPixelFormat::Bc4RUnorm;
        case BasisImporter::TargetFormat::Bc5RG:
            return CompressedPixelFormat::Bc5RGUnorm;
        case BasisImporter::TargetFormat::Bc7RGB:
            return isSrgb ? CompressedPixelFormat::Bc7RGBASrgb : CompressedPixelFormat::Bc7RGBAUnorm;
        case BasisImporter::TargetFormat::Bc7RGBA:
            return isSrgb ? CompressedPixelFormat::Bc7RGBASrgb : CompressedPixelFormat::Bc7RGBAUnorm;
        case BasisImporter::TargetFormat::PvrtcRGB4bpp:
            return isSrgb ? CompressedPixelFormat::PvrtcRGB4bppSrgb : CompressedPixelFormat::PvrtcRGB4bppUnorm;
        case BasisImporter::TargetFormat::PvrtcRGBA4bpp:
            return isSrgb ? CompressedPixelFormat::PvrtcRGBA4bppSrgb : CompressedPixelFormat::PvrtcRGBA4bppUnorm;
        case BasisImporter::TargetFormat::Astc4x4RGBA:
            return isSrgb ? CompressedPixelFormat::Astc4x4RGBASrgb : CompressedPixelFormat::Astc4x4RGBAUnorm;
        /** @todo use etc2/eacR/eacRG based on channel count? needs a bit from
            https://github.com/BinomialLLC/basis_universal/issues/66 */
        case BasisImporter::TargetFormat::EacR:
            return CompressedPixelFormat::EacR11Unorm;
        case BasisImporter::TargetFormat::EacRG:
            return CompressedPixelFormat::EacRG11Unorm;
        default: CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
    }
}

constexpr const char* FormatNames[]{
    "Etc1RGB", "Etc2RGBA",
    "Bc1RGB", "Bc3RGBA", "Bc4R", "Bc5RG", "Bc7RGB", "Bc7RGBA",
    "PvrtcRGB4bpp", "PvrtcRGBA4bpp",
    "Astc4x4RGBA",
    nullptr, nullptr, /* ATC formats */
    "RGBA8", nullptr, nullptr, nullptr, /* RGB565 / BGR565 / RGBA4444 */
    nullptr, nullptr, nullptr,
    "EacR", "EacRG"
};

/* Last element has to be on the same index as last enum value */
static_assert(Containers::arraySize(FormatNames) - 1 == Int(BasisImporter::TargetFormat::EacRG), "bad string format mapping");

}}}

namespace Corrade { namespace Utility {

/* Configuration value implementation for BasisImporter::TargetFormat */
template<> struct ConfigurationValue<Magnum::Trade::BasisImporter::TargetFormat> {
    static std::string toString(Magnum::Trade::BasisImporter::TargetFormat value, ConfigurationValueFlags) {
        if(Magnum::UnsignedInt(value) < Containers::arraySize(Magnum::Trade::FormatNames))
            return Magnum::Trade::FormatNames[Magnum::UnsignedInt(value)];

        return "<invalid>";
    }

    static Magnum::Trade::BasisImporter::TargetFormat fromString(const std::string& value, ConfigurationValueFlags) {
        Magnum::Int i = 0;
        for(const char* name: Magnum::Trade::FormatNames) {
            if(name && value == name) return Magnum::Trade::BasisImporter::TargetFormat(i);
            ++i;
        }

        return Magnum::Trade::BasisImporter::TargetFormat(~Magnum::UnsignedInt{});
    }
};

}}

namespace Magnum { namespace Trade {

struct BasisImporter::State {
    /* There is only this type of codebook */
    basist::etc1_global_selector_codebook codebook;
    Containers::Optional<basist::basisu_transcoder> transcoder;
    Containers::Array<char> in;
    basist::basisu_file_info fileInfo;
    bool isSrgb;

    bool noTranscodeFormatWarningPrinted = false;

    explicit State(): codebook(basist::g_global_selector_cb_size,
        basist::g_global_selector_cb) {}
};

void BasisImporter::initialize() {
    basist::basisu_transcoder_init();
}

BasisImporter::BasisImporter(): _state{InPlaceInit} {
    /* Initialize default configuration values */
    /** @todo horrible workaround, fix this properly */
    configuration().setValue("format", "");
}

BasisImporter::BasisImporter(PluginManager::AbstractManager& manager, const std::string& plugin): AbstractImporter{manager, plugin} {
    /* Initializes codebook */
    _state.reset(new State);

    /* Set format configuration from plugin alias */
    if(Corrade::Utility::String::beginsWith(plugin, "BasisImporter")) {
        /* Has type prefix. We can assume the substring results in a valid
           value as the plugin conf limits it to known suffixes */
        if(plugin.size() > 13) configuration().setValue("format", plugin.substr(13));
    }
}

BasisImporter::~BasisImporter() = default;

ImporterFeatures BasisImporter::doFeatures() const { return ImporterFeature::OpenData; }

bool BasisImporter::doIsOpened() const {
    /* Both the transcoder and then input data have to be present or both
       have to be empty */
    CORRADE_INTERNAL_ASSERT(!_state->transcoder == !_state->in);
    return _state->in;
}

void BasisImporter::doClose() {
    _state->transcoder = Containers::NullOpt;
    _state->in = nullptr;
}

void BasisImporter::doOpenData(Containers::Array<char>&& data, DataFlags dataFlags) {
    /* Because here we're copying the data and using the _in to check if file
       is opened, having them nullptr would mean openData() would fail without
       any error message. It's not possible to do this check on the importer
       side, because empty file is valid in some formats (OBJ or glTF). We also
       can't do the full import here because then doImage2D() would need to
       copy the imported data instead anyway (and the uncompressed size is much
       larger). This way it'll also work nicely with a future openMemory(). */
    if(data.empty()) {
        Error{} << "Trade::BasisImporter::openData(): the file is empty";
        return;
    }

    _state->transcoder.emplace(&_state->codebook);
    Containers::ScopeGuard transcoderGuard{&_state->transcoder, [](Containers::Optional<basist::basisu_transcoder>* o) {
        *o = Containers::NullOpt;
    }};
    if(!_state->transcoder->validate_header(data.data(), data.size())) {
        Error() << "Trade::BasisImporter::openData(): invalid header";
        return;
    }

    /* Save the global file info to avoid calling that again each time we check
       for image count and whatnot; start transcoding */
    if(!_state->transcoder->get_file_info(data.data(), data.size(), _state->fileInfo) ||
       !_state->transcoder->start_transcoding(data.data(), data.size())) {
        Error() << "Trade::BasisImporter::openData(): bad basis file";
        return;
    }

    /* For some reason cBASISHeaderFlagSRGB is not exposed in basisu_file_info,
       get it from the header directly */
    const basist::basis_file_header& header = *reinterpret_cast<const basist::basis_file_header*>(data.data());
    _state->isSrgb = header.m_flags & basist::basis_header_flags::cBASISHeaderFlagSRGB;

    /* All good, release the transcoder guard. Keep the original data, take
       over the existing array or copy the data if we can't. */
    transcoderGuard.release();
    if(dataFlags & (DataFlag::Owned|DataFlag::ExternallyOwned)) {
        _state->in = std::move(data);
    } else {
        _state->in = Containers::Array<char>{NoInit, data.size()};
        Utility::copy(data, _state->in);
    }
}

UnsignedInt BasisImporter::doImage2DCount() const {
    return _state->fileInfo.m_total_images;
}

UnsignedInt BasisImporter::doImage2DLevelCount(const UnsignedInt id) {
    return _state->fileInfo.m_image_mipmap_levels[id];
}

Containers::Optional<ImageData2D> BasisImporter::doImage2D(const UnsignedInt id, const UnsignedInt level) {
    std::string targetFormatStr = configuration().value<std::string>("format");
    TargetFormat targetFormat;
    if(targetFormatStr.empty()) {
        if(!_state->noTranscodeFormatWarningPrinted)
            Warning{} << "Trade::BasisImporter::image2D(): no format to transcode to was specified, falling back to uncompressed RGBA8. To get rid of this warning either load the plugin via one of its BasisImporterEtc1RGB, ... aliases, or explicitly set the format option in plugin configuration.";
        targetFormat = TargetFormat::RGBA8;
        _state->noTranscodeFormatWarningPrinted = true;
    } else {
        targetFormat = configuration().value<TargetFormat>("format");
        if(UnsignedInt(targetFormat) == ~UnsignedInt{}) {
            Error{} << "Trade::BasisImporter::image2D(): invalid transcoding target format"
                << targetFormatStr << Debug::nospace << ", expected to be one of EacR, EacRG, Etc1RGB, Etc2RGBA, Bc1RGB, Bc3RGBA, Bc4R, Bc5RG, Bc7RGB, Bc7RGBA, Pvrtc1RGB4bpp, Pvrtc1RGBA4bpp, Astc4x4RGBA, RGBA8";
            return Containers::NullOpt;
        }
    }

    const auto format = basist::transcoder_texture_format(Int(targetFormat));
    const bool isUncompressed = basist::basis_transcoder_format_is_uncompressed(format);

    basist::basisu_image_info info;
    /* Header validation etc. is already done in doOpenData() and id is
       bounds-checked against doImage2DCount() by AbstractImporter, so by
       looking at the code there's nothing else that could fail and wasn't
       already caught before. That means we also can't craft any file to cover
       an error path, so turning this into an assert. When this blows up for
       someome, we'd most probably need to harden doOpenData() to catch that,
       not turning this into a graceful error. */
    CORRADE_INTERNAL_ASSERT_OUTPUT(_state->transcoder->get_image_info(_state->in.data(), _state->in.size(), info, id));

    UnsignedInt origWidth, origHeight, totalBlocks;
    /* Same as above, it checks for state we already verified before. If this
       blows up for someone, we can reconsider. */
    CORRADE_INTERNAL_ASSERT_OUTPUT(_state->transcoder->get_image_level_desc(_state->in.data(), _state->in.size(), id, level, origWidth, origHeight, totalBlocks));

    /* No flags used by transcode_image_level() by default */
    const std::uint32_t flags = 0;
    if(!_state->fileInfo.m_y_flipped) {
        /** @todo replace with the flag once the PR is submitted */
        Warning{} << "Trade::BasisImporter::image2D(): the image was not encoded Y-flipped, imported data will have wrong orientation";
        //flags |= basist::basisu_transcoder::cDecodeFlagsFlipY;
    }

    const Vector2i size{Int(origWidth), Int(origHeight)};
    UnsignedInt rowStride, outputRowsInPixels, outputSizeInBlocksOrPixels;
    if(isUncompressed) {
        rowStride = size.x();
        outputRowsInPixels = size.y();
        outputSizeInBlocksOrPixels = size.product();
    } else {
        rowStride = 0; /* left up to Basis to calculate */
        outputRowsInPixels = 0; /* not used for compressed data */
        outputSizeInBlocksOrPixels = totalBlocks;
    }
    const UnsignedInt dataSize = basis_get_bytes_per_block_or_pixel(format)*outputSizeInBlocksOrPixels;
    Containers::Array<char> dest{DefaultInit, dataSize};
    if(!_state->transcoder->transcode_image_level(_state->in.data(), _state->in.size(), id, level, dest.data(), outputSizeInBlocksOrPixels, format, flags, rowStride, nullptr, outputRowsInPixels)) {
        Error{} << "Trade::BasisImporter::image2D(): transcoding failed";
        return Containers::NullOpt;
    }

    if(isUncompressed)
        return Trade::ImageData2D{pixelFormat(targetFormat, _state->isSrgb), size, std::move(dest)};
    else
        return Trade::ImageData2D{compressedPixelFormat(targetFormat, _state->isSrgb), size, std::move(dest)};
}

void BasisImporter::setTargetFormat(TargetFormat format) {
    configuration().setValue("format", format);
}

BasisImporter::TargetFormat BasisImporter::targetFormat() const {
    return configuration().value<TargetFormat>("format");
}

}}

CORRADE_PLUGIN_REGISTER(BasisImporter, Magnum::Trade::BasisImporter,
    "cz.mosra.magnum.Trade.AbstractImporter/0.3.4")
