#include "RealtimeGIScreenGather.h"

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "MeshPassProcessor.h"
#include "SceneRendering.h"
#include "Renderer/Private/ScenePrivate.h"
#include "SceneTextureParameters.h"
#include "PixelShaderUtils.h"


static TAutoConsoleVariable<int32> CVarVisualizeScreenGather(
	TEXT("r.RealtimeGI.VisualizeScreenGather"),
	0,
	TEXT("Visualize diffuse indirect screen gather result"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarScreenGatherDownsampleFactor(
	TEXT("r.RealtimeGI.ScreenGatherDownsampleFactor"),
	2,
	TEXT("RT downsample size"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarUseReservoirSpatialReuse(
	TEXT("r.RealtimeGI.UseReservoirSpatialReuse"),
	1,
	TEXT("If enable reservoir spatial reuse"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarSpatialReuseSampleNum(
	TEXT("r.RealtimeGI.SpatialReuseSampleNum"),
	8,
	TEXT("Number of neighbor pixels to check in spatial reuse pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarSpatialSecondaryReuseSampleNum(
	TEXT("r.RealtimeGI.SpatialSecondaryReuseSampleNum"),
	4,
	TEXT("Number of neighbor pixels to check in spatial reuse pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarSpatialSecondaryReuse(
	TEXT("r.RealtimeGI.SpatialSecondaryReuse"),
	0,
	TEXT("Use secondary spatial reuse pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarIndirectShadowEnable(
	TEXT("r.RealtimeGI.IndirectShadowEnable"),
	1,
	TEXT("Trace extra ray to validate spatial reservoir and generate indirect shadow"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarIndirectShadowSharpness(
	TEXT("r.RealtimeGI.IndirectShadowSharpness"),
	1.0,
	TEXT("Radius of neighbor pixels to check in spatial reuse pass (unit is meter)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarIndirectShadowIntensity(
	TEXT("r.RealtimeGI.IndirectShadowIntensity"),
	1.0,
	TEXT("Radius of neighbor pixels to check in spatial reuse pass (unit is meter)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarSpatialReuseSearchRange(
	TEXT("r.RealtimeGI.SpatialReuseSearchRange"),
	0.5,
	TEXT("Radius of neighbor pixels to check in spatial reuse pass (unit is meter)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarDiffuseTemporalFilterWeight(
	TEXT("r.RealtimeGI.DiffuseTemporalFilterWeight"),
	0.95,
	TEXT("History weight in temporal filter pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarDiffuseTemporalFilterHistoryColorBoundScale(
	TEXT("r.RealtimeGI.DiffuseTemporalFilterHistoryColorBoundScale"),
	0.1,
	TEXT("History color bounding box scale in temporal filter pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarDiffuseSpatialFilterIterationNum(
	TEXT("r.RealtimeGI.DiffuseSpatialFilterIterationNum"),
	5,
	TEXT("Rounds to do spatial filter"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarFilterGuidanceSSAORange(
	TEXT("r.RealtimeGI.FilterGuidanceSSAORange"),
	1.0,
	TEXT("SSAO range for filter guidance (unit is meter)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarFilterGuidanceSSAOIntensity(
	TEXT("r.RealtimeGI.FilterGuidanceSSAOIntensity"),
	1.0,
	TEXT("SSAO intensity for filter guidance"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarFilterGuidanceSSAOSharpness(
	TEXT("r.RealtimeGI.FilterGuidanceSSAOSharpness"),
	2.0,
	TEXT("SSAO sharpness for filter guidance"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarFilterGuidanceSSAOWeight(
	TEXT("r.RealtimeGI.FilterGuidanceSSAOWeight"),
	10.0,
	TEXT("SSAO weight for spatial filter"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarSpecularTemporalFilterWeight(
	TEXT("r.RealtimeGI.SpecularTemporalFilterWeight"),
	0.96,
	TEXT("History weight in temporal filter pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarSpecularTemporalFilterHistoryColorBoundScale(
	TEXT("r.RealtimeGI.SpecularTemporalFilterHistoryColorBoundScale"),
	0.8,
	TEXT("History color bounding box scale in temporal filter pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarSpecularResolveSearchRange(
	TEXT("r.RealtimeGI.SpecularResolveSearchRange"),
	4.0,
	TEXT("Neighbor search range in specular resolve pass"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarSpecularFilterSearchRange(
	TEXT("r.RealtimeGI.SpecularFilterSearchRange"),
	4.0,
	TEXT("Neighbor search range in specular filter pass"),
	ECVF_RenderThreadSafe
);


class FNormalDepthDownsamplePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNormalDepthDownsamplePS);
	SHADER_USE_PARAMETER_STRUCT(FNormalDepthDownsamplePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(FVector2D, InputExtentInverse)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNormalDepthDownsamplePS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "NormalDepthDownsamplePS", SF_Pixel);

class FInitialSampleScreenTraceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FInitialSampleScreenTraceCS);
	SHADER_USE_PARAMETER_STRUCT(FInitialSampleScreenTraceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER(FVector4, HZBUvFactorAndInvFactor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, HZB)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneColorHistory)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWInitialSampleRadiance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWInitialSampleHitInfo)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWVoxelTraceRayCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWVoxelTraceRayCompactBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 8;
	static const uint32 ThreadGroupSizeY = 8;
	static const uint32 ThreadGroupSizeZ = 1;

	class FUseDistanceField : SHADER_PERMUTATION_BOOL("USE_DISTANCE_FIELD");
	class FTraceSpecularRay : SHADER_PERMUTATION_BOOL("TRACE_SPECULAR_RAY");
	using FPermutationDomain = TShaderPermutationDomain<FUseDistanceField, FTraceSpecularRay>;

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
IMPLEMENT_GLOBAL_SHADER(FInitialSampleScreenTraceCS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "InitialSampleScreenTraceCS", SF_Compute);

class FBuildVoxelTraceIndirectArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildVoxelTraceIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildVoxelTraceIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumThreadsForVoxelTrace)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, VoxelTraceRayCounter)
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
IMPLEMENT_GLOBAL_SHADER(FBuildVoxelTraceIndirectArgsCS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "BuildVoxelTraceIndirectArgsCS", SF_Compute);

class FInitialSampleVoxelTraceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FInitialSampleVoxelTraceCS);
	SHADER_USE_PARAMETER_STRUCT(FInitialSampleVoxelTraceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWInitialSampleRadiance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWInitialSampleHitInfo)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, VoxelTraceRayCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, VoxelTraceRayCompactBuffer)
		SHADER_PARAMETER_RDG_BUFFER(StructuredBuffer, IndirectArgsBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 64;
	static const uint32 ThreadGroupSizeY = 1;
	static const uint32 ThreadGroupSizeZ = 1;

	class FUseDistanceField : SHADER_PERMUTATION_BOOL("USE_DISTANCE_FIELD");
	class FTraceSpecularRay : SHADER_PERMUTATION_BOOL("TRACE_SPECULAR_RAY");
	using FPermutationDomain = TShaderPermutationDomain<FUseDistanceField, FTraceSpecularRay>;

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
IMPLEMENT_GLOBAL_SHADER(FInitialSampleVoxelTraceCS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "InitialSampleVoxelTraceCS", SF_Compute);

class FReservoirTemporalReuseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReservoirTemporalReuseCS);
	SHADER_USE_PARAMETER_STRUCT(FReservoirTemporalReuseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InitialSampleRadiance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InitialSampleHitInfo)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataA)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataB)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataC)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataD)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataC)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataD)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 8;
	static const uint32 ThreadGroupSizeY = 8;
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
IMPLEMENT_GLOBAL_SHADER(FReservoirTemporalReuseCS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "ReservoirTemporalReuseCS", SF_Compute);

class FReservoirSpatialReuseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReservoirSpatialReuseCS);
	SHADER_USE_PARAMETER_STRUCT(FReservoirSpatialReuseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(int32, SpatialReuseSampleNum)
		SHADER_PARAMETER(float, SpatialReuseSearchRange)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataA)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataB)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataC)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataD)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataC)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWReservoirDataD)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = 8;
	static const uint32 ThreadGroupSizeY = 8;
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
IMPLEMENT_GLOBAL_SHADER(FReservoirSpatialReuseCS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "ReservoirSpatialReuseCS", SF_Compute);

class FReservoirEvaluateIrradiancePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReservoirEvaluateIrradiancePS);
	SHADER_USE_PARAMETER_STRUCT(FReservoirEvaluateIrradiancePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, MiniDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataA)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataB)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataC)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ReservoirDataD)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, IrradianceFallbackTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FIndirectShadowEnable : SHADER_PERMUTATION_BOOL("INDIRECT_SHADOW_ENABLE");
	using FPermutationDomain = TShaderPermutationDomain<FIndirectShadowEnable>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FReservoirEvaluateIrradiancePS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "ReservoirEvaluateIrradiancePS", SF_Pixel);

class FDiffuseTemporalFilterPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDiffuseTemporalFilterPS);
	SHADER_USE_PARAMETER_STRUCT(FDiffuseTemporalFilterPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER(float, TemporalFilterWeight)
		SHADER_PARAMETER(float, HistoryColorBoundScale)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, DiffuseIndirectTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, DiffuseIndirectHistory)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, IndirectShadowTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, IndirectShadowHistory)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FDiffuseTemporalFilterPS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "DiffuseTemporalFilterPS", SF_Pixel);

class FDiffuseSpatialFilterPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDiffuseSpatialFilterPS);
	SHADER_USE_PARAMETER_STRUCT(FDiffuseSpatialFilterPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(FIntPoint, RTSize)
		SHADER_PARAMETER(float, FilterRadius)
		SHADER_PARAMETER(float, SSAOGuidanceWeight)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, DiffuseIndirectTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, IndirectShadowTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FIndirectShadowEnable : SHADER_PERMUTATION_BOOL("INDIRECT_SHADOW_ENABLE");
	using FPermutationDomain = TShaderPermutationDomain<FIndirectShadowEnable>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FDiffuseSpatialFilterPS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "DiffuseSpatialFilterPS", SF_Pixel);

class FDiffuseCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDiffuseCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FDiffuseCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(float, AOIntensity)
		SHADER_PARAMETER(float, IndirectShadowSharpness)
		SHADER_PARAMETER(float, IndirectShadowIntensity)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, DiffuseIndirectTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, IndirectShadowTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FDiffuseCompositePS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "DiffuseCompositePS", SF_Pixel);

class FFilterGuidanceSSAOPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFilterGuidanceSSAOPS);
	SHADER_USE_PARAMETER_STRUCT(FFilterGuidanceSSAOPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(float, AOWorldRange)
		SHADER_PARAMETER(float, AOSharpness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FFilterGuidanceSSAOPS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "FilterGuidanceSSAOPS", SF_Pixel);

class FSpecularResolvePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSpecularResolvePS);
	SHADER_USE_PARAMETER_STRUCT(FSpecularResolvePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(FIntPoint, ScreenGatherRTSize)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(float, SpecularResolveSearchRange)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InitialSampleRadiance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InitialSampleHitInfo)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_TEXTURE(Texture2D, PreIntegratedGF)
		SHADER_PARAMETER_SAMPLER(SamplerState, PreIntegratedGFSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FTraceSpecularRay : SHADER_PERMUTATION_BOOL("TRACE_SPECULAR_RAY");
	using FPermutationDomain = TShaderPermutationDomain<FTraceSpecularRay>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FSpecularResolvePS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "SpecularResolvePS", SF_Pixel);

class FSpecularTemporalFilterPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSpecularTemporalFilterPS);
	SHADER_USE_PARAMETER_STRUCT(FSpecularTemporalFilterPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(FIntPoint, SceneTextureRTSize)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(float, TemporalFilterWeight)
		SHADER_PARAMETER(float, HistoryColorBoundScale)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SpecularIndirectTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SpecularIndirectHistory)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthHistory)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_TEXTURE(Texture2D, PreIntegratedGF)
		SHADER_PARAMETER_SAMPLER(SamplerState, PreIntegratedGFSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FTraceSpecularRay : SHADER_PERMUTATION_BOOL("TRACE_SPECULAR_RAY");
	using FPermutationDomain = TShaderPermutationDomain<FTraceSpecularRay>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FSpecularTemporalFilterPS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "SpecularTemporalFilterPS", SF_Pixel);

class FSpecularSpatialFilterPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSpecularSpatialFilterPS);
	SHADER_USE_PARAMETER_STRUCT(FSpecularSpatialFilterPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(uint32, FrameNumber)
		SHADER_PARAMETER(FIntPoint, RTSize)
		SHADER_PARAMETER(float, FilterRadius)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SpecularIndirectTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FSpecularSpatialFilterPS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "SpecularSpatialFilterPS", SF_Pixel);


class FSpecularCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSpecularCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FSpecularCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER(FIntPoint, SceneTextureRTSize)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, NormalDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SpecularIndirectTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, PreIntegratedGF)
		SHADER_PARAMETER_SAMPLER(SamplerState, PreIntegratedGFSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FSpecularCompositePS, "/Engine/Private/RealtimeGI/ScreenGather.usf", "SpecularCompositePS", SF_Pixel);


void FRealtimeGIScreenGatherPipeline::Setup(TRDGUniformBufferRef<FSceneTextureUniformParameters> InSceneTexturesUniformBuffer)
{
	SceneTexturesUniformBuffer = InSceneTexturesUniformBuffer;
}

