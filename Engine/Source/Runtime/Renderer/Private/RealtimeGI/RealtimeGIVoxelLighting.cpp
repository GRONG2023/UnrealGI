#include "RealtimeGIVoxelLighting.h"

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "MeshPassProcessor.h"
#include "SceneRendering.h"
#include "Renderer/Private/ScenePrivate.h"
#include "SceneTextureParameters.h"

// #pragma optimize ("", off)


static TAutoConsoleVariable<int32> CVarVoxelLightingCheckerBoardSize(
	TEXT("r.RealtimeGI.VoxelLightingCheckerBoardSize"),
	2,
	TEXT("Checker board size to calculate voxel lighting (default is 2x2x2)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarFreezeLightingForDebug(
	TEXT("r.RealtimeGI.FreezeLightingForDebug"),
	0,
	TEXT("Stop voxel lighting update (for debug)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarProbeUpdateCheckerBoardSize(
	TEXT("r.RealtimeGI.ProbeGatherCheckerBoardSize"),
	2,
	TEXT("Checker board size to update far field probe (default is 2x2)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarVisualizeIrradianceProbe(
	TEXT("r.RealtimeGI.VisualizeIrradianceProbe"),
	0,
	TEXT("Visualize probe irradiance volume"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarVisualizeRadianceProbe(
	TEXT("r.RealtimeGI.VisualizeRadianceProbe"),
	0,
	TEXT("Visualize far field probe radiance"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRadianceProbeResolution(
	TEXT("r.RealtimeGI.RadianceProbeResolution"),
	16,
	TEXT("Radiance probe resolution"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarReuseRadianceProbe(
	TEXT("r.RealtimeGI.ReuseRadianceProbe"),
	0,
	TEXT("Radiance probe will used as fallback when lower clipmap level's probe trace fail"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarUseProbeOcclusionTest(
	TEXT("r.RealtimeGI.UseProbeOcclusionTest"),
	1,
	TEXT("Use probe hit depth texture to prevent light leaking"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRadianceProbeMinClipLevel(
	TEXT("r.RealtimeGI.RadianceProbeMinClipLevel"),
	2,
	TEXT("Clipmap Index >= this, will spawn radiance probe"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarIrradianceProbeMinClipLevel(
	TEXT("r.RealtimeGI.IrradianceProbeMinClipLevel"),
	0,
	TEXT("Clipmap Index >= this, will spawn irradiance probe"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarIrradianceProbeSampleNum(
	TEXT("r.RealtimeGI.IrradianceProbeSampleNum"),
	1,
	TEXT("Num rays to trace per probe (rays == 64 if value is 1)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarIrradianceProbeTemporalWeight(
	TEXT("r.RealtimeGI.IrradianceProbeTemporalWeight"),
	0.75f,
	TEXT("Weight for temporal blend in irradiance probe gather pass"),
	ECVF_RenderThreadSafe
);

TGlobalResource<StencilingGeometry::TStencilSphereVertexBuffer<18, 12, FVector4>> GSphereVertexBuffer;
TGlobalResource<StencilingGeometry::TStencilSphereIndexBuffer<18, 12>> GSphereIndexBuffer;


class FPickValidVoxelCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPickValidVoxelCS);
	SHADER_USE_PARAMETER_STRUCT(FPickValidVoxelCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER(FIntVector4, CheckerBoardInfo)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWValidVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWValidVoxelBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 4;
	static const uint32 ThreadGroupSizeY = 4;
	static const uint32 ThreadGroupSizeZ = 4;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FPickValidVoxelCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "PickValidVoxelCS", SF_Compute);

class FBuildVoxelLightingIndirectArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildVoxelLightingIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildVoxelLightingIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumThreadsForVoxelLighting)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 1;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FBuildVoxelLightingIndirectArgsCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "BuildVoxelLightingIndirectArgsCS", SF_Compute);

class FVoxelLightingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelLightingCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelLightingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
		SHADER_PARAMETER(FVector, MainLightDirection)
		SHADER_PARAMETER(FLinearColor, MainLightColor)
		SHADER_PARAMETER(int32, CSMNumCascades)
		SHADER_PARAMETER_ARRAY(FVector4, CSMShadowBounds, [4])
		SHADER_PARAMETER_ARRAY(FMatrix, CSMWorldToShadowMatrixs, [4])
		SHADER_PARAMETER_TEXTURE(Texture2D, ShadowDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ShadowDepthTextureSampler)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidVoxelBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWVoxelPoolRadiance)
		SHADER_PARAMETER_RDG_BUFFER(StructuredBuffer, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 8;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	class FUseProbeOcclusionTest : SHADER_PERMUTATION_BOOL("USE_PROBE_OCCLUSION_TEST");
	class FUseDistanceField : SHADER_PERMUTATION_BOOL("USE_DISTANCE_FIELD");
	using FPermutationDomain = TShaderPermutationDomain<FUseProbeOcclusionTest, FUseDistanceField>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelLightingCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "VoxelLightingCS", SF_Compute);

class FPickValidProbeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPickValidProbeCS);
	SHADER_USE_PARAMETER_STRUCT(FPickValidProbeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER(FIntVector4, CheckerBoardInfo)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWValidProbeCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWValidProbeBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWProbeOffsetClipmap)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 4;
	static const uint32 ThreadGroupSizeY = 4;
	static const uint32 ThreadGroupSizeZ = 4;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FPickValidProbeCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "PickValidProbeCS", SF_Compute);

class FRadianceProbeAllocateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadianceProbeAllocateCS);
	SHADER_USE_PARAMETER_STRUCT(FRadianceProbeAllocateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER(int32, MaxRadianceProbeNum)
		SHADER_PARAMETER(FIntVector4, CheckerBoardInfo)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWRadianceProbeFreeList)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWRadianceProbeReleaseList)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, ProbeOffsetClipmap)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWRadianceProbeIdClipmap)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 4;
	static const uint32 ThreadGroupSizeY = 4;
	static const uint32 ThreadGroupSizeZ = 4;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRadianceProbeAllocateCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "RadianceProbeAllocateCS", SF_Compute);

class FBuildRadianceProbeReleaseAndCaptureIndirectArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildRadianceProbeReleaseAndCaptureIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildRadianceProbeReleaseAndCaptureIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, MaxRadianceProbeNum)
		SHADER_PARAMETER(FIntVector, VolumeResolution)
		SHADER_PARAMETER(int32, RadianceProbeResolution)
		SHADER_PARAMETER(int32, NumThreadsForProbeRelease)
		SHADER_PARAMETER(FIntPoint, NumThreadsForProbeCapture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidProbeCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWNumProbesToReleaseCounter)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWRadianceProbeFreeList)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWRadianceProbeReleaseList)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWProbeReleaseIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWProbeCaptureIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWProbeOutputMergeIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 1;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FBuildRadianceProbeReleaseAndCaptureIndirectArgsCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "BuildRadianceProbeReleaseAndCaptureIndirectArgsCS", SF_Compute);

class FRadianceProbeReleaseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadianceProbeReleaseCS);
	SHADER_USE_PARAMETER_STRUCT(FRadianceProbeReleaseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, MaxRadianceProbeNum)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, NumProbesToReleaseCounter)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWRadianceProbeFreeList)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWRadianceProbeReleaseList)
		SHADER_PARAMETER_RDG_BUFFER(StructuredBuffer, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 8;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRadianceProbeReleaseCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "RadianceProbeReleaseCS", SF_Compute);

class FRadianceProbeCaptureCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadianceProbeCaptureCS);
	SHADER_USE_PARAMETER_STRUCT(FRadianceProbeCaptureCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidProbeBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWRadianceProbeAtlas)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWRadianceProbeDistanceAtlas)
		SHADER_PARAMETER_RDG_BUFFER(StructuredBuffer, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 32;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	class FUseDistanceField : SHADER_PERMUTATION_BOOL("USE_DISTANCE_FIELD");
	using FPermutationDomain = TShaderPermutationDomain<FUseDistanceField>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRadianceProbeCaptureCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "RadianceProbeCaptureCS", SF_Compute);

class FRadianceProbeOutputMergeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadianceProbeOutputMergeCS);
	SHADER_USE_PARAMETER_STRUCT(FRadianceProbeOutputMergeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidProbeBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, RadianceProbeOutput)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, RadianceProbeDistanceOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWRadianceProbeAtlas)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWRadianceProbeDistanceAtlas)
		SHADER_PARAMETER_RDG_BUFFER(StructuredBuffer, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = FRadianceProbeCaptureCS::ThreadGroupSizeX;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRadianceProbeOutputMergeCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "RadianceProbeOutputMergeCS", SF_Compute);

class FBuildIrradianceProbeGatherIndirectArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildIrradianceProbeGatherIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildIrradianceProbeGatherIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidProbeCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 1;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FBuildIrradianceProbeGatherIndirectArgsCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "BuildIrradianceProbeGatherIndirectArgsCS", SF_Compute);

class FRadianceToIrradianceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRadianceToIrradianceCS);
	SHADER_USE_PARAMETER_STRUCT(FRadianceToIrradianceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidProbeBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWIrradianceProbeClipmap)
		SHADER_PARAMETER_RDG_BUFFER(StructuredBuffer, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 64;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRadianceToIrradianceCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "RadianceToIrradianceCS", SF_Compute);

class FIrradianceProbeGatherCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FIrradianceProbeGatherCS);
	SHADER_USE_PARAMETER_STRUCT(FIrradianceProbeGatherCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
		SHADER_PARAMETER(int32, NumSamples)
		SHADER_PARAMETER(float, TemporalWeight)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ValidProbeBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWIrradianceProbeClipmap)
		SHADER_PARAMETER_RDG_BUFFER(StructuredBuffer, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 64;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	class FSampleRadianceProbe : SHADER_PERMUTATION_BOOL("USE_RADIANCE_PROBE_AS_FALLBACK");
	class FUseProbeOcclusionTest : SHADER_PERMUTATION_BOOL("USE_PROBE_OCCLUSION_TEST");
	class FUseDistanceField : SHADER_PERMUTATION_BOOL("USE_DISTANCE_FIELD");
	using FPermutationDomain = TShaderPermutationDomain<FSampleRadianceProbe, FUseProbeOcclusionTest, FUseDistanceField>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FIrradianceProbeGatherCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "IrradianceProbeGatherCS", SF_Compute);

class FVisualizeProbeVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVisualizeProbeVS);
	SHADER_USE_PARAMETER_STRUCT(FVisualizeProbeVS, FGlobalShader);
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, VisualizeMode)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};
IMPLEMENT_GLOBAL_SHADER(FVisualizeProbeVS, "/Engine/Private/RealtimeGI/VisualizeProbe.usf", "VisualizeProbeVS", SF_Vertex);

class FVisualizeProbePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVisualizeProbePS);
	SHADER_USE_PARAMETER_STRUCT(FVisualizeProbePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVisualizeProbeVS::FParameters, Common)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};
IMPLEMENT_GLOBAL_SHADER(FVisualizeProbePS, "/Engine/Private/RealtimeGI/VisualizeProbe.usf", "VisualizeProbePS", SF_Pixel);

class FClearDirtyVoxelRadianceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearDirtyVoxelRadianceCS);
	SHADER_USE_PARAMETER_STRUCT(FClearDirtyVoxelRadianceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER(FIntVector, UpdateChunkResolution)
		SHADER_PARAMETER_SRV(StructuredBuffer, UpdateChunkCleanupList)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWVoxelPoolRadiance)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 4;
	static const uint32 ThreadGroupSizeY = 4;
	static const uint32 ThreadGroupSizeZ = 4;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FClearDirtyVoxelRadianceCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "ClearDirtyVoxelRadianceCS", SF_Compute);

class FClearDirtyProbeIrradianceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClearDirtyProbeIrradianceCS);
	SHADER_USE_PARAMETER_STRUCT(FClearDirtyProbeIrradianceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER(FIntVector, UpdateChunkResolution)
		SHADER_PARAMETER_SRV(StructuredBuffer, UpdateChunkCleanupList)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWIrradianceProbeClipmap)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 4;
	static const uint32 ThreadGroupSizeY = 4;
	static const uint32 ThreadGroupSizeZ = 4;

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Z"), ThreadGroupSizeZ);
	}
};
IMPLEMENT_GLOBAL_SHADER(FClearDirtyProbeIrradianceCS, "/Engine/Private/RealtimeGI/VoxelLighting.usf", "ClearDirtyProbeIrradianceCS", SF_Compute);


void GetMainLightShadowInfos(const FLightSceneProxy* LightProxy, const FVisibleLightInfo& VisibleLightInfo, 
	TArray<FMatrix>& OutWorldToShadowMatrixs, TArray<FSphere>& OutShadowBounds, FRHITexture*& OutShadowDepthTexture)
{
	TArray<const FProjectedShadowInfo*> ShadowInfos;

	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		const FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];

		if (ProjectedShadowInfo->bAllocated
			&& ProjectedShadowInfo->bWholeSceneShadow
			&& !ProjectedShadowInfo->bRayTracedDistanceField)
		{
			ShadowInfos.Add(ProjectedShadowInfo);

			FVector4 ShadowmapMinMax;
			FMatrix WorldToShadowMatrix = ProjectedShadowInfo->GetWorldToShadowMatrix(ShadowmapMinMax);
			OutWorldToShadowMatrixs.Add(WorldToShadowMatrix);

			OutShadowBounds.Add(ProjectedShadowInfo->ShadowBounds);
		}
	}

	if (ShadowInfos.Num() > 0)
	{
		OutShadowDepthTexture = ShadowInfos[0]->RenderTargets.DepthTarget->GetRenderTargetItem().ShaderResourceTexture.GetReference();
	}

	Algo::Reverse(OutWorldToShadowMatrixs);
	Algo::Reverse(OutShadowBounds);
}

FProbeVolumeParameters FRealtimeGIRadianceCache::SetupProbeVolumeParameters(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FProbeVolumeParameters PassParameters;
	PassParameters.RadianceProbeResolution = RadianceProbeResolution;
	PassParameters.NumRadianceProbesInAtlasXY = NumRadianceProbesInAtlasXY;
	PassParameters.RadianceProbeMinClipLevel = CVarRadianceProbeMinClipLevel.GetValueOnRenderThread();
	PassParameters.IrradianceProbeMinClipLevel = CVarIrradianceProbeMinClipLevel.GetValueOnRenderThread();
	PassParameters.ProbeOffsetClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ProbeOffsetClipmap.RDGTexture));
	PassParameters.IrradianceProbeClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(IrradianceProbeClipmap.RDGTexture));
	PassParameters.RadianceProbeIdClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(RadianceProbeIdClipmap.RDGTexture));
	PassParameters.RadianceProbeAtlas = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(RadianceProbeAtlas.RDGTexture));
	PassParameters.RadianceProbeDistanceAtlas = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(RadianceProbeDistanceAtlas.RDGTexture));

	return PassParameters;
}

void FRealtimeGIRadianceCache::Update(FRDGBuilder& GraphBuilder, FSceneRenderer& SceneRenderer, FScene* Scene, FViewInfo& View)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;

	PrepareRenderResources(GraphBuilder, Scene, View);

	const int32 ClipIdMax = VoxelClipmap->NumClips - 1;
	for (int32 ClipId = ClipIdMax; ClipId >= 0; ClipId -= 1)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "FRealtimeGIRadianceCacheUpdate_Clip%d", ClipId);

		// skip update lighting
		if (CVarFreezeLightingForDebug.GetValueOnRenderThread() > 0)
		{
			continue;
		}

		CleanupDirtyUpdateChunk(GraphBuilder, Scene, View, ClipId);

		PickValidVoxel(GraphBuilder, Scene, View, ClipId);

		VoxelLighting(GraphBuilder, SceneRenderer, Scene, View, ClipId);

		PickValidProbe(GraphBuilder, Scene, View, ClipId);

		/*
		if (ClipId >= CVarRadianceProbeMinClipLevel.GetValueOnRenderThread())
		{
			RadianceProbeCapture(GraphBuilder, Scene, View, ClipId);
			RadianceToIrradiance(GraphBuilder, Scene, View, ClipId);
		}
		else
		*/
		if (ClipId >= CVarIrradianceProbeMinClipLevel.GetValueOnRenderThread())
		{
			IrradianceProbeGather(GraphBuilder, Scene, View, ClipId);
		}
	}

}

