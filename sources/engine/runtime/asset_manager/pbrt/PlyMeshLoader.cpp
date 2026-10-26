#include "PlyMesh.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace runtime::asset
{
namespace
{

enum class EFormat
{
    Ascii,
    BinaryLittleEndian,
    BinaryBigEndian,
    Unsupported
};

struct Property
{
    std::string Name;
    int Size { 0 };
    bool IsFloat { false };
};

// 把 PLY 类型名映射为字节宽与是否为浮点.
bool parseTypeSize(const std::string& Type, int& OutSize, bool& OutFloat)
{
    if (Type == "char" || Type == "int8" || Type == "uchar" || Type == "uint8")
    {
        OutSize = 1;
        OutFloat = false;
        return true;
    }
    if (Type == "short" || Type == "int16" || Type == "ushort" || Type == "uint16")
    {
        OutSize = 2;
        OutFloat = false;
        return true;
    }
    if (Type == "int" || Type == "int32" || Type == "uint" || Type == "uint32")
    {
        OutSize = 4;
        OutFloat = false;
        return true;
    }
    if (Type == "float" || Type == "float32")
    {
        OutSize = 4;
        OutFloat = true;
        return true;
    }
    if (Type == "double" || Type == "float64")
    {
        OutSize = 8;
        OutFloat = true;
        return true;
    }
    return false;
}

// 逐字节读取小端整数.
std::uint32_t readU32(const std::byte* P)
{
    std::uint32_t V = 0;
    std::memcpy(&V, P, 4);
    return V;
}

float readF32(const std::byte* P)
{
    float V = 0.0f;
    std::memcpy(&V, P, 4);
    return V;
}

double readF64(const std::byte* P)
{
    double V = 0.0;
    std::memcpy(&V, P, 8);
    return V;
}

std::int32_t readI32(const std::byte* P)
{
    std::int32_t V = 0;
    std::memcpy(&V, P, 4);
    return V;
}

} // namespace

std::optional<PlyMesh> PlyMeshLoader::loadFromFile(const std::string& Path)
{
    std::ifstream File(Path, std::ios::binary);
    if (!File)
        return std::nullopt;
    File.seekg(0, std::ios::end);
    const std::streamoff Size = File.tellg();
    if (Size <= 0)
        return std::nullopt;
    File.seekg(0, std::ios::beg);

    std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
    File.read(reinterpret_cast<char*>(Bytes.data()), Size);
    if (!File)
        return std::nullopt;

    auto Result = loadFromMemory(Bytes, Path);
    return Result;
}

std::optional<PlyMesh> PlyMeshLoader::loadFromMemory(std::span<const std::byte> Data,
    const std::string& DebugName)
{
    if (Data.size() < 16)
        return std::nullopt;

    // ---- 1. 逐行解析头 ----
    const char* Text = reinterpret_cast<const char*>(Data.data());
    const std::size_t TextLength = Data.size();
    std::size_t Pos = 0;

    auto readLine = [&]() -> std::string
    {
        const std::size_t Start = Pos;
        while (Pos < TextLength && Text[Pos] != '\n')
            ++Pos;
        std::string Line(Text + Start, Pos - Start);
        if (Pos < TextLength)
            ++Pos; // 跳过 '\n'
        if (!Line.empty() && Line.back() == '\r')
            Line.pop_back();
        return Line;
    };

    if (readLine() != "ply")
        return std::nullopt;

    EFormat Format = EFormat::Unsupported;
    std::uint64_t VertexCount = 0;
    std::uint64_t FaceCount = 0;
    std::vector<Property> VertexProperties;
    // 面列表: <计数类型大小, 索引类型大小>.
    int FaceCountSize = 0;
    int FaceIndexSize = 0;

    for (;;)
    {
        const std::string Line = readLine();
        if (Line == "end_header")
            break;
        std::istringstream Iss(Line);
        std::string Keyword;
        Iss >> Keyword;
        if (Keyword == "format")
        {
            std::string Fmt;
            Iss >> Fmt;
            if (Fmt == "ascii")
                Format = EFormat::Ascii;
            else if (Fmt == "binary_little_endian")
                Format = EFormat::BinaryLittleEndian;
            else if (Fmt == "binary_big_endian")
                Format = EFormat::BinaryBigEndian;
        }
        else if (Keyword == "element")
        {
            std::string Kind;
            std::uint64_t Count = 0;
            Iss >> Kind >> Count;
            if (Kind == "vertex")
                VertexCount = Count;
            else if (Kind == "face")
                FaceCount = Count;
        }
        else if (Keyword == "property")
        {
            std::string Next;
            Iss >> Next;
            if (Next == "list")
            {
                // property list <count_type> <index_type> <name>
                std::string CountType, IndexType, Name;
                Iss >> CountType >> IndexType >> Name;
                int Sz = 0;
                bool Flt = false;
                if (parseTypeSize(CountType, Sz, Flt))
                    FaceCountSize = Sz;
                if (parseTypeSize(IndexType, Sz, Flt))
                    FaceIndexSize = Sz;
            }
            else
            {
                const std::string Type = Next;
                std::string Name;
                Iss >> Name;
                int Sz = 0;
                bool Flt = false;
                if (parseTypeSize(Type, Sz, Flt))
                    VertexProperties.push_back(Property { Name, Sz, Flt });
            }
        }
    }

    if (Format == EFormat::Unsupported || VertexCount == 0 || FaceCount == 0)
        return std::nullopt;
    if (FaceCountSize == 0 || FaceIndexSize == 0)
        return std::nullopt;

    // 顶点各分量的属性下标(-1 表示缺失).
    int IdxX = -1, IdxY = -1, IdxZ = -1;
    int IdxNx = -1, IdxNy = -1, IdxNz = -1;
    int IdxU = -1, IdxV = -1;
    int VertexStride = 0;
    for (std::size_t I = 0; I < VertexProperties.size(); ++I)
    {
        const std::string& Name = VertexProperties[I].Name;
        if (Name == "x")
            IdxX = static_cast<int>(I);
        else if (Name == "y")
            IdxY = static_cast<int>(I);
        else if (Name == "z")
            IdxZ = static_cast<int>(I);
        else if (Name == "nx")
            IdxNx = static_cast<int>(I);
        else if (Name == "ny")
            IdxNy = static_cast<int>(I);
        else if (Name == "nz")
            IdxNz = static_cast<int>(I);
        else if (Name == "u" || Name == "s" || Name == "texture_u")
            IdxU = static_cast<int>(I);
        else if (Name == "v" || Name == "t" || Name == "texture_v")
            IdxV = static_cast<int>(I);
        VertexStride += VertexProperties[I].Size;
    }

    const bool HasPosition = IdxX >= 0 && IdxY >= 0 && IdxZ >= 0;
    if (!HasPosition)
        return std::nullopt;

    PlyMesh Mesh;
    Mesh.Source = DebugName;
    Mesh.VertexCount = static_cast<std::uint32_t>(VertexCount);
    Mesh.HasNormals = IdxNx >= 0 && IdxNy >= 0 && IdxNz >= 0;
    Mesh.HasTexcoords = IdxU >= 0 && IdxV >= 0;
    Mesh.Positions.resize(static_cast<std::size_t>(VertexCount) * 3);
    if (Mesh.HasNormals)
        Mesh.Normals.resize(static_cast<std::size_t>(VertexCount) * 3);
    if (Mesh.HasTexcoords)
        Mesh.Texcoords.resize(static_cast<std::size_t>(VertexCount) * 2);

    const std::byte* Body = Data.data() + Pos;

    auto readScalar = [&](const std::byte* Base, const Property& P) -> double
    {
        if (P.IsFloat)
        {
            if (P.Size == 8)
                return readF64(Base);
            return readF32(Base);
        }
        // 整数属性统一按小端读入.
        switch (P.Size)
        {
        case 1: return static_cast<double>(*reinterpret_cast<const std::uint8_t*>(Base));
        case 2:
        {
            std::uint16_t V = 0;
            std::memcpy(&V, Base, 2);
            return static_cast<double>(V);
        }
        case 4: return static_cast<double>(readU32(Base));
        default: return 0.0;
        }
    };

    if (Format == EFormat::BinaryLittleEndian)
    {
        const std::byte* V = Body;
        for (std::uint64_t I = 0; I < VertexCount; ++I, V += VertexStride)
        {
            int Offset = 0;
            std::array<float, 8> P { 0, 0, 0, 0, 0, 0, 0, 0 };
            for (std::size_t K = 0; K < VertexProperties.size(); ++K)
            {
                const double Val = readScalar(V + Offset, VertexProperties[K]);
                Offset += VertexProperties[K].Size;
                if (static_cast<int>(K) == IdxX)
                    P[0] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxY)
                    P[1] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxZ)
                    P[2] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxNx)
                    P[3] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxNy)
                    P[4] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxNz)
                    P[5] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxU)
                    P[6] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxV)
                    P[7] = static_cast<float>(Val);
            }
            const std::size_t Base = static_cast<std::size_t>(I) * 3;
            Mesh.Positions[Base + 0] = P[0];
            Mesh.Positions[Base + 1] = P[1];
            Mesh.Positions[Base + 2] = P[2];
            if (Mesh.HasNormals)
            {
                Mesh.Normals[Base + 0] = P[3];
                Mesh.Normals[Base + 1] = P[4];
                Mesh.Normals[Base + 2] = P[5];
            }
            if (Mesh.HasTexcoords)
            {
                Mesh.Texcoords[static_cast<std::size_t>(I) * 2 + 0] = P[6];
                Mesh.Texcoords[static_cast<std::size_t>(I) * 2 + 1] = P[7];
            }
        }

        const std::byte* F = V;
        for (std::uint64_t I = 0; I < FaceCount; ++I)
        {
            std::uint32_t Count = 0;
            if (FaceCountSize == 1)
                Count = *reinterpret_cast<const std::uint8_t*>(F);
            else if (FaceCountSize == 4)
                Count = readU32(F);
            F += FaceCountSize;
            std::uint32_t Prev0 = 0, Prev1 = 0;
            for (std::uint32_t K = 0; K < Count; ++K)
            {
                std::uint32_t Idx = FaceIndexSize == 4 ? readU32(F) : static_cast<std::uint32_t>(*reinterpret_cast<const std::uint16_t*>(F));
                F += FaceIndexSize;
                if (K >= 2)
                {
                    Mesh.Indices.push_back(Prev0);
                    Mesh.Indices.push_back(Prev1);
                    Mesh.Indices.push_back(Idx);
                }
                Prev0 = Prev1;
                Prev1 = Idx;
            }
        }
    }
    else
    {
        // ascii 路径: 从 Body 起把剩余内容当文本.
        std::string AsciiBody(Text + Pos, TextLength - Pos);
        std::istringstream Iss(AsciiBody);
        for (std::uint64_t I = 0; I < VertexCount; ++I)
        {
            float P[8] {};
            for (std::size_t K = 0; K < VertexProperties.size(); ++K)
            {
                double Val = 0.0;
                Iss >> Val;
                if (static_cast<int>(K) == IdxX)
                    P[0] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxY)
                    P[1] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxZ)
                    P[2] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxNx)
                    P[3] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxNy)
                    P[4] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxNz)
                    P[5] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxU)
                    P[6] = static_cast<float>(Val);
                else if (static_cast<int>(K) == IdxV)
                    P[7] = static_cast<float>(Val);
            }
            const std::size_t Base = static_cast<std::size_t>(I) * 3;
            Mesh.Positions[Base] = P[0];
            Mesh.Positions[Base + 1] = P[1];
            Mesh.Positions[Base + 2] = P[2];
            if (Mesh.HasNormals)
            {
                Mesh.Normals[Base] = P[3];
                Mesh.Normals[Base + 1] = P[4];
                Mesh.Normals[Base + 2] = P[5];
            }
            if (Mesh.HasTexcoords)
            {
                Mesh.Texcoords[static_cast<std::size_t>(I) * 2] = P[6];
                Mesh.Texcoords[static_cast<std::size_t>(I) * 2 + 1] = P[7];
            }
        }
        for (std::uint64_t I = 0; I < FaceCount; ++I)
        {
            std::uint32_t Count = 0;
            Iss >> Count;
            std::uint32_t Prev0 = 0, Prev1 = 0;
            for (std::uint32_t K = 0; K < Count; ++K)
            {
                std::uint32_t Idx = 0;
                Iss >> Idx;
                if (K >= 2)
                {
                    Mesh.Indices.push_back(Prev0);
                    Mesh.Indices.push_back(Prev1);
                    Mesh.Indices.push_back(Idx);
                }
                Prev0 = Prev1;
                Prev1 = Idx;
            }
        }
    }

    Mesh.TriangleCount = static_cast<std::uint32_t>(Mesh.Indices.size() / 3);
    return Mesh;
}

} // namespace runtime::asset