void FRealtimeGIScreenGatherPipeline::Update(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	PrepareRenderResources(GraphBuilder, Scene, View);

	NormalDepthDownsample(GraphBuilder, Scene, View);

	{
		RDG_EVENT_SCOPE(GraphBuilder, "DiffuseIndirect");

		InitialSampleScreenTrace(GraphBuilder, Scene, View, TM_Diffuse);

		InitialSampleVoxelTrace(GraphBuilder, Scene, View, TM_Diffuse);

		ReservoirTemporalReuse(GraphBuilder, Scene, View);

		ReservoirEvaluateIrradiance(GraphBuilder, Scene, View, RS_Temporal);
		DiffuseResolveOutputTexture = TemporalReservoirIrradiance;

		if (CVarUseReservoirSpatialReuse.GetValueOnRenderThread() > 0)
		{
			// reuse from temporal reservoir to spatial reservoir
			ReservoirSpatialReuse(GraphBuilder, Scene, View, RS_Temporal);

			// secondary reuse from ping pong reservoir buffer
			if (CVarSpatialSecondaryReuse.GetValueOnRenderThread() > 0)
			{
				ReservoirSpatialReuse(GraphBuilder, Scene, View, RS_Spatial);
			}

			// resolve
			ReservoirEvaluateIrradiance(GraphBuilder, Scene, View, RS_Spatial);
			DiffuseResolveOutputTexture = SpatialReservoirIrradiance;
		}

		RenderFilterGuidanceSSAO(GraphBuilder, Scene, View);

		DiffuseTemporalFilter(GraphBuilder, Scene, View);

		DiffuseSpatialFilter(GraphBuilder, Scene, View);
	}

	{
		RDG_EVENT_SCOPE(GraphBuilder, "SpecularIndirect");

		InitialSampleScreenTrace(GraphBuilder, Scene, View, TM_Specular);

		InitialSampleVoxelTrace(GraphBuilder, Scene, View, TM_Specular);

		SpecularResolve(GraphBuilder, Scene, View);
		
		SpecularTemporalFilter(GraphBuilder, Scene, View);
		
		SpecularSpatialFilter(GraphBuilder, Scene, View);
	}

	// note: manually copy, do not use RDG extraction
	{
		RDG_EVENT_SCOPE(GraphBuilder, "NormalDepthTextureCopyHistory");
		SimpleBlit(GraphBuilder, Scene, View, NormalDepthTexture, NormalDepthHistory.RDGTexture);
	}
}

void FRealtimeGIScreenGatherPipeline::PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	const int32 DownsampleFactor = FMath::Clamp(CVarScreenGatherDownsampleFactor.GetValueOnRenderThread(), 1, 4);
	SceneTextureRTSize = (*SceneTexturesUniformBuffer)->SceneDepthTexture->Desc.Extent;
	ScreenGatherRTSize = SceneTextureRTSize / DownsampleFactor;

	const FRDGTextureDesc DescR8(FRDGTextureDesc::Create2D(ScreenGatherRTSize, PF_R8, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ShaderResource));
	const FRDGTextureDesc DescR16F(FRDGTextureDesc::Create2D(ScreenGatherRTSize, PF_R16F, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ShaderResource));
	const FRDGTextureDesc DescRGBA16F(FRDGTextureDesc::Create2D(ScreenGatherRTSize, PF_FloatRGBA, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ShaderResource));
	const FRDGTextureDesc DescRGBA32F(FRDGTextureDesc::Create2D(ScreenGatherRTSize, PF_A32B32G32R32F, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ShaderResource));
	const FRDGTextureDesc DescR32I(FRDGTextureDesc::Create2D(ScreenGatherRTSize, PF_R32_UINT, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ShaderResource));

	MiniDepthTexture = GraphBuilder.CreateTexture(DescR16F, TEXT("MiniDepthTexture"));
	NormalDepthTexture = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("NormalDepthTexture"));
	Create2DTexFn(GraphBuilder, NormalDepthHistory, ScreenGatherRTSize, PF_FloatRGBA, TEXT("NormalDepthHistory"));
	Create2DTexFn(GraphBuilder, SceneColorHistory, ScreenGatherRTSize, PF_FloatRGBA, TEXT("SceneColorHistory"), true);

	InitialSampleRadiance = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("InitialSampleRadiance"));
	InitialSampleHitInfo = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("InitialSampleHitInfo"));

	// cur frame reservoir
	Create2DTexFn(GraphBuilder, TemporalReservoirDataA[0], ScreenGatherRTSize, PF_FloatRGBA, TEXT("TemporalReservoirDataA_PingPong0"));
	Create2DTexFn(GraphBuilder, TemporalReservoirDataB[0], ScreenGatherRTSize, PF_FloatRGBA, TEXT("TemporalReservoirDataB_PingPong0"));
	Create2DTexFn(GraphBuilder, TemporalReservoirDataC[0], ScreenGatherRTSize, PF_A32B32G32R32F, TEXT("TemporalReservoirDataC_PingPong0"));
	Create2DTexFn(GraphBuilder, TemporalReservoirDataD[0], ScreenGatherRTSize, PF_A32B32G32R32F, TEXT("TemporalReservoirDataD_PingPong0"));

	// prev frame reservoir
	Create2DTexFn(GraphBuilder, TemporalReservoirDataA[1], ScreenGatherRTSize, PF_FloatRGBA, TEXT("TemporalReservoirDataA_PingPong1"));
	Create2DTexFn(GraphBuilder, TemporalReservoirDataB[1], ScreenGatherRTSize, PF_FloatRGBA, TEXT("TemporalReservoirDataB_PingPong1"));
	Create2DTexFn(GraphBuilder, TemporalReservoirDataC[1], ScreenGatherRTSize, PF_A32B32G32R32F, TEXT("TemporalReservoirDataC_PingPong1"));
	Create2DTexFn(GraphBuilder, TemporalReservoirDataD[1], ScreenGatherRTSize, PF_A32B32G32R32F, TEXT("TemporalReservoirDataD_PingPong1"));

	// ping pong reservoir
	SpatialReservoirDataA[0] = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("SpatialReservoirDataA_PingPong0"));
	SpatialReservoirDataB[0] = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("SpatialReservoirDataB_PingPong0"));
	SpatialReservoirDataC[0] = GraphBuilder.CreateTexture(DescRGBA32F, TEXT("SpatialReservoirDataC_PingPong0"));
	SpatialReservoirDataD[0] = GraphBuilder.CreateTexture(DescRGBA32F, TEXT("SpatialReservoirDataD_PingPong0"));

	SpatialReservoirDataA[1] = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("SpatialReservoirDataA_PingPong1"));
	SpatialReservoirDataB[1] = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("SpatialReservoirDataB_PingPong1"));
	SpatialReservoirDataC[1] = GraphBuilder.CreateTexture(DescRGBA32F, TEXT("SpatialReservoirDataC_PingPong1"));
	SpatialReservoirDataD[1] = GraphBuilder.CreateTexture(DescRGBA32F, TEXT("SpatialReservoirDataD_PingPong1"));

	TemporalReservoirIrradiance = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("TemporalReservoirIrradiance"));
	SpatialReservoirIrradiance = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("SpatialReservoirIrradiance"));
	// DiffuseResolveOutputTexture = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("DiffuseResolveOutputTexture"));
	DiffuseTemporalFilterOutput = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("DiffuseTemporalFilterOutput"));
	DiffuseSpatialFilterOutput = GraphBuilder.CreateTexture(DescRGBA16F, TEXT("DiffuseSpatialFilterOutput"));
	Create2DTexFn(GraphBuilder, DiffuseIndirectHistory, ScreenGatherRTSize, PF_FloatRGBA, TEXT("DiffuseIndirectHistory"));
	
	IndirectShadowTexture = GraphBuilder.CreateTexture(DescR8, TEXT("IndirectShadowTexture"));
	IndirectShadowTemporalFilterOutput = GraphBuilder.CreateTexture(DescR8, TEXT("IndirectShadowTemporalFilterOutput"));
	IndirectShadowSpatialFilterOutput = GraphBuilder.CreateTexture(DescR8, TEXT("IndirectShadowSpatialFilterOutput"));
	Create2DTexFn(GraphBuilder, IndirectShadowHistory, ScreenGatherRTSize, PF_R8, TEXT("IndirectShadowHistory"));

	const FRDGTextureDesc DescFullResRGBA16F(FRDGTextureDesc::Create2D(SceneTextureRTSize, PF_FloatRGBA, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ShaderResource));
	SpecularResolveOutputTexture = GraphBuilder.CreateTexture(DescFullResRGBA16F, TEXT("SpecularResolveOutputTexture"));
	SpecularTemporalFilterOutput = GraphBuilder.CreateTexture(DescFullResRGBA16F, TEXT("SpecularTemporalFilterOutput"));
	SpecularSpatialFilterOutput = GraphBuilder.CreateTexture(DescFullResRGBA16F, TEXT("SpecularSpatialFilterOutput"));
	Create2DTexFn(GraphBuilder, SpecularIndirectHistory, SceneTextureRTSize, PF_FloatRGBA, TEXT("SpecularIndirectHistory"));


	VoxelTraceRayCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
		TEXT("VoxelTraceRayCounter")
	);

	VoxelTraceRayCompactBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FIntPoint), ScreenGatherRTSize.X * ScreenGatherRTSize.Y),
		TEXT("VoxelTraceRayCompactBuffer")
	);

	VoxelTraceIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("VoxelTraceIndirectArgs")
	);
}

