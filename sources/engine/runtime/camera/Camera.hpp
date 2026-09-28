
#pragma once

#include <Core.hpp>
namespace runtime
{
enum class EProjectionType
{
    None,
    Perspective,
    Orthographic,
    Fisheye,
    Panoramic,
    CubeMap,
    Custom
};

// 轴的方向
enum class EFovAxisType 
{
    Horizontal, // 水平方向 
    Vertical,   // 垂直方向
    Diagonal    // 对角线
};

enum class EAspectRatioPolicy 
{
    ConstrainHeight, // 固定垂直 FOV，水平随 Viewport 变
    ConstrainWidth,  // 固定水平 FOV
    Stretch,
    Crop,
    Overscan
};

enum class EProjectionDepthMode
{
    NormalZ,
    ReversedZ,
    InfiniteFar,
    ReversedInfiniteFar
};

struct LensParams {
    // 物理镜头 / 传感器
    float FocalLengthMm; // 焦距
    float SensorWidthMm; // 传感器宽
    float SensorHeightMm;// 传感器高
    float SensorOffsetX ;// 传感器偏移
    float SensorOffsetY;
    float Squeeze;       // 变形宽银幕

    // 曝光
    float FStop;
    float ShutterSpeed;
    float ISO;
    float ExposureCompensation; // 曝光补偿

    // 对焦 / 景深
    float FocusDistance;
    float CircleOfConfusion;
    int ApertureBladeCount;  // 光圈叶片数

    // 变焦
    float MinFocalLengthMm;
    float MaxFocalLengthMm;
};

struct CameraParams
{
    EProjectionType Projection { EProjectionType::None };
    EFovAxisType FovAxis { EFovAxisType::Vertical };
    EProjectionDepthMode depthMode { EProjectionDepthMode::ReversedZ }; 
    EAspectRatioPolicy AspectPolicy { EAspectRatioPolicy::ConstrainHeight };
    float FovDegrees;
    float NarZ;
    float FarZ;

    float OrthoWidth;
    float OrthoHeight;

    // 离轴 / 移轴 / VR 非对称视锥
    float LensShiftX;
    float LensShiftY;
    float OffCenterX;
    float OffCenterY;

    // 自定义裁剪
    bool EnableObliqueNearPlane { false };
    core::Vec4f ObliqueNearPlane;

    // TAA / 时序抖动
    core::Vec2f ProjectionJitter {0.0f, 0.0f};

    LensParams lens;
};
class Camera
{


};
}