void FRealtimeGIRadianceCache::PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;

	const FIntVector NumPagesInXYZ = View.RealtimeGIVoxelClipmap->NumVoxelPagesInXYZ;
	FIntVector PoolSize = NumPagesInXYZ * VOXEL_BLOCK_SIZE;
	PoolSize.Z *= 2;	// for two side voxel
	Create3DTexFn(GraphBuilder, VoxelPoolRadiance, PoolSize, PF_FloatRGB, TEXT("VoxelPoolRadiance"));

	ValidVoxelCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
		TEXT("ValidVoxelCounter")
	);

	const FIntVector NumBlocksInXYZ = VoxelClipmap->VolumeResolution / VOXEL_BLOCK_SIZE;
	int32 NumBlocksToLight1D = NumBlocksInXYZ.X * NumBlocksInXYZ.Y * NumBlocksInXYZ.Z;
	ValidVoxelBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), NumBlocksToLight1D * NUM_VOXEL_PER_BLOCK),
		TEXT("ValidVoxelBuffer")
	);

	VoxelLightingIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("VoxelLightingIndirectArgs")
	);

	ValidProbeCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
		TEXT("ValidProbeCounter")
	);

	ValidProbeBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), NumBlocksToLight1D),
		TEXT("ValidProbeBuffer")
	);

	IrradianceProbeGatherIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("IrradianceProbeGatherIndirectArgs")
	);

	const FIntVector NumProbesInXYZ = NumBlocksInXYZ;
	FIntVector ClipmapResolution = NumProbesInXYZ;
	ClipmapResolution.Z *= VoxelClipmap->NumClips;	// for clipmap

	FIntVector IrradianceVolumeResolution = ClipmapResolution;
	IrradianceVolumeResolution.X *= 7;	// for SH9

	Create3DTexFn(GraphBuilder, IrradianceProbeClipmap, IrradianceVolumeResolution, PF_FloatRGBA, TEXT("IrradianceProbeClipmap"));
	Create3DTexFn(GraphBuilder, ProbeOffsetClipmap, ClipmapResolution, PF_B8G8R8A8, TEXT("ProbeOffsetClipmap"));

	RadianceProbeResolution = CVarRadianceProbeResolution.GetValueOnRenderThread();
	FIntPoint AtlasResolution = NumRadianceProbesInAtlasXY * (RadianceProbeResolution + 2);
	Create2DTexFn(GraphBuilder, RadianceProbeAtlas, AtlasResolution, PF_FloatRGB, TEXT("RadianceProbeAtlas"));
	Create2DTexFn(GraphBuilder, RadianceProbeDistanceAtlas, AtlasResolution, PF_R16F, TEXT("RadianceProbeDistanceAtlas"));
	Create2DTexFn(GraphBuilder, RadianceProbeOutput, AtlasResolution, PF_FloatRGB, TEXT("RadianceProbeOutput"));
	Create2DTexFn(GraphBuilder, RadianceProbeDistanceOutput, AtlasResolution, PF_R16F, TEXT("RadianceProbeDistanceOutput"));

	bool NeedResetVolume = Create3DTexFn(GraphBuilder, RadianceProbeIdClipmap, ClipmapResolution, PF_R32_UINT, TEXT("RadianceProbeIdClipmap"));
	if (NeedResetVolume)
	{
		InitTexture3D(GraphBuilder, RadianceProbeIdClipmap, ClipmapResolution, PROBE_ID_INVALID);
	}

	// radiance probe free list
	const int32 NumRadianceProbes = NumRadianceProbesInAtlasXY.X * NumRadianceProbesInAtlasXY.Y;
	const int32 NumElementsFreeList = NumRadianceProbes + 1;	// we use last element as allocator pointer
	const int32 NumBytesFreeList = sizeof(uint32) * NumElementsFreeList;
	if (RadianceProbeFreeList.NumBytes == 0 || NeedResetVolume)
	{
		RadianceProbeFreeList.Initialize(
			sizeof(int32), NumElementsFreeList,
			BUF_Static, TEXT("RadianceProbeFreeList")
		);

		TArray<int32> InitData;
		InitData.SetNum(NumElementsFreeList);
		for (int32 i = 0; i < NumRadianceProbes; i++)
		{
			InitData[i] = i;
		}

		FRHIStructuredBuffer* BufferRHI = RadianceProbeFreeList.Buffer;
		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytesFreeList, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, InitData.GetData(), NumBytesFreeList);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}

	if (RadianceProbeReleaseList.NumBytes == 0)
	{
		RadianceProbeReleaseList.Initialize(
			sizeof(int32), NumElementsFreeList,
			BUF_Static, TEXT("RadianceProbeReleaseList")
		);
	}

	RadianceProbeReleaseIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("RadianceProbeReleaseIndirectArgs")
	);

	RadianceProbeCaptureIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("RadianceProbeCaptureIndirectArgs")
	);

	RadianceProbeOutputMergeIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("RadianceProbeOutputMergeIndirectArgs")
	);
}

void FRealtimeGIRadianceCache::PickValidVoxel(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	ClearCounterBuffer(GraphBuilder, Scene, ValidVoxelCounter, 1);

	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const FRealtimeGIVolumeInfo& ClipmapInfo = VoxelClipmap->ClipmapInfos[ClipId];

	int32& CheckerBoardSize = VoxelLightingCheckerBoardSize;
	CheckerBoardSize = CVarVoxelLightingCheckerBoardSize.GetValueOnRenderThread();
	CheckerBoardSize = FMath::RoundUpToPowerOfTwo(CheckerBoardSize);
	CheckerBoardSize = FMath::Clamp(CheckerBoardSize, 1, 4);

	FIntVector NumBlocksInXYZ = VoxelClipmap->VolumeResolution / VOXEL_BLOCK_SIZE;
	FIntVector NumBlocksToLightInXYZ = NumBlocksInXYZ / CheckerBoardSize;
	int32 MaxFrameNum = CheckerBoardSize * CheckerBoardSize * CheckerBoardSize;
	FIntVector CheckerBoardOffset = Index1DTo3DLinear(FrameNumberRenderThread % MaxFrameNum, FIntVector(CheckerBoardSize, CheckerBoardSize, CheckerBoardSize));

	TShaderMapRef<FPickValidVoxelCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FPickValidVoxelCS::FParameters>();
	PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
	PassParameters->CheckerBoardInfo = FIntVector4(CheckerBoardOffset.X, CheckerBoardOffset.Y, CheckerBoardOffset.Z, CheckerBoardSize);
	PassParameters->RWValidVoxelCounter = GraphBuilder.CreateUAV(ValidVoxelCounter);
	PassParameters->RWValidVoxelBuffer = GraphBuilder.CreateUAV(ValidVoxelBuffer);

	// one thread for one block
	const FIntVector NumGroups = FIntVector(
		NumBlocksToLightInXYZ.X / FPickValidVoxelCS::ThreadGroupSizeX,
		NumBlocksToLightInXYZ.Y / FPickValidVoxelCS::ThreadGroupSizeY,
		NumBlocksToLightInXYZ.Z / FPickValidVoxelCS::ThreadGroupSizeZ
	);

	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("PickValidVoxel"),
		ComputeShader, PassParameters, NumGroups
	);
}