void FRealtimeGIScreenGatherPipeline::NormalDepthDownsample(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	FScreenPassTextureViewport InputViewport(SceneTextureRTSize);
	FScreenPassTextureViewport OutputViewport(ScreenGatherRTSize);

	TShaderMapRef<FNormalDepthDownsamplePS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FNormalDepthDownsamplePS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->InputExtentInverse = FVector2D(1.0f) / FVector2D(SceneTextureRTSize);
	PassParameters->RenderTargets[0] = FRenderTargetBinding(NormalDepthTexture, ERenderTargetLoadAction::ENoAction);
	PassParameters->RenderTargets[1] = FRenderTargetBinding(MiniDepthTexture, ERenderTargetLoadAction::ENoAction);

	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("NormalDepthDownsample"),
		View, OutputViewport, InputViewport, PixelShader, PassParameters
	);
}

void FRealtimeGIScreenGatherPipeline::InitialSampleScreenTrace(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, ETraceMode TraceMode)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	FRealtimeGIRadianceCache* RadianceCache = View.RealtimeGIRadianceCache;

	ClearCounterBuffer(GraphBuilder, Scene, VoxelTraceRayCounter, 1);

	FInitialSampleScreenTraceCS::FPermutationDomain Permutation;
	Permutation.Set<FInitialSampleScreenTraceCS::FUseDistanceField>(RealtimeGIUseDistanceField());
	Permutation.Set<FInitialSampleScreenTraceCS::FTraceSpecularRay>(TraceMode == TM_Specular);

	TShaderMapRef<FInitialSampleScreenTraceCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
	auto* PassParameters = GraphBuilder.AllocParameters<FInitialSampleScreenTraceCS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->NormalDepthHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthHistory.RDGTexture));
	PassParameters->SceneColorHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneColorHistory.RDGTexture));
	PassParameters->RWInitialSampleRadiance = GraphBuilder.CreateUAV(InitialSampleRadiance);
	PassParameters->RWInitialSampleHitInfo = GraphBuilder.CreateUAV(InitialSampleHitInfo);
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();

	const FVector2D HZBUvFactor(float(View.ViewRect.Width()) / float(2 * View.HZBMipmap0Size.X), float(View.ViewRect.Height()) / float(2 * View.HZBMipmap0Size.Y));
	FRDGTextureRef HZB = GraphBuilder.RegisterExternalTexture(View.HZB);

	PassParameters->HZBUvFactorAndInvFactor = FVector4(HZBUvFactor.X, HZBUvFactor.Y, 1.0f / HZBUvFactor.X, 1.0f / HZBUvFactor.Y);
	PassParameters->HZB = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HZB));

	PassParameters->RWVoxelTraceRayCounter = GraphBuilder.CreateUAV(VoxelTraceRayCounter);
	PassParameters->RWVoxelTraceRayCompactBuffer = GraphBuilder.CreateUAV(VoxelTraceRayCompactBuffer);

	FIntVector NumGroups = FIntVector(
		FMath::CeilToInt(float(ScreenGatherRTSize.X) / float(FInitialSampleScreenTraceCS::ThreadGroupSizeX)),
		FMath::CeilToInt(float(ScreenGatherRTSize.Y) / float(FInitialSampleScreenTraceCS::ThreadGroupSizeY)),
		1
	);

	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("InitialSampleScreenTrace"),
		ComputeShader, PassParameters, NumGroups
	);
}

