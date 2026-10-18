#include <infra/common/Error.hpp>
#include "SystemFonts.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#ifdef __APPLE__
#include <CoreText/CoreText.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <filesystem>
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#elif defined(__linux__)
#include <fontconfig/fontconfig.h>
#endif

namespace ui
{
SystemFonts discoverSystemFonts()
{
    std::set<std::string> Paths;
#ifdef __APPLE__
    const auto Collection = CTFontCollectionCreateFromAvailableFonts(nullptr);
    if (!Collection) Error::raise(EErrorCode::Runtime, "Unable to query system fonts");
    const auto Descriptors = CTFontCollectionCreateMatchingFontDescriptors(Collection);
    if (Descriptors)
    {
        for (CFIndex Index = 0; Index < CFArrayGetCount(Descriptors); ++Index)
        {
            const auto Descriptor = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(Descriptors, Index));
            const auto URL = static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(Descriptor, kCTFontURLAttribute));
            if (!URL) continue;
            const auto Path = CFURLCopyFileSystemPath(URL, kCFURLPOSIXPathStyle);
            if (Path)
            {
                const auto Capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(Path), kCFStringEncodingUTF8) + 1;
                std::string Buffer(static_cast<std::size_t>(Capacity), '\0');
                if (CFStringGetCString(Path, Buffer.data(), Capacity, kCFStringEncodingUTF8))
                {
                    Buffer.resize(std::char_traits<char>::length(Buffer.c_str()));
                    Paths.insert(std::move(Buffer));
                }
                CFRelease(Path);
            }
            CFRelease(URL);
        }
        CFRelease(Descriptors);
    }
    CFRelease(Collection);
#elif defined(_WIN32)
    using Microsoft::WRL::ComPtr;
    ComPtr<IDWriteFactory> Factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(Factory.GetAddressOf()))))
        Error::raise(EErrorCode::Runtime, "Unable to create DirectWrite factory");
    ComPtr<IDWriteFontCollection> Collection;
    if (FAILED(Factory->GetSystemFontCollection(&Collection)))
        Error::raise(EErrorCode::Runtime, "Unable to query system fonts");
    for (UINT32 FamilyIndex = 0; FamilyIndex < Collection->GetFontFamilyCount(); ++FamilyIndex)
    {
        ComPtr<IDWriteFontFamily> Family;
        if (FAILED(Collection->GetFontFamily(FamilyIndex, &Family))) continue;
        for (UINT32 Index = 0; Index < Family->GetFontCount(); ++Index)
        {
            ComPtr<IDWriteFont> Font;
            ComPtr<IDWriteFontFace> Face;
            if (FAILED(Family->GetFont(Index, &Font)) || FAILED(Font->CreateFontFace(&Face))) continue;
            UINT32 Count = 0;
            if (FAILED(Face->GetFiles(&Count, nullptr))) continue;
            std::vector<IDWriteFontFile*> Files(Count);
            if (FAILED(Face->GetFiles(&Count, Files.data()))) continue;
            for (auto* File : Files)
            {
                ComPtr<IDWriteFontFile> OwnedFile;
                OwnedFile.Attach(File);
                ComPtr<IDWriteFontFileLoader> Loader;
                ComPtr<IDWriteLocalFontFileLoader> Local;
                const void* Key = nullptr;
                UINT32 KeySize = 0, Length = 0;
                if (FAILED(File->GetLoader(&Loader)) || FAILED(Loader.As(&Local)) ||
                    FAILED(File->GetReferenceKey(&Key, &KeySize)) ||
                    FAILED(Local->GetFilePathLengthFromKey(Key, KeySize, &Length))) continue;
                std::wstring Path(Length + 1, L'\0');
                if (FAILED(Local->GetFilePathFromKey(Key, KeySize, Path.data(), Length + 1))) continue;
                Path.resize(Length);
                const auto UTF8 = std::filesystem::path(Path).u8string();
                Paths.emplace(reinterpret_cast<const char*>(UTF8.data()), UTF8.size());
            }
        }
    }
#elif defined(__linux__)
    std::unique_ptr<FcConfig, decltype(&FcConfigDestroy)> Config(FcInitLoadConfigAndFonts(), FcConfigDestroy);
    if (!Config) Error::raise(EErrorCode::Runtime, "Unable to query system fonts");
    std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> Pattern(FcPatternCreate(), FcPatternDestroy);
    std::unique_ptr<FcObjectSet, decltype(&FcObjectSetDestroy)> Objects(FcObjectSetBuild(FC_FILE, nullptr), FcObjectSetDestroy);
    if (!Pattern || !Objects) Error::raise(EErrorCode::Runtime, "Unable to allocate font query");
    std::unique_ptr<FcFontSet, decltype(&FcFontSetDestroy)> Fonts(
        FcFontList(Config.get(), Pattern.get(), Objects.get()), FcFontSetDestroy);
    if (Fonts)
    {
        for (int Index = 0; Index < Fonts->nfont; ++Index)
        {
            FcChar8* Path = nullptr;
            if (FcPatternGetString(Fonts->fonts[Index], FC_FILE, 0, &Path) == FcResultMatch)
                Paths.emplace(reinterpret_cast<const char*>(Path));
        }
    }
#else
    Error::raise(EErrorCode::Runtime, "System font discovery is unsupported on this platform");
#endif
    FT_Library Library = nullptr;
    if (FT_Init_FreeType(&Library)) Error::raise(EErrorCode::Runtime, "Unable to inspect system fonts");
    const std::unique_ptr<std::remove_pointer_t<FT_Library>, decltype(&FT_Done_FreeType)> OwnedLibrary(Library, FT_Done_FreeType);
    struct Candidate { std::string Path; int Score; };
    std::vector<Candidate> TextFonts;
    SystemFonts Result;
    for (const auto& Path : Paths)
    {
        FT_Face Face = nullptr;
        if (FT_New_Face(Library, Path.c_str(), 0, &Face)) continue;
        const std::unique_ptr<std::remove_pointer_t<FT_Face>, decltype(&FT_Done_Face)> OwnedFace(Face, FT_Done_Face);
        if (FT_Select_Charmap(Face, FT_ENCODING_UNICODE)) continue;
        if (FT_HAS_COLOR(Face) && FT_Get_Char_Index(Face, 0x1f600))
            Result.EmojiFontPaths.push_back(Path);
        else if (FT_IS_SCALABLE(Face) && FT_Get_Char_Index(Face, 'A'))
        {
            int Score = 0;
            for (const auto Scalar : {0x4e2du, 0x4f60u, 0x6c3cu, 0x20acu})
                if (FT_Get_Char_Index(Face, Scalar)) ++Score;
            if (!(Face->style_flags & (FT_STYLE_FLAG_BOLD | FT_STYLE_FLAG_ITALIC))) ++Score;
            TextFonts.push_back({Path, Score});
        }
    }
    std::stable_sort(TextFonts.begin(), TextFonts.end(), [](const auto& First, const auto& Second)
    {
        return First.Score > Second.Score;
    });
    for (const auto& Font : TextFonts)
    {
        if (Result.DefaultFontPaths.size() == 4) break;
        Result.DefaultFontPaths.push_back(Font.Path);
    }
    if (Result.EmojiFontPaths.size() > 2) Result.EmojiFontPaths.resize(2);
    return Result;
}
}