void FRealtimeGIRadianceCache::VoxelLighting(FRDGBuilder& GraphBuilder, FSceneRenderer& SceneRenderer, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const TArray<FRealtimeGIVolumeInfo>& ClipmapInfos = VoxelClipmap->ClipmapInfos;

	// get main light
	const FLightSceneInfo* LightInfo = Scene->SimpleDirectionalLight;
	const FLightSceneProxy* MainLight = LightInfo ? LightInfo->Proxy : nullptr;
	FVector MainLightDirection = MainLight ? MainLight->GetDirection() * -1 : FVector(0, 0, 0);
	FLinearColor MainLightColor = MainLight ? MainLight->GetColor() : FLinearColor(0, 0, 0, 0);

	// get main light shadow
	TArray<FMatrix> WorldToShadowMatrixs;
	FRHITexture* ShadowDepthTexture = GWhiteTexture->TextureRHI->GetTexture2D();
	TArray<FSphere> ShadowBounds;
	if (MainLight)
	{
		GetMainLightShadowInfos(
			MainLight, SceneRenderer.VisibleLightInfos[LightInfo->Id], 
			WorldToShadowMatrixs, ShadowBounds, ShadowDepthTexture
		);
	}

	// 1. build indirect args cause valid voxel num is unpredictable
	{
		TShaderMapRef<FBuildVoxelLightingIndirectArgsCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FBuildVoxelLightingIndirectArgsCS::FParameters>();
		PassParameters->NumThreadsForVoxelLighting = FVoxelLightingCS::ThreadGroupSizeX;
		PassParameters->ValidVoxelCounter = GraphBuilder.CreateSRV(ValidVoxelCounter);
		PassParameters->RWIndirectArgs = GraphBuilder.CreateUAV(VoxelLightingIndirectArgs);

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("BuildVoxelLightingIndirectArgs"),
			ComputeShader, PassParameters, FIntVector(1, 1, 1));
	}

	// 2. calculate lighting for each voxel
	{
		bool UseProbeOcclusionTest = CVarUseProbeOcclusionTest.GetValueOnRenderThread() > 0 ? 1 : 0;
		FVoxelLightingCS::FPermutationDomain Permutation;
		Permutation.Set<FVoxelLightingCS::FUseProbeOcclusionTest>(UseProbeOcclusionTest);
		Permutation.Set<FVoxelLightingCS::FUseDistanceField>(RealtimeGIUseDistanceField());

		TShaderMapRef<FVoxelLightingCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
		auto* PassParameters = GraphBuilder.AllocParameters<FVoxelLightingCS::FParameters>();

		// voxel RT
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ProbeVolumeParameters = SetupProbeVolumeParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ValidVoxelCounter = GraphBuilder.CreateSRV(ValidVoxelCounter);
		PassParameters->ValidVoxelBuffer = GraphBuilder.CreateSRV(ValidVoxelBuffer);

		// light & shadow
		PassParameters->MainLightDirection = MainLightDirection;
		PassParameters->MainLightColor = MainLightColor;
		PassParameters->CSMNumCascades = WorldToShadowMatrixs.Num();
		for (int32 CascadeIndex = 0; CascadeIndex < WorldToShadowMatrixs.Num(); CascadeIndex++)
		{
			PassParameters->CSMWorldToShadowMatrixs[CascadeIndex] = WorldToShadowMatrixs[CascadeIndex];
			PassParameters->CSMShadowBounds[CascadeIndex] = FVector4(ShadowBounds[CascadeIndex].Center, ShadowBounds[CascadeIndex].W);
		}
		PassParameters->ShadowDepthTexture = ShadowDepthTexture;
		PassParameters->ShadowDepthTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// out
		PassParameters->RWVoxelPoolRadiance = GraphBuilder.CreateUAV(VoxelPoolRadiance.RDGTexture);
		PassParameters->IndirectArgsBuffer = VoxelLightingIndirectArgs;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("VoxelLighting"),
			ComputeShader, PassParameters, VoxelLightingIndirectArgs, 0);
	}
}

void FRealtimeGIRadianceCache::PickValidProbe(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	ClearCounterBuffer(GraphBuilder, Scene, ValidProbeCounter, 1);

	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const FRealtimeGIVolumeInfo& ClipmapInfo = VoxelClipmap->ClipmapInfos[ClipId];
	
	int32& CheckerBoardSize = ProbeUpdateCheckerBoardSize;
	CheckerBoardSize = CVarProbeUpdateCheckerBoardSize.GetValueOnRenderThread();
	CheckerBoardSize = FMath::RoundUpToPowerOfTwo(CheckerBoardSize);
	CheckerBoardSize = FMath::Clamp(CheckerBoardSize, 1, 4);

	// one thread for one block, one block for one probe
	FIntVector NumProbesInXYZ = VoxelClipmap->VolumeResolution / VOXEL_BLOCK_SIZE;
	int32 MaxFrameNum = CheckerBoardSize * CheckerBoardSize * CheckerBoardSize;
	FIntVector CheckerBoardOffset = Index1DTo3DLinear(FrameNumberRenderThread % MaxFrameNum, FIntVector(CheckerBoardSize, CheckerBoardSize, CheckerBoardSize));

	TShaderMapRef<FPickValidProbeCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FPickValidProbeCS::FParameters>();
	PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
	PassParameters->CheckerBoardInfo = FIntVector4(CheckerBoardOffset.X, CheckerBoardOffset.Y, CheckerBoardOffset.Z, CheckerBoardSize);
	PassParameters->RWValidProbeCounter = GraphBuilder.CreateUAV(ValidProbeCounter);
	PassParameters->RWValidProbeBuffer = GraphBuilder.CreateUAV(ValidProbeBuffer);
	PassParameters->RWProbeOffsetClipmap = GraphBuilder.CreateUAV(ProbeOffsetClipmap.RDGTexture);

	FIntVector NumProbesToUpdateInXYZ = NumProbesInXYZ / CheckerBoardSize;
	const FIntVector NumGroups = FIntVector(
		NumProbesToUpdateInXYZ.X / FPickValidProbeCS::ThreadGroupSizeX,
		NumProbesToUpdateInXYZ.Y / FPickValidProbeCS::ThreadGroupSizeY,
		NumProbesToUpdateInXYZ.Z / FPickValidProbeCS::ThreadGroupSizeZ
	);

	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("PickValidProbe"),
		ComputeShader, PassParameters, NumGroups
	);
}