void FRealtimeGIScreenGatherPipeline::InitialSampleVoxelTrace(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, ETraceMode TraceMode)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
	FRealtimeGIRadianceCache* RadianceCache = View.RealtimeGIRadianceCache;

	// 1. build indirect args
	{
		TShaderMapRef<FBuildVoxelTraceIndirectArgsCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FBuildVoxelTraceIndirectArgsCS::FParameters>();
		PassParameters->NumThreadsForVoxelTrace = FInitialSampleVoxelTraceCS::ThreadGroupSizeX;
		PassParameters->VoxelTraceRayCounter = GraphBuilder.CreateSRV(VoxelTraceRayCounter);
		PassParameters->RWIndirectArgs = GraphBuilder.CreateUAV(VoxelTraceIndirectArgs);

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("BuildVoxelTraceIndirectArgs"),
			ComputeShader, PassParameters, FIntVector(1, 1, 1)
		);
	}

	// 2. voxel trace
	{
		FInitialSampleVoxelTraceCS::FPermutationDomain Permutation;
		Permutation.Set<FInitialSampleVoxelTraceCS::FUseDistanceField>(RealtimeGIUseDistanceField());
		Permutation.Set<FInitialSampleVoxelTraceCS::FTraceSpecularRay>(TraceMode == TM_Specular);

		TShaderMapRef<FInitialSampleVoxelTraceCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
		auto* PassParameters = GraphBuilder.AllocParameters<FInitialSampleVoxelTraceCS::FParameters>();
		PassParameters->VoxelRaytracingParameters = VoxelClipmap->SetupVoxelRaytracingParameters(GraphBuilder, Scene, View);
		PassParameters->ProbeVolumeParameters = RadianceCache->SetupProbeVolumeParameters(GraphBuilder, Scene, View);
		PassParameters->SceneTextures = SceneTexturesUniformBuffer;
		PassParameters->FrameNumber = FrameNumberRenderThread;
		PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
		PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
		PassParameters->RWInitialSampleRadiance = GraphBuilder.CreateUAV(InitialSampleRadiance);
		PassParameters->RWInitialSampleHitInfo = GraphBuilder.CreateUAV(InitialSampleHitInfo);
		PassParameters->VoxelTraceRayCounter = GraphBuilder.CreateSRV(VoxelTraceRayCounter);
		PassParameters->VoxelTraceRayCompactBuffer = GraphBuilder.CreateSRV(VoxelTraceRayCompactBuffer);
		PassParameters->IndirectArgsBuffer = VoxelTraceIndirectArgs;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("InitialSampleVoxelTrace"),
			ComputeShader, PassParameters, VoxelTraceIndirectArgs, 0
		);
	}
}

void FRealtimeGIScreenGatherPipeline::ReservoirTemporalReuse(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	uint32 CurFrame = (FrameNumberRenderThread + 0) % 2;
	uint32 PrevFrame = (FrameNumberRenderThread + 1) % 2;

	TShaderMapRef<FReservoirTemporalReuseCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FReservoirTemporalReuseCS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->InitialSampleRadiance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InitialSampleRadiance));
	PassParameters->InitialSampleHitInfo = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InitialSampleHitInfo));
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->NormalDepthHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthHistory.RDGTexture));
	// reservoir read
	PassParameters->ReservoirDataA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataA[PrevFrame].RDGTexture));
	PassParameters->ReservoirDataB = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataB[PrevFrame].RDGTexture));
	PassParameters->ReservoirDataC = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataC[PrevFrame].RDGTexture));
	PassParameters->ReservoirDataD = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataD[PrevFrame].RDGTexture));
	// reservoir write
	PassParameters->RWReservoirDataA = GraphBuilder.CreateUAV(TemporalReservoirDataA[CurFrame].RDGTexture);
	PassParameters->RWReservoirDataB = GraphBuilder.CreateUAV(TemporalReservoirDataB[CurFrame].RDGTexture);
	PassParameters->RWReservoirDataC = GraphBuilder.CreateUAV(TemporalReservoirDataC[CurFrame].RDGTexture);
	PassParameters->RWReservoirDataD = GraphBuilder.CreateUAV(TemporalReservoirDataD[CurFrame].RDGTexture);

	FIntVector NumGroups = FIntVector(
		FMath::CeilToInt(float(ScreenGatherRTSize.X) / float(FReservoirTemporalReuseCS::ThreadGroupSizeX)),
		FMath::CeilToInt(float(ScreenGatherRTSize.Y) / float(FReservoirTemporalReuseCS::ThreadGroupSizeY)),
		1
	);

	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("ReservoirTemporalReuse"),
		ComputeShader, PassParameters, NumGroups
	);
}

void FRealtimeGIScreenGatherPipeline::ReservoirSpatialReuse(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, EReservoirSource ReservoirSource)
{
	uint32 CurFrame = (FrameNumberRenderThread + 0) % 2;
	uint32 PrevFrame = (FrameNumberRenderThread + 1) % 2;

	TShaderMapRef<FReservoirSpatialReuseCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FReservoirSpatialReuseCS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->SpatialReuseSearchRange = CVarSpatialReuseSearchRange.GetValueOnRenderThread() * 100.0f;
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));

	if (ReservoirSource == RS_Temporal)
	{
		PassParameters->SpatialReuseSampleNum = CVarSpatialReuseSampleNum.GetValueOnRenderThread();

		PassParameters->ReservoirDataA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataA[CurFrame].RDGTexture));
		PassParameters->ReservoirDataB = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataB[CurFrame].RDGTexture));
		PassParameters->ReservoirDataC = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataC[CurFrame].RDGTexture));
		PassParameters->ReservoirDataD = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataD[CurFrame].RDGTexture));

		PassParameters->RWReservoirDataA = GraphBuilder.CreateUAV(SpatialReservoirDataA[0]);
		PassParameters->RWReservoirDataB = GraphBuilder.CreateUAV(SpatialReservoirDataB[0]);
		PassParameters->RWReservoirDataC = GraphBuilder.CreateUAV(SpatialReservoirDataC[0]);
		PassParameters->RWReservoirDataD = GraphBuilder.CreateUAV(SpatialReservoirDataD[0]);
	}
	else
	{
		PassParameters->SpatialReuseSampleNum = CVarSpatialSecondaryReuseSampleNum.GetValueOnRenderThread();

		PassParameters->ReservoirDataA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataA[0]));
		PassParameters->ReservoirDataB = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataB[0]));
		PassParameters->ReservoirDataC = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataC[0]));
		PassParameters->ReservoirDataD = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataD[0]));

		PassParameters->RWReservoirDataA = GraphBuilder.CreateUAV(SpatialReservoirDataA[1]);
		PassParameters->RWReservoirDataB = GraphBuilder.CreateUAV(SpatialReservoirDataB[1]);
		PassParameters->RWReservoirDataC = GraphBuilder.CreateUAV(SpatialReservoirDataC[1]);
		PassParameters->RWReservoirDataD = GraphBuilder.CreateUAV(SpatialReservoirDataD[1]);
	}

	FIntVector NumGroups = FIntVector(
		FMath::CeilToInt(float(ScreenGatherRTSize.X) / float(FReservoirSpatialReuseCS::ThreadGroupSizeX)),
		FMath::CeilToInt(float(ScreenGatherRTSize.Y) / float(FReservoirSpatialReuseCS::ThreadGroupSizeY)),
		1
	);

	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("ReservoirSpatialReuse"),
		ComputeShader, PassParameters, NumGroups
	);
}

