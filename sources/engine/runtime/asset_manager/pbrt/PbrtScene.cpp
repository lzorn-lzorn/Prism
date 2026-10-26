#include "PbrtScene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace runtime::asset::pbrt
{
namespace
{

// ============================================================================
//  词法分析: 把 pbrt 文本切分为 Token
// ============================================================================

enum class TokKind
{
    Word,     // 裸词(指令名等)
    Number,   // 数字
    String,   // "引号字符串"
    LBracket, // [
    RBracket, // ]
};

struct Token
{
    TokKind Kind { TokKind::Word };
    std::string Text;
    double Number { 0.0 };
};

std::string readFile(const std::string& Path)
{
    std::ifstream File(Path, std::ios::binary);
    if (!File)
        throw std::runtime_error("pbrt: cannot open file: " + Path);
    std::ostringstream Ss;
    Ss << File.rdbuf();
    return Ss.str();
}

std::vector<Token> tokenize(const std::string& Text)
{
    std::vector<Token> Tokens;
    const std::size_t N = Text.size();
    std::size_t I = 0;

    auto skipWhitespaceAndComments = [&]()
    {
        for (;;)
        {
            while (I < N && std::isspace(static_cast<unsigned char>(Text[I])))
                ++I;
            if (I < N && Text[I] == '#')
            {
                while (I < N && Text[I] != '\n')
                    ++I;
                continue;
            }
            break;
        }
    };

    for (;;)
    {
        skipWhitespaceAndComments();
        if (I >= N)
            break;
        const char C = Text[I];
        if (C == '"')
        {
            ++I;
            std::string Value;
            while (I < N && Text[I] != '"')
                Value.push_back(Text[I++]);
            ++I; // 跳过收尾引号
            Token T;
            T.Kind = TokKind::String;
            T.Text = Value;
            Tokens.push_back(std::move(T));
            continue;
        }
        if (C == '[')
        {
            Tokens.push_back(Token { TokKind::LBracket, "[" });
            ++I;
            continue;
        }
        if (C == ']')
        {
            Tokens.push_back(Token { TokKind::RBracket, "]" });
            ++I;
            continue;
        }
        // 数字或裸词: 读到空白/括号/引号/注释为止.
        std::size_t Start = I;
        while (I < N && !std::isspace(static_cast<unsigned char>(Text[I])) &&
            Text[I] != '[' && Text[I] != ']' && Text[I] != '"' && Text[I] != '#')
            ++I;
        const std::string Word = Text.substr(Start, I - Start);
        if (Word.empty())
        {
            ++I;
            continue;
        }
        // 判断是否为数字(整段都能被 strtod 消费).
        char* End = nullptr;
        const double Number = std::strtod(Word.c_str(), &End);
        Token T;
        if (End && *End == '\0')
        {
            T.Kind = TokKind::Number;
            T.Number = Number;
            T.Text = Word;
        }
        else
        {
            T.Kind = TokKind::Word;
            T.Text = Word;
        }
        Tokens.push_back(std::move(T));
    }
    return Tokens;
}

// ============================================================================
//  矩阵辅助(pbrt 行向量约定 → 引擎列向量约定)
// ============================================================================

// pbrt 的 Transform [16] 是行主序 + 行向量(平移在最后一行).
// 引擎 Matrix4x4 是行主序存储 + 列向量(M * v, 平移在最后一列). 故取转置.
infra::Matrix4x4 fromPbrtMatrix(const std::array<double, 16>& V)
{
    infra::Matrix4x4 M {};
    for (int Row = 0; Row < 4; ++Row)
        for (int Col = 0; Col < 4; ++Col)
            M[Row][Col] = static_cast<float>(V[static_cast<std::size_t>(Col) * 4 + Row]);
    return M;
}

infra::Matrix4x4 makeTranslation(float X, float Y, float Z)
{
    infra::Matrix4x4 M = infra::Matrix4x4::Identity();
    M[0][3] = X;
    M[1][3] = Y;
    M[2][3] = Z;
    return M;
}

infra::Matrix4x4 makeScale(float X, float Y, float Z)
{
    infra::Matrix4x4 M = infra::Matrix4x4::Identity();
    M[0][0] = X;
    M[1][1] = Y;
    M[2][2] = Z;
    return M;
}

// 绕任意轴旋转(角度制). 返回列向量约定的旋转矩阵.
infra::Matrix4x4 makeRotateAxis(float Degrees, float X, float Y, float Z)
{
    const float Rad = Degrees * 3.14159265358979323846f / 180.0f;
    const float C = std::cos(Rad);
    const float S = std::sin(Rad);
    const float Len = std::sqrt(X * X + Y * Y + Z * Z);
    if (Len < 1e-12f)
        return infra::Matrix4x4::Identity();
    X /= Len;
    Y /= Len;
    Z /= Len;
    const float T = 1.0f - C;

    infra::Matrix4x4 M = infra::Matrix4x4::Identity();
    M[0][0] = T * X * X + C;
    M[0][1] = T * X * Y - S * Z;
    M[0][2] = T * X * Z + S * Y;
    M[1][0] = T * X * Y + S * Z;
    M[1][1] = T * Y * Y + C;
    M[1][2] = T * Y * Z - S * X;
    M[2][0] = T * X * Z - S * Y;
    M[2][1] = T * Y * Z + S * X;
    M[2][2] = T * Z * Z + C;
    return M;
}

// ============================================================================
//  黑体色温 → 线性 RGB(归一化到 [0,1] 的相对能量)
// ============================================================================

std::array<float, 3> blackbodyToRgb(float Kelvin)
{
    const float Temp = std::max(1000.0f, Kelvin) / 100.0f;
    float R, G, B;

    if (Temp <= 66.0f)
    {
        R = 255.0f;
        G = 99.4708025861f * std::log(Temp) - 161.1195681661f;
    }
    else
    {
        R = 329.698727446f * std::pow(Temp - 60.0f, -0.1332047592f);
        G = 288.1221695283f * std::pow(Temp - 60.0f, -0.0755148492f);
    }
    if (Temp >= 66.0f)
        B = 255.0f;
    else if (Temp <= 19.0f)
        B = 0.0f;
    else
        B = 138.5177312231f * std::log(Temp - 10.0f) - 305.0447927307f;

    R = std::clamp(R, 0.0f, 255.0f) / 255.0f;
    G = std::clamp(G, 0.0f, 255.0f) / 255.0f;
    B = std::clamp(B, 0.0f, 255.0f) / 255.0f;
    return { R, G, B };
}

// ============================================================================
//  场景解析器
// ============================================================================

struct GraphicsState
{
    infra::Matrix4x4 CTM = infra::Matrix4x4::Identity();
    std::uint32_t MaterialId { 0 };
    bool ReverseOrientation { false };
};

class Parser
{
public:
    explicit Parser(Scene& Out, std::vector<std::string>& Warnings)
        : Scene_(Out), Warnings_(Warnings)
    {
        // [0] = 默认材质(找不到命名材质时回退).
        Material Default;
        Default.Type = "diffuse";
        Default.Reflectance = Spectrum { { 0.8f, 0.1f, 0.1f }, {}, false };
        Scene_.Materials.push_back(std::move(Default));
    }

    void parseFile(const std::string& Path)
    {
        const std::size_t Slash = Path.find_last_of("/\\");
        const std::string Dir = Slash == std::string::npos ? std::string() : Path.substr(0, Slash);
        const std::string SavedDir = CurrentDir_;
        CurrentDir_ = Dir;
        parseTokens(tokenize(readFile(Path)));
        CurrentDir_ = SavedDir;
    }

private:
    Scene& Scene_;
    std::vector<std::string>& Warnings_;

    GraphicsState State_;
    std::vector<GraphicsState> AttributeStack_;
    std::map<std::string, std::uint32_t> MaterialByName_;

    // ObjectBegin/ObjectInstance: 名字 → 捕获到的实例(已带 ObjectBegin 时的世界变换).
    std::map<std::string, std::vector<Instance>> Objects_;
    std::vector<Instance> CaptureBuffer_;
    std::string CapturingObject_; // 非空表示正在捕获 Object 定义

    void warn(const std::string& Msg) { Warnings_.push_back(Msg); }

    std::uint32_t resolveMaterial(const std::string& Name)
    {
        const auto It = MaterialByName_.find(Name);
        if (It == MaterialByName_.end())
        {
            warn("pbrt: unknown NamedMaterial '" + Name + "', fallback to default");
            return 0;
        }
        return It->second;
    }

    // ------------------------------------------------------------------
    //  读取助手
    // ------------------------------------------------------------------
    const Token& Peek() const
    {
        if (Pos_ >= Tokens_.size())
            throw std::runtime_error("pbrt: unexpected end of stream");
        return Tokens_[Pos_];
    }

    const Token& Next()
    {
        if (Pos_ >= Tokens_.size())
            throw std::runtime_error("pbrt: unexpected end of stream");
        return Tokens_[Pos_++];
    }

    bool Eof() const { return Pos_ >= Tokens_.size(); }

    void expectWord(const std::string& W)
    {
        const Token& T = Next();
        if (T.Kind != TokKind::Word || T.Text != W)
            throw std::runtime_error("pbrt: expected '" + W + "'");
    }

    std::string expectString()
    {
        const Token& T = Next();
        if (T.Kind != TokKind::String)
            throw std::runtime_error("pbrt: expected string");
        return T.Text;
    }

    double expectNumber()
    {
        const Token& T = Next();
        if (T.Kind != TokKind::Number)
            throw std::runtime_error("pbrt: expected number");
        return T.Number;
    }

    // 读取一个参数值: 单 token, 或 [ token ... ].
    std::vector<Token> readParamValue()
    {
        std::vector<Token> Value;
        if (!Eof() && Peek().Kind == TokKind::LBracket)
        {
            Next(); // [
            while (!Eof() && Peek().Kind != TokKind::RBracket)
                Value.push_back(Next());
            if (!Eof())
                Next(); // ]
        }
        else
        {
            Value.push_back(Next());
        }
        return Value;
    }

    std::vector<double> asNumbers(const std::vector<Token>& V) const
    {
        std::vector<double> Out;
        Out.reserve(V.size());
        for (const Token& T : V)
        {
            if (T.Kind != TokKind::Number)
                return {};
            Out.push_back(T.Number);
        }
        return Out;
    }

    std::string asSingleString(const std::vector<Token>& V) const
    {
        // 允许 "name" 或 [ "name" ].
        for (const Token& T : V)
        {
            if (T.Kind == TokKind::String)
                return T.Text;
        }
        return {};
    }

    // 读取一个 (参数类型, 值) 对, 返回参数类型; 值通过 out 返回.
    std::string readParamPair(std::vector<Token>& OutValue)
    {
        const std::string ParamType = expectString();
        OutValue = readParamValue();
        return ParamType;
    }

    // ------------------------------------------------------------------
    //  核心指令分发
    // ------------------------------------------------------------------
    void parseTokens(const std::vector<Token>& Tokens)
    {
        // 保存现场, 支持 Include 的递归展开(状态跨文件延续).
        const std::size_t SavedPos = Pos_;
        const std::vector<Token> SavedTokens = std::move(Tokens_);
        Tokens_ = Tokens;
        Pos_ = 0;

        while (!Eof())
        {
            const Token& Word = Next();
            if (Word.Kind != TokKind::Word)
            {
                warn("pbrt: skipping unexpected token '" + Word.Text + "'");
                continue;
            }
            dispatch(Word.Text);
        }

        Tokens_ = std::move(SavedTokens);
        Pos_ = SavedPos;
    }

    void dispatch(const std::string& Name)
    {
        if (Name == "AttributeBegin")
        {
            AttributeStack_.push_back(State_);
        }
        else if (Name == "AttributeEnd")
        {
            if (!AttributeStack_.empty())
            {
                State_ = AttributeStack_.back();
                AttributeStack_.pop_back();
            }
        }
        else if (Name == "WorldBegin")
        {
            State_ = GraphicsState {};
        }
        else if (Name == "Transform")
        {
            State_.CTM = fromPbrtMatrix(expectMatrix16());
        }
        else if (Name == "Identity")
        {
            State_.CTM = infra::Matrix4x4::Identity();
        }
        else if (Name == "Translate")
        {
            const float X = static_cast<float>(expectNumber());
            const float Y = static_cast<float>(expectNumber());
            const float Z = static_cast<float>(expectNumber());
            State_.CTM = State_.CTM * makeTranslation(X, Y, Z);
        }
        else if (Name == "Scale")
        {
            const float X = static_cast<float>(expectNumber());
            const float Y = static_cast<float>(expectNumber());
            const float Z = static_cast<float>(expectNumber());
            State_.CTM = State_.CTM * makeScale(X, Y, Z);
        }
        else if (Name == "Rotate")
        {
            const float Deg = static_cast<float>(expectNumber());
            const float X = static_cast<float>(expectNumber());
            const float Y = static_cast<float>(expectNumber());
            const float Z = static_cast<float>(expectNumber());
            State_.CTM = State_.CTM * makeRotateAxis(Deg, X, Y, Z);
        }
        else if (Name == "LookAt")
        {
            Camera Cam;
            Cam.Eye = { static_cast<float>(expectNumber()), static_cast<float>(expectNumber()), static_cast<float>(expectNumber()) };
            Cam.LookAt = { static_cast<float>(expectNumber()), static_cast<float>(expectNumber()), static_cast<float>(expectNumber()) };
            Cam.Up = { static_cast<float>(expectNumber()), static_cast<float>(expectNumber()), static_cast<float>(expectNumber()) };
            if (Scene_.Cam)
                Cam.Fov = Scene_.Cam->Fov; // 保留已解析的 fov
            Scene_.Cam = Cam;
        }
        else if (Name == "Camera")
        {
            parseCamera();
        }
        else if (Name == "Shape")
        {
            parseShape();
        }
        else if (Name == "NamedMaterial")
        {
            const std::string MaterialName = expectString();
            State_.MaterialId = resolveMaterial(MaterialName);
        }
        else if (Name == "Material")
        {
            const std::string Type = expectString();
            Material M;
            M.Type = Type;
            parseMaterialBody(M);
            State_.MaterialId = static_cast<std::uint32_t>(Scene_.Materials.size());
            Scene_.Materials.push_back(std::move(M));
        }
        else if (Name == "MakeNamedMaterial")
        {
            const std::string MaterialName = expectString();
            Material M;
            parseMaterialBody(M); // 首个参数对通常是 "string type"
            const std::uint32_t Id = static_cast<std::uint32_t>(Scene_.Materials.size());
            Scene_.Materials.push_back(std::move(M));
            MaterialByName_[MaterialName] = Id;
        }
        else if (Name == "Texture")
        {
            // 记录命名纹理(仅用于解析, 不求值): 跳过其参数.
            const std::string TexName = expectString();
            expectString(); // 纹理类别(spectrum/float)
            expectString(); // 纹理类型(imagemap/scale/mix/...)
            while (!Eof() && Peek().Kind == TokKind::String)
            {
                std::vector<Token> Value;
                readParamPair(Value);
            }
            TextureNames_.insert(TexName);
        }
        else if (Name == "MakeNamedMedium")
        {
            // 命名介质: 记录名字, 参数跳过(参与介质求值留待后续).
            const std::string MediumName = expectString();
            while (!Eof() && Peek().Kind == TokKind::String)
            {
                std::vector<Token> Value;
                readParamPair(Value);
            }
            Media_.insert(MediumName);
        }
        else if (Name == "LightSource")
        {
            parseLight(false);
        }
        else if (Name == "AreaLightSource")
        {
            parseLight(true);
        }
        else if (Name == "ObjectBegin")
        {
            CapturingObject_ = expectString();
            CaptureBuffer_.clear();
        }
        else if (Name == "ObjectEnd")
        {
            if (!CapturingObject_.empty())
            {
                Objects_[CapturingObject_] = std::move(CaptureBuffer_);
                CapturingObject_.clear();
            }
            // ObjectEnd 可带一个可选的名字字符串(本场景不带), 消费掉以避免误判.
            if (!Eof() && Peek().Kind == TokKind::String)
                (void)Next();
        }
        else if (Name == "ObjectInstance")
        {
            const std::string ObjectName = expectString();
            const auto It = Objects_.find(ObjectName);
            if (It != Objects_.end())
            {
                for (const Instance& Base : It->second)
                {
                    Instance Inst = Base;
                    Inst.Transform = State_.CTM * Base.Transform;
                    Scene_.Instances.push_back(std::move(Inst));
                }
            }
            else
            {
                warn("pbrt: ObjectInstance '" + ObjectName + "' before its ObjectBegin");
            }
        }
        else if (Name == "ReverseOrientation")
        {
            State_.ReverseOrientation = true;
        }
        else if (Name == "MediumInterface")
        {
            expectString();
            expectString();
        }
        else if (Name == "Include")
        {
            const std::string Relative = expectString();
            const std::string Included = resolvePath(CurrentDir_, Relative);
            // 被 include 的文件可能继续 include 相对其自身的文件, 切换当前目录后递归.
            const std::string SavedDir = CurrentDir_;
            const std::size_t Slash = Included.find_last_of("/\\");
            CurrentDir_ = Slash == std::string::npos ? std::string() : Included.substr(0, Slash);
            parseTokens(tokenize(readFile(Included)));
            CurrentDir_ = SavedDir;
        }
        else if (Name == "ConcatTransform")
        {
            State_.CTM = State_.CTM * fromPbrtMatrix(expectMatrix16());
        }
        else
        {
            // 明确忽略的指令: 只消费其参数, 不告警.
            static const std::set<std::string> NoOp {
                "Integrator", "Film", "Sampler", "PixelFilter", "Accelerator",
                "CoordSysTransform", "CoordinateSystem", "WorldEnd"
            };
            if (!NoOp.count(Name))
                warn("pbrt: unknown directive '" + Name + "'");
            skipNoOp();
        }
    }

    /**
     * @brief 消费"无需处理"指令的体: <类型字符串?> <参数对...>.
     *
     * 例如 Film "gbuffer" "integer yresolution" [1500] ... / Integrator "volpath" ...
     * 这些指令第一个 token 是类型字符串(不是参数对), 因此不能直接用 readParamPair 循环.
     */
    void skipNoOp()
    {
        if (!Eof() && Peek().Kind == TokKind::String)
            (void)Next(); // 类型字符串(如 Film 的 "gbuffer")
        while (!Eof() && Peek().Kind == TokKind::String)
        {
            std::vector<Token> Value;
            readParamPair(Value);
        }
    }

    std::array<double, 16> expectMatrix16()
    {
        std::array<double, 16> M {};
        const Token& B = Next();
        if (B.Kind != TokKind::LBracket)
            throw std::runtime_error("pbrt: Transform expects [16 numbers]");
        for (int I = 0; I < 16; ++I)
            M[static_cast<std::size_t>(I)] = expectNumber();
        if (!Eof())
            Next(); // ]
        return M;
    }

    void parseCamera()
    {
        const std::string CamType = expectString();
        Camera Cam = Scene_.Cam.value_or(Camera {});
        std::vector<Token> Value;
        while (!Eof() && Peek().Kind == TokKind::String)
        {
            const std::string Param = readParamPair(Value);
            if (Param == "float fov")
            {
                const auto Nums = asNumbers(Value);
                if (!Nums.empty())
                    Cam.Fov = static_cast<float>(Nums.front());
            }
        }
        Scene_.Cam = Cam;
    }

    void parseShape()
    {
        const std::string ShapeType = expectString();
        std::map<std::string, std::vector<Token>> Params;
        while (!Eof() && Peek().Kind == TokKind::String)
        {
            std::vector<Token> Value;
            const std::string Param = readParamPair(Value);
            Params[Param] = std::move(Value);
        }

        MeshRef Mesh;
        if (ShapeType == "plymesh")
        {
            const auto It = Params.find("string filename");
            if (It == Params.end())
            {
                warn("pbrt: plymesh without filename");
                return;
            }
            Mesh.File = asSingleString(It->second);
        }
        else if (ShapeType == "trianglemesh")
        {
            // 内联三角汤: P/N/uv/indices.
            auto fill = [&](const char* Key, std::vector<float>& Out, std::size_t Comp)
            {
                const auto It = Params.find(Key);
                if (It != Params.end())
                {
                    const std::vector<double> Nums = asNumbers(It->second);
                    Out.reserve(Nums.size());
                    for (double V : Nums)
                        Out.push_back(static_cast<float>(V));
                }
            };
            fill("point3 P", Mesh.Positions, 3);
            fill("normal N", Mesh.Normals, 3);
            fill("point2 uv", Mesh.Texcoords, 2);
            const auto It = Params.find("integer indices");
            if (It != Params.end())
            {
                const std::vector<double> Nums = asNumbers(It->second);
                Mesh.Indices.reserve(Nums.size());
                for (double V : Nums)
                    Mesh.Indices.push_back(static_cast<std::uint32_t>(V));
            }
            else
            {
                // 无索引: 按顶点顺序 0,1,2,3,... 生成三角形.
                const std::size_t Tri = Mesh.Positions.size() / 9; // 9 float = 3 个 point
                Mesh.Indices.reserve(Tri * 3);
                for (std::size_t I = 0; I < Tri * 3; ++I)
                    Mesh.Indices.push_back(static_cast<std::uint32_t>(I));
            }
        }
        else
        {
            // sphere/disk/cylinder 等图元本加载层不展开为网格, 静默跳过(仅未知类型告警).
            static const std::set<std::string> KnownPrimitives {
                "sphere", "disk", "cylinder", "cone", "paraboloid", "hyperboloid", "nurbs", "loopsubdiv"
            };
            if (!KnownPrimitives.count(ShapeType))
                warn("pbrt: Shape type '" + ShapeType + "' not converted to mesh (skipped)");
            return;
        }

        // 面光源的发光网格: 把紧随其后的 plymesh 文件名回填到最近的面光源.
        if (PendingAreaLightIndex_ >= 0 && !Mesh.File.empty())
        {
            Scene_.Lights[static_cast<std::size_t>(PendingAreaLightIndex_)].MeshFile = Mesh.File;
            PendingAreaLightIndex_ = -1;
        }

        Instance Inst;
        Inst.MeshId = static_cast<std::uint32_t>(Scene_.Meshes.size());
        Inst.MaterialId = State_.MaterialId;
        Inst.Transform = State_.CTM;
        Scene_.Meshes.push_back(std::move(Mesh));

        if (!CapturingObject_.empty())
            CaptureBuffer_.push_back(std::move(Inst));
        else
            Scene_.Instances.push_back(std::move(Inst));
    }

    void parseMaterialBody(Material& M)
    {
        while (!Eof() && Peek().Kind == TokKind::String)
        {
            std::vector<Token> Value;
            const std::string Param = readParamPair(Value);

            if (Param == "string type")
                M.Type = asSingleString(Value);
            else if (Param == "rgb reflectance" || Param == "rgb albedo")
            {
                const std::vector<double> Nums = asNumbers(Value);
                if (Nums.size() >= 3)
                    M.Reflectance = Spectrum { { static_cast<float>(Nums[0]), static_cast<float>(Nums[1]), static_cast<float>(Nums[2]) }, {}, false };
            }
            else if (Param == "texture reflectance")
                M.Reflectance = Spectrum { {}, asSingleString(Value), true };
            else if (Param == "float roughness")
            {
                const std::vector<double> Nums = asNumbers(Value);
                if (!Nums.empty())
                    M.Roughness = Spectrum { { static_cast<float>(Nums.front()), 0, 0 }, {}, false };
            }
            else if (Param == "texture roughness")
                M.Roughness = Spectrum { {}, asSingleString(Value), true };
            else if (Param == "texture displacement")
                M.DisplacementMap = asSingleString(Value);
            else if (Param == "string normalmap")
                M.NormalMap = asSingleString(Value);
            else if (Param == "float eta")
            {
                const std::vector<double> Nums = asNumbers(Value);
                if (!Nums.empty())
                    M.Ior = static_cast<float>(Nums.front());
            }
            else
            {
                M.Extra[Param] = serializeValue(Value);
            }
        }
    }

    void parseLight(bool Area)
    {
        const std::string LightType = expectString();
        Light L;
        L.K = Area ? Light::Kind::Area : parseLightKind(LightType);
        L.Transform = State_.CTM;

        std::vector<Token> Value;
        while (!Eof() && Peek().Kind == TokKind::String)
        {
            const std::string Param = readParamPair(Value);
            if (Param == "blackbody L")
            {
                const std::vector<double> Nums = asNumbers(Value);
                if (!Nums.empty())
                    L.Color = blackbodyToRgb(static_cast<float>(Nums.front()));
            }
            else if (Param == "rgb L")
            {
                const std::vector<double> Nums = asNumbers(Value);
                if (Nums.size() >= 3)
                    L.Color = { static_cast<float>(Nums[0]), static_cast<float>(Nums[1]), static_cast<float>(Nums[2]) };
            }
            else if (Param == "float scale")
            {
                const std::vector<double> Nums = asNumbers(Value);
                if (!Nums.empty())
                    L.Scale = static_cast<float>(Nums.front());
            }
        }

        // 网格面光源: 紧随其后的 Shape "plymesh" 会引用同一个 CTM; 记录其文件名.
        if (Area)
        {
            // 面光源的形状由后续 Shape 声明; 这里无法直接关联, 保留 CTM 即可.
            // 调用方可用 "材质为纯黑 + AreaLightSource" 的约定重建发光网格.
        }
        Scene_.Lights.push_back(std::move(L));
        if (Area)
            PendingAreaLightIndex_ = static_cast<std::int64_t>(Scene_.Lights.size()) - 1;
    }

    static Light::Kind parseLightKind(const std::string& Type)
    {
        if (Type == "infinite")
            return Light::Kind::Infinite;
        if (Type == "point")
            return Light::Kind::Point;
        if (Type == "spot")
            return Light::Kind::Spot;
        if (Type == "distant")
            return Light::Kind::Distant;
        return Light::Kind::Point;
    }

    std::string serializeValue(const std::vector<Token>& Value) const
    {
        std::ostringstream Oss;
        for (const Token& T : Value)
        {
            if (T.Kind == TokKind::Number)
                Oss << T.Number << ' ';
            else if (T.Kind == TokKind::String)
                Oss << '"' << T.Text << "\" ";
            else
                Oss << T.Text << ' ';
        }
        return Oss.str();
    }

    static std::string resolvePath(const std::string& BaseDir, const std::string& Relative)
    {
        if (!BaseDir.empty() && !Relative.empty() && (Relative[0] != '/' && Relative[0] != '\\') &&
            Relative.find(':') == std::string::npos)
        {
            return BaseDir + "/" + Relative;
        }
        return Relative;
    }

    // token 游标与当前目录.
    std::vector<Token> Tokens_;
    std::size_t Pos_ { 0 };
    std::string CurrentDir_;

    std::set<std::string> TextureNames_;
    std::set<std::string> Media_;

    // 面光源声明后紧随的 Shape 是"发光网格"; 记录其索引用于回填文件名.
    std::int64_t PendingAreaLightIndex_ { -1 };
};

} // namespace

ParseResult parseSceneFile(const std::string& EntryPath)
{
    ParseResult Result;
    Parser Parser(Result.Scene, Result.Warnings);
    Parser.parseFile(EntryPath);
    return Result;
}

} // namespace runtime::asset::pbrt