void FRealtimeGIRadianceCache::RadianceProbeCapture(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RadianceProbeCapture");

	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const TArray<FRealtimeGIVolumeInfo>& ClipmapInfos = VoxelClipmap->ClipmapInfos;
	const FRealtimeGIVolumeInfo& ClipmapInfo = VoxelClipmap->ClipmapInfos[ClipId];

	int32& CheckerBoardSize = ProbeUpdateCheckerBoardSize;
	int32 MaxFrameNum = CheckerBoardSize * CheckerBoardSize * CheckerBoardSize;
	FIntVector CheckerBoardOffset = Index1DTo3DLinear(FrameNumberRenderThread % MaxFrameNum, FIntVector(CheckerBoardSize, CheckerBoardSize, CheckerBoardSize));

	int32 MaxRadianceProbeNum = NumRadianceProbesInAtlasXY.X * NumRadianceProbesInAtlasXY.Y;
	FIntVector NumProbesInXYZ = VoxelClipmap->VolumeResolution / VOXEL_BLOCK_SIZE;
	FIntVector NumProbesToUpdateInXYZ = NumProbesInXYZ / CheckerBoardSize;

	FRDGBufferRef NumProbesToReleaseCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
		TEXT("NumProbesToReleaseCounter")
	);

	// 1. allocate atlas space for radiance probe
	{
		TShaderMapRef<FRadianceProbeAllocateCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FRadianceProbeAllocateCS::FParameters>();
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->CheckerBoardInfo = FIntVector4(CheckerBoardOffset.X, CheckerBoardOffset.Y, CheckerBoardOffset.Z, CheckerBoardSize);
		PassParameters->MaxRadianceProbeNum = MaxRadianceProbeNum;

		PassParameters->RWRadianceProbeFreeList = RadianceProbeFreeList.UAV;
		PassParameters->RWRadianceProbeReleaseList = RadianceProbeReleaseList.UAV;

		PassParameters->ProbeOffsetClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ProbeOffsetClipmap.RDGTexture));
		PassParameters->RWRadianceProbeIdClipmap = GraphBuilder.CreateUAV(RadianceProbeIdClipmap.RDGTexture);

		const FIntVector NumGroups = FIntVector(
			NumProbesToUpdateInXYZ.X / FRadianceProbeAllocateCS::ThreadGroupSizeX,
			NumProbesToUpdateInXYZ.Y / FRadianceProbeAllocateCS::ThreadGroupSizeY,
			NumProbesToUpdateInXYZ.Z / FRadianceProbeAllocateCS::ThreadGroupSizeZ
		);

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("RadianceProbeAllocate"),
			ComputeShader, PassParameters, NumGroups
		);
	}

	// 2. build indirect args for probe release and probe capture
	{
		TShaderMapRef<FBuildRadianceProbeReleaseAndCaptureIndirectArgsCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FBuildRadianceProbeReleaseAndCaptureIndirectArgsCS::FParameters>();
		PassParameters->MaxRadianceProbeNum = MaxRadianceProbeNum;
		PassParameters->VolumeResolution = VoxelClipmap->VolumeResolution;
		PassParameters->RadianceProbeResolution = RadianceProbeResolution;
		PassParameters->NumThreadsForProbeRelease = FRadianceProbeReleaseCS::ThreadGroupSizeX;
		PassParameters->NumThreadsForProbeCapture = FIntPoint(FRadianceProbeCaptureCS::ThreadGroupSizeX, FRadianceProbeCaptureCS::ThreadGroupSizeY);
		PassParameters->ValidProbeCounter = GraphBuilder.CreateSRV(ValidProbeCounter);
		PassParameters->RWNumProbesToReleaseCounter = GraphBuilder.CreateUAV(NumProbesToReleaseCounter);
		PassParameters->RWRadianceProbeFreeList = RadianceProbeFreeList.UAV;
		PassParameters->RWRadianceProbeReleaseList = RadianceProbeReleaseList.UAV;
		PassParameters->RWProbeReleaseIndirectArgs = GraphBuilder.CreateUAV(RadianceProbeReleaseIndirectArgs);
		PassParameters->RWProbeCaptureIndirectArgs = GraphBuilder.CreateUAV(RadianceProbeCaptureIndirectArgs);
		PassParameters->RWProbeOutputMergeIndirectArgs = GraphBuilder.CreateUAV(RadianceProbeOutputMergeIndirectArgs);

		const FIntVector NumGroups = FIntVector(1, 1, 1);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("BuildRadianceProbeReleaseAndCaptureIndirectArgs"),
			ComputeShader, PassParameters, NumGroups
		);
	}

	// 3. probe release
	{
		TShaderMapRef<FRadianceProbeReleaseCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FRadianceProbeReleaseCS::FParameters>();
		PassParameters->MaxRadianceProbeNum = MaxRadianceProbeNum;
		PassParameters->NumProbesToReleaseCounter = GraphBuilder.CreateSRV(NumProbesToReleaseCounter);
		PassParameters->RWRadianceProbeFreeList = RadianceProbeFreeList.UAV;
		PassParameters->RWRadianceProbeReleaseList = RadianceProbeReleaseList.UAV;
		PassParameters->IndirectArgsBuffer = RadianceProbeReleaseIndirectArgs;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("RadianceProbeRelease"),
			ComputeShader, PassParameters, RadianceProbeReleaseIndirectArgs, 0
		);
	}
	
	// 4. probe capture
	{
		FRadianceProbeCaptureCS::FPermutationDomain Permutation;
		Permutation.Set<FRadianceProbeCaptureCS::FUseDistanceField>(RealtimeGIUseDistanceField());

		TShaderMapRef<FRadianceProbeCaptureCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
		auto* PassParameters = GraphBuilder.AllocParameters<FRadianceProbeCaptureCS::FParameters>();
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ProbeVolumeParameters = SetupProbeVolumeParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ValidProbeBuffer = GraphBuilder.CreateSRV(ValidProbeBuffer);
		PassParameters->RWRadianceProbeAtlas = GraphBuilder.CreateUAV(RadianceProbeOutput.RDGTexture);
		PassParameters->RWRadianceProbeDistanceAtlas = GraphBuilder.CreateUAV(RadianceProbeDistanceOutput.RDGTexture);
		PassParameters->IndirectArgsBuffer = RadianceProbeCaptureIndirectArgs;

		FIntVector NumPixelsToUpdateInXYZ = NumProbesToUpdateInXYZ;
		NumPixelsToUpdateInXYZ.X *= RadianceProbeResolution;
		NumPixelsToUpdateInXYZ.Y *= RadianceProbeResolution;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("RadianceProbeRaytracing"),
			ComputeShader, PassParameters, RadianceProbeCaptureIndirectArgs, 0
		);
	}

	// 5. output merge
	{
		TShaderMapRef<FRadianceProbeOutputMergeCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FRadianceProbeOutputMergeCS::FParameters>();
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ProbeVolumeParameters = SetupProbeVolumeParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ValidProbeBuffer = GraphBuilder.CreateSRV(ValidProbeBuffer);
		PassParameters->RadianceProbeOutput = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(RadianceProbeOutput.RDGTexture));
		PassParameters->RadianceProbeDistanceOutput = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(RadianceProbeDistanceOutput.RDGTexture));
		PassParameters->RWRadianceProbeAtlas = GraphBuilder.CreateUAV(RadianceProbeAtlas.RDGTexture);
		PassParameters->RWRadianceProbeDistanceAtlas = GraphBuilder.CreateUAV(RadianceProbeDistanceAtlas.RDGTexture);
		PassParameters->IndirectArgsBuffer = RadianceProbeOutputMergeIndirectArgs;

		FIntVector NumPixelsToUpdateInXYZ = NumProbesToUpdateInXYZ;
		NumPixelsToUpdateInXYZ.X *= RadianceProbeResolution;
		NumPixelsToUpdateInXYZ.Y *= RadianceProbeResolution;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("RadianceProbeOutputMerge"),
			ComputeShader, PassParameters, RadianceProbeOutputMergeIndirectArgs, 0
		);
	}
}