void FRealtimeGIScreenGatherPipeline::ReservoirEvaluateIrradiance(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, EReservoirSource ReservoirSource)
{
	FScreenPassTextureViewport InputViewport(ScreenGatherRTSize);
	FScreenPassTextureViewport OutputViewport(ScreenGatherRTSize);

	bool IndirectShadowEnable = ReservoirSource == EReservoirSource::RS_Spatial
							 && CVarIndirectShadowEnable.GetValueOnRenderThread() > 0;

	FReservoirEvaluateIrradiancePS::FPermutationDomain Permutation;
	Permutation.Set<FReservoirEvaluateIrradiancePS::FIndirectShadowEnable>(IndirectShadowEnable);

	TShaderMapRef<FReservoirEvaluateIrradiancePS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
	auto* PassParameters = GraphBuilder.AllocParameters<FReservoirEvaluateIrradiancePS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->MiniDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(MiniDepthTexture));

	if (ReservoirSource == EReservoirSource::RS_Spatial)
	{
		PassParameters->IrradianceFallbackTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirIrradiance));

		uint32 SpatialReservoirSource = CVarSpatialSecondaryReuse.GetValueOnRenderThread() > 0 ? 1 : 0;
		PassParameters->ReservoirDataA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataA[SpatialReservoirSource]));
		PassParameters->ReservoirDataB = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataB[SpatialReservoirSource]));
		PassParameters->ReservoirDataC = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataC[SpatialReservoirSource]));
		PassParameters->ReservoirDataD = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialReservoirDataD[SpatialReservoirSource]));
	
		PassParameters->RenderTargets[0] = FRenderTargetBinding(SpatialReservoirIrradiance, ERenderTargetLoadAction::ENoAction);
	}
	else
	{
		uint32 TemporalReservoirCurFrame = (FrameNumberRenderThread + 0) % 2;
		PassParameters->ReservoirDataA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataA[TemporalReservoirCurFrame].RDGTexture));
		PassParameters->ReservoirDataB = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataB[TemporalReservoirCurFrame].RDGTexture));
		PassParameters->ReservoirDataC = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataC[TemporalReservoirCurFrame].RDGTexture));
		PassParameters->ReservoirDataD = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(TemporalReservoirDataD[TemporalReservoirCurFrame].RDGTexture));
		
		PassParameters->RenderTargets[0] = FRenderTargetBinding(TemporalReservoirIrradiance, ERenderTargetLoadAction::ENoAction);
	}

	PassParameters->RenderTargets[1] = FRenderTargetBinding(IndirectShadowTexture, ERenderTargetLoadAction::ENoAction);

	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("ReservoirEvaluateIrradiance"),
		View, OutputViewport, InputViewport, PixelShader, PassParameters
	);
}

void FRealtimeGIScreenGatherPipeline::RenderFilterGuidanceSSAO(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	FScreenPassTextureViewport InputViewport(ScreenGatherRTSize);
	FScreenPassTextureViewport OutputViewport(ScreenGatherRTSize);

	TShaderMapRef<FFilterGuidanceSSAOPS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FFilterGuidanceSSAOPS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->AOWorldRange = CVarFilterGuidanceSSAORange.GetValueOnRenderThread() * 100.0f;
	PassParameters->AOSharpness = CVarFilterGuidanceSSAOSharpness.GetValueOnRenderThread();
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->RenderTargets[0] = FRenderTargetBinding(DiffuseResolveOutputTexture, ERenderTargetLoadAction::ENoAction);

	TShaderMapRef<FScreenPassVS> VertexShader(View.ShaderMap);
	FRHIBlendState* BlendState = TStaticBlendState<CW_ALPHA, BO_Add, BF_One, BF_One>::GetRHI();
	FRHIDepthStencilState* DepthStencilState = FScreenPassPipelineState::FDefaultDepthStencilState::GetRHI();
	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("RenderFilterGuidanceSSAO"),
		View, OutputViewport, InputViewport, VertexShader, PixelShader,
		BlendState, DepthStencilState, PassParameters
	);
}

void FRealtimeGIScreenGatherPipeline::DiffuseTemporalFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	const FRDGTextureDesc& InputDesc = DiffuseResolveOutputTexture->Desc;
	const FRDGTextureDesc& OutputDesc = DiffuseTemporalFilterOutput->Desc;
	const FScreenPassTextureViewport InputViewport(InputDesc.Extent, FIntRect(FIntPoint::ZeroValue, InputDesc.Extent));
	const FScreenPassTextureViewport OutputViewport(OutputDesc.Extent, FIntRect(FIntPoint::ZeroValue, OutputDesc.Extent));

	TShaderMapRef<FDiffuseTemporalFilterPS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FDiffuseTemporalFilterPS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->TemporalFilterWeight = CVarDiffuseTemporalFilterWeight.GetValueOnRenderThread();
	PassParameters->HistoryColorBoundScale = CVarDiffuseTemporalFilterHistoryColorBoundScale.GetValueOnRenderThread();
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->NormalDepthHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthHistory.RDGTexture));
	PassParameters->DiffuseIndirectTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DiffuseResolveOutputTexture));
	PassParameters->DiffuseIndirectHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DiffuseIndirectHistory.RDGTexture));
	PassParameters->IndirectShadowTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(IndirectShadowTexture));
	PassParameters->IndirectShadowHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(IndirectShadowHistory.RDGTexture));
	PassParameters->RenderTargets[0] = FRenderTargetBinding(DiffuseTemporalFilterOutput, ERenderTargetLoadAction::ENoAction);
	PassParameters->RenderTargets[1] = FRenderTargetBinding(IndirectShadowTemporalFilterOutput, ERenderTargetLoadAction::ENoAction);

	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("DiffuseTemporalFilter"), 
		View, OutputViewport, InputViewport, PixelShader, PassParameters
	);

	{
		RDG_EVENT_SCOPE(GraphBuilder, "DiffuseIndirectCopyHistory");
		SimpleBlit(GraphBuilder, Scene, View, DiffuseTemporalFilterOutput, DiffuseIndirectHistory.RDGTexture);
	}

	{
		RDG_EVENT_SCOPE(GraphBuilder, "IndirectShadowCopyHistory");
		SimpleBlit(GraphBuilder, Scene, View, IndirectShadowTemporalFilterOutput, IndirectShadowHistory.RDGTexture);
	}
}