void FRealtimeGIRadianceCache::RadianceToIrradiance(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const TArray<FRealtimeGIVolumeInfo>& ClipmapInfos = VoxelClipmap->ClipmapInfos;

	// 1. build indirect args
	{
		TShaderMapRef<FBuildIrradianceProbeGatherIndirectArgsCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FBuildIrradianceProbeGatherIndirectArgsCS::FParameters>();
		PassParameters->ValidProbeCounter = GraphBuilder.CreateSRV(ValidProbeCounter);
		PassParameters->RWIndirectArgs = GraphBuilder.CreateUAV(IrradianceProbeGatherIndirectArgs);

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("BuildRadianceToIrradianceIndirectArgs"),
			ComputeShader, PassParameters, FIntVector(1, 1, 1));
	}

	// 2. 
	{
		TShaderMapRef<FRadianceToIrradianceCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FRadianceToIrradianceCS::FParameters>();

		// voxel RT
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ProbeVolumeParameters = SetupProbeVolumeParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ValidProbeBuffer = GraphBuilder.CreateSRV(ValidProbeBuffer);

		// out
		PassParameters->RWIrradianceProbeClipmap = GraphBuilder.CreateUAV(IrradianceProbeClipmap.RDGTexture);
		PassParameters->IndirectArgsBuffer = IrradianceProbeGatherIndirectArgs;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("RadianceToIrradiance"),
			ComputeShader, PassParameters, IrradianceProbeGatherIndirectArgs, 0
		);
	}
}

void FRealtimeGIRadianceCache::IrradianceProbeGather(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const TArray<FRealtimeGIVolumeInfo>& ClipmapInfos = VoxelClipmap->ClipmapInfos;

	// 1. build indirect args
	{
		TShaderMapRef<FBuildIrradianceProbeGatherIndirectArgsCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FBuildIrradianceProbeGatherIndirectArgsCS::FParameters>();
		PassParameters->ValidProbeCounter = GraphBuilder.CreateSRV(ValidProbeCounter);
		PassParameters->RWIndirectArgs = GraphBuilder.CreateUAV(IrradianceProbeGatherIndirectArgs);

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("BuildIrradianceProbeGatherIndirectArgs"),
			ComputeShader, PassParameters, FIntVector(1, 1, 1));
	}

	// 2. probe gather irradiance
	// note: one group for one probe, one thread for one ray
	{
		bool SampleRadianceProbe = CVarReuseRadianceProbe.GetValueOnRenderThread() > 0 ? 1 : 0;
		bool UseProbeOcclusionTest = CVarUseProbeOcclusionTest.GetValueOnRenderThread() > 0 ? 1 : 0;

		FIrradianceProbeGatherCS::FPermutationDomain Permutation;
		Permutation.Set<FIrradianceProbeGatherCS::FSampleRadianceProbe>(false);		// SampleRadianceProbe
		Permutation.Set<FIrradianceProbeGatherCS::FUseProbeOcclusionTest>(false);	// UseProbeOcclusionTest
		Permutation.Set<FIrradianceProbeGatherCS::FUseDistanceField>(RealtimeGIUseDistanceField());

		TShaderMapRef<FIrradianceProbeGatherCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
		auto* PassParameters = GraphBuilder.AllocParameters<FIrradianceProbeGatherCS::FParameters>();

		// voxel RT
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ProbeVolumeParameters = SetupProbeVolumeParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->ValidProbeBuffer = GraphBuilder.CreateSRV(ValidProbeBuffer);
		
		// pass
		PassParameters->NumSamples = FMath::Clamp(CVarIrradianceProbeSampleNum.GetValueOnRenderThread(), 0, 4);
		PassParameters->TemporalWeight = FMath::Clamp(CVarIrradianceProbeTemporalWeight.GetValueOnRenderThread(), 0.0f, 1.0f);
		PassParameters->RWIrradianceProbeClipmap = GraphBuilder.CreateUAV(IrradianceProbeClipmap.RDGTexture);
		PassParameters->IndirectArgsBuffer = IrradianceProbeGatherIndirectArgs;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("IrradianceProbeGather"),
			ComputeShader, PassParameters, IrradianceProbeGatherIndirectArgs, 0
		);
	}
}

void FRealtimeGIRadianceCache::CleanupDirtyUpdateChunk(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const FRealtimeGIVolumeInfo& ClipmapInfo = VoxelClipmap->ClipmapInfos[ClipId];
	const int32 NumDirtyChunks = ClipmapInfo.ChunksToCleanup.Num();

#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (NumDirtyChunks == 0)
	{
		return;
	}
#endif

	// 1. clear voxel radiance
	{
		TShaderMapRef<FClearDirtyVoxelRadianceCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FClearDirtyVoxelRadianceCS::FParameters>();
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->UpdateChunkResolution = ClipmapInfo.UpdateChunkResolution;
		PassParameters->UpdateChunkCleanupList = VoxelClipmap->UpdateChunkCleanupList[ClipId].SRV;
		PassParameters->RWVoxelPoolRadiance = GraphBuilder.CreateUAV(VoxelPoolRadiance.RDGTexture);

		// one thread for one voxel
		FIntVector NumGroups = FIntVector(
			ClipmapInfo.UpdateChunkResolution.X / FClearDirtyVoxelRadianceCS::ThreadGroupSizeX,
			ClipmapInfo.UpdateChunkResolution.Y / FClearDirtyVoxelRadianceCS::ThreadGroupSizeY,
			ClipmapInfo.UpdateChunkResolution.Z / FClearDirtyVoxelRadianceCS::ThreadGroupSizeZ
		);
		NumGroups.X *= NumDirtyChunks;	// we flatten chunks in x

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("ClearDirtyVoxelRadiance"),
			ComputeShader, PassParameters, NumGroups
		);
	}

	// 2. clear irradiance probe
	{
		TShaderMapRef<FClearDirtyProbeIrradianceCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FClearDirtyProbeIrradianceCS::FParameters>();
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
		PassParameters->UpdateChunkResolution = ClipmapInfo.UpdateChunkResolution;
		PassParameters->UpdateChunkCleanupList = VoxelClipmap->UpdateChunkCleanupList[ClipId].SRV;
		PassParameters->RWIrradianceProbeClipmap = GraphBuilder.CreateUAV(IrradianceProbeClipmap.RDGTexture);

		// one thread for one block (probe)
		FIntVector NumGroups = FIntVector(
			(ClipmapInfo.UpdateChunkResolution.X / VOXEL_BLOCK_SIZE) / FClearDirtyProbeIrradianceCS::ThreadGroupSizeX,
			(ClipmapInfo.UpdateChunkResolution.Y / VOXEL_BLOCK_SIZE) / FClearDirtyProbeIrradianceCS::ThreadGroupSizeY,
			(ClipmapInfo.UpdateChunkResolution.Z / VOXEL_BLOCK_SIZE) / FClearDirtyProbeIrradianceCS::ThreadGroupSizeZ
		);
		NumGroups.X *= NumDirtyChunks;	// we flatten chunks in x

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("ClearDirtyProbeIrradiance"),
			ComputeShader, PassParameters, NumGroups
		);
	}

}

void FRealtimeGIRadianceCache::VisualizeProbe(
	FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture, EProbeVisualizeMode VisualizeMode
)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	const TArray<FRealtimeGIVolumeInfo>& ClipmapInfos = VoxelClipmap->ClipmapInfos;

	int32 ClipId = VisualizeMode == VM_IrradianceProbe
		? CVarVisualizeIrradianceProbe.GetValueOnRenderThread() - 1
		: CVarVisualizeRadianceProbe.GetValueOnRenderThread() - 1;

	if (ClipId < 0)
	{
		return;
	}

	auto* PassParameters = GraphBuilder.AllocParameters<FVisualizeProbePS::FParameters>();
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilNop);

	PassParameters->Common.VisualizeMode = (VisualizeMode == VM_IrradianceProbe) ? 0 : 1;
	PassParameters->Common.VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View, ClipId);
	PassParameters->Common.ProbeVolumeParameters = SetupProbeVolumeParameters(GraphBuilder, Scene, View, ClipId);

	TShaderMapRef<FVisualizeProbeVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FVisualizeProbePS> PixelShader(View.ShaderMap);

	const FIntVector NumProbesInXYZ = VoxelClipmap->VolumeResolution / VOXEL_BLOCK_SIZE;
	const FIntPoint ViewRect = View.ViewRect.Max - View.ViewRect.Min;
	const int32 NumInstance = NumProbesInXYZ.X * NumProbesInXYZ.Y * NumProbesInXYZ.Z;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VisualizeProbe_Clip%d", ClipId),
		PassParameters,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, PassParameters, NumInstance, ViewRect](FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

		RHICmdList.SetViewport(0, 0, 0.0f, ViewRect.X, ViewRect.Y, 1.0f);
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
		GraphicsPSOInit.BlendState = TStaticBlendStateWriteMask<CW_RGB, CW_RGBA>::GetRHI();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);
		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->Common);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

		RHICmdList.SetStreamSource(0, GSphereVertexBuffer.VertexBufferRHI, 0);
		RHICmdList.DrawIndexedPrimitive(
			GSphereIndexBuffer.IndexBufferRHI, 0, 0,
			GSphereVertexBuffer.GetVertexCount(), 0,
			GSphereIndexBuffer.GetIndexCount() / 3,
			NumInstance
		);
	});
}

extern void RealtimeGIVoxelLighting(FRDGBuilder& GraphBuilder, FSceneRenderer& SceneRenderer, FScene* Scene, TArray<FViewInfo>& Views)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RealtimeGIVoxelLighting");
	DECLARE_GPU_STAT(RealtimeGIVoxelLighting);
	RDG_GPU_STAT_SCOPE(GraphBuilder, RealtimeGIVoxelLighting);

	for (FViewInfo& View : Views)
	{
		View.RealtimeGIRadianceCache->Update(GraphBuilder, SceneRenderer, Scene, View);
	}
}