void FRealtimeGIScreenGatherPipeline::DiffuseSpatialFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	const FRDGTextureDesc& InputDesc = DiffuseTemporalFilterOutput->Desc;
	const FRDGTextureDesc& OutputDesc = DiffuseSpatialFilterOutput->Desc;
	const FScreenPassTextureViewport InputViewport(InputDesc.Extent, FIntRect(FIntPoint::ZeroValue, InputDesc.Extent));
	const FScreenPassTextureViewport OutputViewport(OutputDesc.Extent, FIntRect(FIntPoint::ZeroValue, OutputDesc.Extent));

	FRDGTextureRef SpatialFilterOutputTemp = GraphBuilder.CreateTexture(DiffuseSpatialFilterOutput->Desc, TEXT("SpatialFilterOutputTemp"));
	FRDGTextureRef SpatialFilterShadowOutputTemp = GraphBuilder.CreateTexture(IndirectShadowSpatialFilterOutput->Desc, TEXT("SpatialFilterShadowOutputTemp"));

	auto SpatialFilterFn = [this, &GraphBuilder, Scene, &View, &InputViewport, &OutputViewport](
		FRDGTextureRef& InputTex, FRDGTextureRef& OutputTex, float FilterRadius,
		FRDGTextureRef& InputShadowTex, FRDGTextureRef& OutputShadowTex, bool FilterIndirectShadow
		)
	{
		FDiffuseSpatialFilterPS::FPermutationDomain Permutation;
		Permutation.Set<FDiffuseSpatialFilterPS::FIndirectShadowEnable>(FilterIndirectShadow);

		TShaderMapRef<FDiffuseSpatialFilterPS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
		auto* PassParameters = GraphBuilder.AllocParameters<FDiffuseSpatialFilterPS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTextures = SceneTexturesUniformBuffer;
		PassParameters->FrameNumber = FrameNumberRenderThread;
		PassParameters->RTSize = InputViewport.Extent;
		PassParameters->FilterRadius = FilterRadius;
		PassParameters->SSAOGuidanceWeight = CVarFilterGuidanceSSAOWeight.GetValueOnRenderThread();
		PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
		PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
		PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
		PassParameters->NormalDepthHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthHistory.RDGTexture));
		PassParameters->DiffuseIndirectTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InputTex));
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTex, ERenderTargetLoadAction::ENoAction);
		
		if (FilterIndirectShadow)
		{
			PassParameters->IndirectShadowTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InputShadowTex));
			PassParameters->RenderTargets[1] = FRenderTargetBinding(OutputShadowTex, ERenderTargetLoadAction::ENoAction);
		}

		AddDrawScreenPass(
			GraphBuilder, RDG_EVENT_NAME("DiffuseSpatialFilter"),
			View, OutputViewport, InputViewport, PixelShader, PassParameters
		);
	};

	int32 IterNum = FMath::Clamp(CVarDiffuseSpatialFilterIterationNum.GetValueOnRenderThread(), 1, 10);
	SpatialFilterFn(
		DiffuseTemporalFilterOutput, DiffuseSpatialFilterOutput, 1,
		IndirectShadowTemporalFilterOutput, IndirectShadowSpatialFilterOutput, true
	);

	for (int32 i = 1; i < IterNum; i++)
	{
		bool FilterIndirectShadow = i < 2;	// @TODO: as parameter

		SpatialFilterFn(
			DiffuseSpatialFilterOutput, SpatialFilterOutputTemp, (1 << i),
			IndirectShadowSpatialFilterOutput, SpatialFilterShadowOutputTemp, FilterIndirectShadow
		);

		Swap(DiffuseSpatialFilterOutput, SpatialFilterOutputTemp);
		if (FilterIndirectShadow)
		{
			Swap(IndirectShadowSpatialFilterOutput, SpatialFilterShadowOutputTemp);
		}
	}
}

void FRealtimeGIScreenGatherPipeline::SpecularResolve(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	FScreenPassTextureViewport InputViewport(ScreenGatherRTSize);
	FScreenPassTextureViewport OutputViewport(SceneTextureRTSize);

	FSpecularResolvePS::FPermutationDomain Permutation;
	Permutation.Set<FSpecularResolvePS::FTraceSpecularRay>(true);

	TShaderMapRef<FSpecularResolvePS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
	auto* PassParameters = GraphBuilder.AllocParameters<FSpecularResolvePS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->SpecularResolveSearchRange = CVarSpecularResolveSearchRange.GetValueOnRenderThread();
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->InitialSampleRadiance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InitialSampleRadiance));
	PassParameters->InitialSampleHitInfo = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InitialSampleHitInfo));
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SpecularResolveOutputTexture, ERenderTargetLoadAction::ENoAction);

	PassParameters->PreIntegratedGF = GSystemTextures.PreintegratedGF->GetRenderTargetItem().ShaderResourceTexture;
	PassParameters->PreIntegratedGFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("SpecularResolve"),
		View, OutputViewport, InputViewport, PixelShader, PassParameters
	);
}

void FRealtimeGIScreenGatherPipeline::SpecularTemporalFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	FScreenPassTextureViewport InputViewport(SceneTextureRTSize);
	FScreenPassTextureViewport OutputViewport(SceneTextureRTSize);

	FSpecularTemporalFilterPS::FPermutationDomain Permutation;
	Permutation.Set<FSpecularTemporalFilterPS::FTraceSpecularRay>(true);

	TShaderMapRef<FSpecularTemporalFilterPS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
	auto* PassParameters = GraphBuilder.AllocParameters<FSpecularTemporalFilterPS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->TemporalFilterWeight = CVarSpecularTemporalFilterWeight.GetValueOnRenderThread();
	PassParameters->HistoryColorBoundScale = CVarSpecularTemporalFilterHistoryColorBoundScale.GetValueOnRenderThread();
	PassParameters->SceneTextureRTSize = SceneTextureRTSize;
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->SpecularIndirectTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpecularResolveOutputTexture));
	PassParameters->SpecularIndirectHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpecularIndirectHistory.RDGTexture));
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->NormalDepthHistory = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthHistory.RDGTexture));
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SpecularTemporalFilterOutput, ERenderTargetLoadAction::ENoAction);

	PassParameters->PreIntegratedGF = GSystemTextures.PreintegratedGF->GetRenderTargetItem().ShaderResourceTexture;
	PassParameters->PreIntegratedGFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("SpecularTemporalFilter"),
		View, OutputViewport, InputViewport, PixelShader, PassParameters
	);

	{
		RDG_EVENT_SCOPE(GraphBuilder, "SpecularIndirectCopyHistory");
		SimpleBlit(GraphBuilder, Scene, View, SpecularTemporalFilterOutput, SpecularIndirectHistory.RDGTexture);
	}
}

void FRealtimeGIScreenGatherPipeline::SpecularSpatialFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	const FRDGTextureDesc& InputDesc = SpecularTemporalFilterOutput->Desc;
	const FRDGTextureDesc& OutputDesc = SpecularSpatialFilterOutput->Desc;
	const FScreenPassTextureViewport InputViewport(InputDesc.Extent, FIntRect(FIntPoint::ZeroValue, InputDesc.Extent));
	const FScreenPassTextureViewport OutputViewport(OutputDesc.Extent, FIntRect(FIntPoint::ZeroValue, OutputDesc.Extent));

	TShaderMapRef<FSpecularSpatialFilterPS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FSpecularSpatialFilterPS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->RTSize = InputViewport.Extent;
	PassParameters->FilterRadius = CVarSpecularFilterSearchRange.GetValueOnRenderThread();
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->SpecularIndirectTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpecularTemporalFilterOutput));
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SpecularSpatialFilterOutput, ERenderTargetLoadAction::ENoAction);

	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("SpecularSpatialFilter"),
		View, OutputViewport, InputViewport, PixelShader, PassParameters
	);
}

void FRealtimeGIScreenGatherPipeline::DiffuseComposite(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture)
{
	FScreenPassTextureViewport InputViewport(ScreenGatherRTSize);
	FScreenPassTextureViewport OutputViewport(SceneTextureRTSize);

	TShaderMapRef<FDiffuseCompositePS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FDiffuseCompositePS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->ScreenGatherRTSize = ScreenGatherRTSize;
	PassParameters->FrameNumber = FrameNumberRenderThread;
	PassParameters->AOIntensity = CVarFilterGuidanceSSAOIntensity.GetValueOnRenderThread();
	PassParameters->IndirectShadowSharpness = CVarIndirectShadowSharpness.GetValueOnRenderThread();
	PassParameters->IndirectShadowIntensity = CVarIndirectShadowIntensity.GetValueOnRenderThread();
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->PointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->DiffuseIndirectTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DiffuseSpatialFilterOutput));
	PassParameters->IndirectShadowTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(IndirectShadowSpatialFilterOutput));
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ENoAction);

	TShaderMapRef<FScreenPassVS> VertexShader(View.ShaderMap);
	FRHIBlendState* BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();
	FRHIDepthStencilState* DepthStencilState = FScreenPassPipelineState::FDefaultDepthStencilState::GetRHI();
	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("RealtimeGIDiffuseComposite"), 
		View, OutputViewport, InputViewport, VertexShader, PixelShader, 
		BlendState, DepthStencilState, PassParameters
	);
}

void FRealtimeGIScreenGatherPipeline::SpecularComposite(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture)
{
	FScreenPassTextureViewport InputViewport(SceneTextureRTSize);
	FScreenPassTextureViewport OutputViewport(SceneTextureRTSize);

	TShaderMapRef<FSpecularCompositePS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FSpecularCompositePS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTextures = SceneTexturesUniformBuffer;
	PassParameters->SceneTextureRTSize = SceneTextureRTSize;
	PassParameters->NormalDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(NormalDepthTexture));
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->SpecularIndirectTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpecularSpatialFilterOutput));
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ENoAction);

	PassParameters->PreIntegratedGF = GSystemTextures.PreintegratedGF->GetRenderTargetItem().ShaderResourceTexture;
	PassParameters->PreIntegratedGFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	TShaderMapRef<FScreenPassVS> VertexShader(View.ShaderMap);
	FRHIBlendState* BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();
	FRHIDepthStencilState* DepthStencilState = FScreenPassPipelineState::FDefaultDepthStencilState::GetRHI();
	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("RealtimeGISpecularComposite"),
		View, OutputViewport, InputViewport, VertexShader, PixelShader,
		BlendState, DepthStencilState, PassParameters
	);
}

void FRealtimeGIScreenGatherPipeline::VisualizeRealtimeGIScreenGather(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "VisualizeRealtimeGIScreenGather");

	int32 VisualizeMode = CVarVisualizeScreenGather.GetValueOnRenderThread();
	if (VisualizeMode <= 0)
	{
		return;
	}

	if (VisualizeMode == 1)
	{
		SimpleBlit(GraphBuilder, Scene, View, DiffuseResolveOutputTexture, SceneColorTexture);
	}
	else if (VisualizeMode == 2)
	{
		SimpleBlit(GraphBuilder, Scene, View, DiffuseTemporalFilterOutput, SceneColorTexture);
	}
	else if (VisualizeMode == 3)
	{
		SimpleBlit(GraphBuilder, Scene, View, DiffuseSpatialFilterOutput, SceneColorTexture);
	}
	else if (VisualizeMode == 4)
	{
		SimpleBlit(GraphBuilder, Scene, View, SpecularResolveOutputTexture, SceneColorTexture);
	}
	else if (VisualizeMode == 5)
	{
		SimpleBlit(GraphBuilder, Scene, View, SpecularTemporalFilterOutput, SceneColorTexture);
	}
	else if (VisualizeMode == 6)
	{
		SimpleBlit(GraphBuilder, Scene, View, SpecularSpatialFilterOutput, SceneColorTexture);
	}
	else if (VisualizeMode == 7)
	{
		SimpleBlit(GraphBuilder, Scene, View, IndirectShadowSpatialFilterOutput, SceneColorTexture);
	}
}

void FRealtimeGIScreenGatherPipeline::RealtimeGICacheSceneColor(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RealtimeGICacheSceneColor");
	SimpleBlit(GraphBuilder, Scene, View, SceneColorTexture, SceneColorHistory.RDGTexture);
	for (int32 i = 0; i < SceneColorHistory.RDGTexture->Desc.NumMips - 1; i++)
	{
		SimpleBlit(GraphBuilder, Scene, View, SceneColorHistory.RDGTexture, SceneColorHistory.RDGTexture, i, i + 1);
	}
}

void RealtimeGIScreenGather(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RealtimeGIScreenGather");
	DECLARE_GPU_STAT(RealtimeGIScreenGather);
	RDG_GPU_STAT_SCOPE(GraphBuilder, RealtimeGIScreenGather);

	for (FViewInfo& View : Views)
	{
		View.RealtimeGIScreenGatherPipeline->Setup(SceneTexturesUniformBuffer);
		View.RealtimeGIScreenGatherPipeline->Update(GraphBuilder, Scene, View);
	}
}

void RealtimeGIDiffuseComposite(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views, FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture)
{
	for (FViewInfo& View : Views)
	{
		FRealtimeGIScreenGatherPipeline* ScreenGatherPipeline = View.RealtimeGIScreenGatherPipeline;
		ScreenGatherPipeline->DiffuseComposite(GraphBuilder, Scene, View, SceneColorTexture, SceneDepthTexture);
	}
}

void RealtimeGISpecularComposite(
	FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture
)
{
	for (FViewInfo& View : Views)
	{
		FRealtimeGIScreenGatherPipeline* ScreenGatherPipeline = View.RealtimeGIScreenGatherPipeline;
		ScreenGatherPipeline->SpecularComposite(GraphBuilder, Scene, View, SceneColorTexture, SceneDepthTexture);
	}
}

void RealtimeGICacheSceneColor(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views, FRDGTextureRef SceneColorTexture)
{
	for (FViewInfo& View : Views)
	{
		FRealtimeGIScreenGatherPipeline* ScreenGatherPipeline = View.RealtimeGIScreenGatherPipeline;
		ScreenGatherPipeline->RealtimeGICacheSceneColor(GraphBuilder, Scene, View, SceneColorTexture);
	}
}

void VisualizeRealtimeGIScreenGather(
	FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture
)
{
	FRealtimeGIScreenGatherPipeline* ScreenGatherPipeline = View.RealtimeGIScreenGatherPipeline;
	ScreenGatherPipeline->VisualizeRealtimeGIScreenGather(GraphBuilder, Scene, View, SceneColorTexture, SceneDepthTexture);
}

