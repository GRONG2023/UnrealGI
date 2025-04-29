#include "RealtimeGIVoxelClipmap.h"

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "MeshPassProcessor.h"
#include "SceneRendering.h"
#include "Renderer/Private/ScenePrivate.h"
#include "SceneTextureParameters.h"
#include "RealtimeGI/RealtimeGIScreenGather.h"

// #pragma optimize ("", off)

#define DEBUG_CHUNK_ROLLING_UPDATE 0

static TAutoConsoleVariable<int32> CVarNumChunksToUpdatePerFrame(
	TEXT("r.RealtimeGI.NumChunksToUpdatePerFrame"),
	64,
	TEXT("Number of voxel chunks to update"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarNumCullingObjectsPerChunk(
	TEXT("r.RealtimeGI.NumCullingObjectsPerChunk"),
	64,
	TEXT("Number of objects in update chunk"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarNumVoxelClips(
	TEXT("r.RealtimeGI.NumVoxelClips"),
	4,
	TEXT("Number of voxel clipmap levels"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarVoxelCellSize(
	TEXT("r.RealtimeGI.VoxelCellSize"),
	0.2,
	TEXT("Cell size of voxel clip 0 (unit is meter)"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarVoxelVisualizeMode(
	TEXT("r.RealtimeGI.VoxelVisualizeMode"),
	0,
	TEXT("Debug view mode of voxel scene, 0 is off, 1 is BaseColor, 2 is Normal, 3 is Emissive, 4 is Depth, 5 is Radiance"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarVoxelVisualizeClipmapLevel(
	TEXT("r.RealtimeGI.VoxelVisualizeClipmapLevel"),
	0,
	TEXT("Clipmap level for visualize voxel"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarVoxelVisualizeUpdateChunk(
	TEXT("r.RealtimeGI.VisualizeUpdateChunk"),
	0,
	TEXT("0 is disable, 1, 2, 3 will visualize update chunks for clip 0, 1, 2"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarClipmapObjectCullingRejectFactor0(
	TEXT("r.RealtimeGI.ClipmapObjectCullingRejectFactor0"),
	1.0,
	TEXT("If object size smaller than (VoxelCellSize x factor), it will be discard in clipmap 0"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarClipmapObjectCullingRejectFactor1(
	TEXT("r.RealtimeGI.ClipmapObjectCullingRejectFactor1"),
	2.0,
	TEXT("If object size smaller than (VoxelCellSize x factor), it will be discard in clipmap 1"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarClipmapObjectCullingRejectFactor2(
	TEXT("r.RealtimeGI.ClipmapObjectCullingRejectFactor2"),
	3.0,
	TEXT("If object size smaller than (VoxelCellSize x factor), it will be discard in clipmap 2"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarClipmapObjectCullingRejectFactor3(
	TEXT("r.RealtimeGI.ClipmapObjectCullingRejectFactor3"),
	4.0,
	TEXT("If object size smaller than (VoxelCellSize x factor), it will be discard in clipmap 3"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarVolumeResolutionLevel(
	TEXT("r.RealtimeGI.VolumeResolutionLevel"),
	2,
	TEXT("Value 0, 1, 2, map to different volume resolution config"),
	ECVF_RenderThreadSafe
);

class FCullObjectToClipmapCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCullObjectToClipmapCS);
	SHADER_USE_PARAMETER_STRUCT(FCullObjectToClipmapCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumObjects)
		SHADER_PARAMETER(float, RejectFactor)
		SHADER_PARAMETER(float, VoxelCellSize)
		SHADER_PARAMETER(FVector, VolumeCenter)
		SHADER_PARAMETER(FVector, VolumeCoverRange)
		SHADER_PARAMETER_SRV(StructuredBuffer, MiniObjectInfoBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWClipmapCullingResult)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWClipmapObjectCounter)
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
IMPLEMENT_GLOBAL_SHADER(FCullObjectToClipmapCS, "/Engine/Private/RealtimeGI/VoxelClipmapUpdate.usf", "CullObjectToClipmapCS", SF_Compute);

class FBuildUpdateChunkCullingIndirectArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildUpdateChunkCullingIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildUpdateChunkCullingIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumUpdateChunks)
		SHADER_PARAMETER(int32, NumThreadsForCulling)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ClipmapObjectCounter)
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
IMPLEMENT_GLOBAL_SHADER(FBuildUpdateChunkCullingIndirectArgsCS, "/Engine/Private/RealtimeGI/VoxelClipmapUpdate.usf", "BuildUpdateChunkCullingIndirectArgsCS", SF_Compute);

class FCullObjectToUpdateChunkCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCullObjectToUpdateChunkCS);
	SHADER_USE_PARAMETER_STRUCT(FCullObjectToUpdateChunkCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumUpdateChunks)
		SHADER_PARAMETER(int32, MaxObjectNumPerUpdateChunk)
		SHADER_PARAMETER(FIntVector, VolumeResolution)
		SHADER_PARAMETER(FIntVector, UpdateChunkResolution)
		SHADER_PARAMETER(FVector, VolumeCenter)
		SHADER_PARAMETER(FVector, VolumeCoverRange)
		SHADER_PARAMETER_SRV(StructuredBuffer, UpdateChunkList)
		SHADER_PARAMETER_SRV(StructuredBuffer, MiniObjectInfoBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ClipmapCullingResult)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ClipmapObjectCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWUpdateChunkCullingResult)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWUpdateChunkObjectCounter)
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
IMPLEMENT_GLOBAL_SHADER(FCullObjectToUpdateChunkCS, "/Engine/Private/RealtimeGI/VoxelClipmapUpdate.usf", "CullObjectToUpdateChunkCS", SF_Compute);

class FVoxelInjectCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelInjectCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelInjectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, SurfaceCacheAtlasResolution)
		SHADER_PARAMETER(int32, MaxObjectNumPerUpdateChunk)
		SHADER_PARAMETER(int32, ClipIndex)
		SHADER_PARAMETER(FIntVector, VolumeResolution)
		SHADER_PARAMETER(FIntVector, UpdateChunkResolution)
		SHADER_PARAMETER(FVector, VolumeCenter)
		SHADER_PARAMETER(FVector, VolumeCoverRange)
		SHADER_PARAMETER(FIntVector, VolumeScrolling)
		SHADER_PARAMETER(FIntVector, NumVoxelPagesInXYZ)
		SHADER_PARAMETER_SRV(StructuredBuffer, UpdateChunkList)
		SHADER_PARAMETER_SRV(StructuredBuffer, ObjectInfoBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer, SurfaceCacheInfoBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer, CardInfoBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, UpdateChunkCullingResult)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, UpdateChunkObjectCounter)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SurfaceCacheAtlasBaseColor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SurfaceCacheAtlasNormal)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SurfaceCacheAtlasEmissive)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SurfaceCacheAtlasDepth)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWVoxelBitOccupyClipmap)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWVoxelPageClipmap)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWVoxelPageFreeList)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWVoxelPageReleaseList)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWVoxelPoolBaseColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWVoxelPoolNormal)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWVoxelPoolEmissive)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWDistanceFieldClipmap)
	END_SHADER_PARAMETER_STRUCT()

	static const uint32 ThreadGroupSizeX = VOXEL_BLOCK_SIZE;
	static const uint32 ThreadGroupSizeY = VOXEL_BLOCK_SIZE;
	static const uint32 ThreadGroupSizeZ = VOXEL_BLOCK_SIZE;

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
IMPLEMENT_GLOBAL_SHADER(FVoxelInjectCS, "/Engine/Private/RealtimeGI/VoxelClipmapUpdate.usf", "VoxelInjectCS", SF_Compute);

class FBuildVoxelPageReleaseIndirectArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBuildVoxelPageReleaseIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildVoxelPageReleaseIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumThreadsForPageRelease)
		SHADER_PARAMETER(FIntVector, NumVoxelPagesInXYZ)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWVoxelPageFreeList)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWVoxelPageReleaseList)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWNumPagesToReleaseCounter)
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
IMPLEMENT_GLOBAL_SHADER(FBuildVoxelPageReleaseIndirectArgsCS, "/Engine/Private/RealtimeGI/VoxelClipmapUpdate.usf", "BuildVoxelPageReleaseIndirectArgsCS", SF_Compute);

class FVoxelPageReleaseCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelPageReleaseCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPageReleaseCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, NumVoxelPagesInXYZ)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWVoxelPageFreeList)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWVoxelPageReleaseList)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, NumPagesToReleaseCounter)
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
IMPLEMENT_GLOBAL_SHADER(FVoxelPageReleaseCS, "/Engine/Private/RealtimeGI/VoxelClipmapUpdate.usf", "VoxelPageReleaseCS", SF_Compute);

class FVisualizeVoxelPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVisualizeVoxelPS);
	SHADER_USE_PARAMETER_STRUCT(FVisualizeVoxelPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FVoxelRaytracingParameters, VoxelRaytracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FProbeVolumeParameters, ProbeVolumeParameters)
		SHADER_PARAMETER(int32, VisualizeMode)
		SHADER_PARAMETER(int32, VisualizeClipmapLevel)
		SHADER_PARAMETER(int32, VisualizeUpdateChunk)
		SHADER_PARAMETER(int32, NumUpdateChunks)
		SHADER_PARAMETER(FVector, UpdateChunkResolution)
		SHADER_PARAMETER_SRV(StructuredBuffer, UpdateChunkList)
		SHADER_PARAMETER_SRV(StructuredBuffer, ObjectInfoBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneDepthTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FUseDistanceField : SHADER_PERMUTATION_BOOL("USE_DISTANCE_FIELD");
	using FPermutationDomain = TShaderPermutationDomain<FUseDistanceField>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVisualizeVoxelPS, "/Engine/Private/RealtimeGI/VisualizeVoxel.usf", "MainPS", SF_Pixel);

class FDistanceFieldPropagateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FDistanceFieldPropagateCS);
	SHADER_USE_PARAMETER_STRUCT(FDistanceFieldPropagateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, ClipIndex)
		SHADER_PARAMETER(FIntVector, VolumeResolution)
		SHADER_PARAMETER(FIntVector, VolumeScrolling)
		SHADER_PARAMETER(FVector, VolumeCoverRange)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, DistanceFieldClipmap)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D, RWDistanceFieldClipmap)
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
IMPLEMENT_GLOBAL_SHADER(FDistanceFieldPropagateCS, "/Engine/Private/RealtimeGI/VoxelClipmapUpdate.usf", "DistanceFieldPropagateCS", SF_Compute);


bool FRealtimeGIVolumeInfo::HasChunksToUpdate()
{
	return PendingUpdateChunks.size() > 0;
}

bool FRealtimeGIVolumeInfo::PopUpdateChunk(FUpdateChunk& OutElement)
{
	if (PendingUpdateChunks.size() == 0)
	{
		return false;
	}

	OutElement = PendingUpdateChunks.front();
	PendingUpdateChunks.pop_front();
	UpdateChunksLookUp.Remove(OutElement.Index1D);

	return true;
}

void FRealtimeGIVolumeInfo::PushUpdateChunk(const FUpdateChunk& InElement)
{
	bool IsAlreadyInSet = false;
	UpdateChunksLookUp.Add(InElement.Index1D, &IsAlreadyInSet);
	if (IsAlreadyInSet)
	{
		return;
	}

	PendingUpdateChunks.push_back(InElement);
}

void FRealtimeGIVolumeInfo::PopulateUpdateChunkList()
{
	const int32 MaxUpdateChunkPerFrame = CVarNumChunksToUpdatePerFrame.GetValueOnRenderThread();

	ChunksToUpdate.Empty();

	// fetch from pending queue
	for (int32 i = 0; i < MaxUpdateChunkPerFrame; i++)
	{
		FUpdateChunk Chunk;
		if (!PopUpdateChunk(Chunk))
		{
			break;
		}

		ChunksToUpdate.Add(Chunk.Index1D);
	}
}

void FRealtimeGIVolumeInfo::PopulateUpdateChunkCleanupList(uint32 FrameIndex)
{
	ChunksToCleanup.Empty();

	// if a chunk been marked as dirty in this frame, we cleanup it
	for (const FUpdateChunk& DirtyChunk : PendingUpdateChunks)
	{
		if (DirtyChunk.TimeStamp == FrameIndex)
		{
			ChunksToCleanup.Add(DirtyChunk.Index1D);
		}
	}
}

FVoxelRaytracingParameters FRealtimeGIVoxelClipmap::SetupVoxelRaytracingParameters(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIRadianceCache* RadianceCache = View.RealtimeGIRadianceCache;

	FVoxelRaytracingParameters PassParameters;

	// volume
	PassParameters.ClipIndex = ClipId;
	PassParameters.NumClips = ClipmapInfos.Num();
	PassParameters.VolumeResolution = VolumeResolution;
	for (int32 ClipIndex = 0; ClipIndex < ClipmapInfos.Num(); ClipIndex++)
	{
		PassParameters.VolumeCenterArray[ClipIndex] = ClipmapInfos[ClipIndex].Center;
		PassParameters.VolumeCoverRangeArray[ClipIndex] = ClipmapInfos[ClipIndex].CoverRange;
		PassParameters.VolumeScrollingArray[ClipIndex] = ClipmapInfos[ClipIndex].Scrolling;
	}

	// voxel
	PassParameters.NumVoxelPagesInXYZ = NumVoxelPagesInXYZ;
	PassParameters.VoxelBitOccupyClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelBitOccupyClipmap.RDGTexture));
	PassParameters.VoxelPageClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelPageClipmap.RDGTexture));
	PassParameters.VoxelPoolBaseColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelPoolBaseColor.RDGTexture));
	PassParameters.VoxelPoolNormal = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelPoolNormal.RDGTexture));
	PassParameters.VoxelPoolEmissive = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelPoolEmissive.RDGTexture));
	PassParameters.VoxelPoolRadiance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(RadianceCache->VoxelPoolRadiance.RDGTexture));
	PassParameters.DistanceFieldClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(GetDistanceFieldClipmap().RDGTexture));
	
	// skylight
	FReflectionUniformParameters ReflectionUniformParameters;
	SetupReflectionUniformParameters(View, ReflectionUniformParameters);
	PassParameters.ReflectionsParameters = CreateUniformBufferImmediate(ReflectionUniformParameters, UniformBuffer_SingleDraw);
	PassParameters.View = View.ViewUniformBuffer;
	PassParameters.LinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters.PointSampler = TStaticSamplerState<SF_Point>::GetRHI();

	return PassParameters;
}

void FRealtimeGIVoxelClipmap::Update(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	PrepareRenderResources(GraphBuilder, Scene, View);

	for (int32 ClipId = 0; ClipId < NumClips; ClipId++)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "RealtimeGIVoxelClipmapUpdate_Clip%d", ClipId);

		UpdateVolumePosition(View, ClipId);

		MarkDirtyChunksToUpdate(Scene, View, ClipId);

		UploadChunkIds(GraphBuilder, Scene, View, ClipId);

		CullObjectToClipmap(GraphBuilder, Scene, View, ClipId);

		CullObjectToUpdateChunk(GraphBuilder, Scene, View, ClipId);

		VoxelInject(GraphBuilder, Scene, View, ClipId);

		DistanceFieldPropagate(GraphBuilder, Scene, View, ClipId);
	}

	ReleaseVoxelPage(GraphBuilder, Scene, View);
}

void FRealtimeGIVoxelClipmap::UpdateVolumePosition(FViewInfo& View, int32 ClipId)
{
	const FVector& ViewLocation = View.ViewLocation;
	FRealtimeGIVolumeInfo& VolumeInfo = ClipmapInfos[ClipId];

	FVector VoxelGridSize = VolumeInfo.CoverRange / FVector(VolumeResolution);
	FIntVector ChunkResolution = VolumeInfo.UpdateChunkResolution;
	FVector ChunkSize = VoxelGridSize * FVector(ChunkResolution);

	// 1. calc moded volume center, min rolling step is an update chunk
	FVector ViewPosGridId = FloorToInt3(ViewLocation / ChunkSize);
	FVector VolumePosGridId = FloorToInt3(VolumeInfo.Center / ChunkSize);
	FIntVector DeltaChunk = FIntVector(ViewPosGridId) - FIntVector(VolumePosGridId);

	// 2. calc scrolling address
	VolumeInfo.Scrolling += Int3Mul(DeltaChunk, ChunkResolution);	// unit is voxel
	VolumeInfo.Scrolling.X = VolumeInfo.Scrolling.X % VolumeResolution.X;
	VolumeInfo.Scrolling.Y = VolumeInfo.Scrolling.Y % VolumeResolution.Y;
	VolumeInfo.Scrolling.Z = VolumeInfo.Scrolling.Z % VolumeResolution.Z;

	VolumeInfo.Scrolling.X += (VolumeInfo.Scrolling.X < 0) ? VolumeResolution.X : 0;
	VolumeInfo.Scrolling.Y += (VolumeInfo.Scrolling.Y < 0) ? VolumeResolution.Y : 0;
	VolumeInfo.Scrolling.Z += (VolumeInfo.Scrolling.Z < 0) ? VolumeResolution.Z : 0;

	// 3. update volume new pos
	VolumeInfo.Center = ViewPosGridId * ChunkSize;
	VolumeInfo.DeltaChunk = DeltaChunk;
}

void FRealtimeGIVoxelClipmap::MarkDirtyChunksToUpdate(FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIVolumeInfo& VolumeInfo = ClipmapInfos[ClipId];

	FVector VoxelGridSize = VolumeInfo.CoverRange / FVector(VolumeResolution);
	FVector ChunkSize = VoxelGridSize * FVector(VolumeInfo.UpdateChunkResolution);
	FIntVector NumChunksInXYZ = Int3Div(VolumeResolution, VolumeInfo.UpdateChunkResolution);
	FIntVector DeltaChunk = VolumeInfo.DeltaChunk;

	// 1. move pending dirty chunks that haven't been update
	TArray<FUpdateChunk> DirtyChunks;
	while (VolumeInfo.HasChunksToUpdate())
	{
		FUpdateChunk UpdateChunk;
		VolumeInfo.PopUpdateChunk(UpdateChunk);
		DirtyChunks.Add(UpdateChunk);
	}
	for (FUpdateChunk& DirtyChunk : DirtyChunks)
	{
		FIntVector ChunkIndex3D = Index1DTo3DLinear(DirtyChunk.Index1D, NumChunksInXYZ);
		FIntVector MovedChunkIndex3D = ChunkIndex3D - DeltaChunk;

		MovedChunkIndex3D.X = MovedChunkIndex3D.X % NumChunksInXYZ.X;
		MovedChunkIndex3D.Y = MovedChunkIndex3D.Y % NumChunksInXYZ.Y;
		MovedChunkIndex3D.Z = MovedChunkIndex3D.Z % NumChunksInXYZ.Z;

		MovedChunkIndex3D.X += (MovedChunkIndex3D.X < 0) ? NumChunksInXYZ.X : 0;
		MovedChunkIndex3D.Y += (MovedChunkIndex3D.Y < 0) ? NumChunksInXYZ.Y : 0;
		MovedChunkIndex3D.Z += (MovedChunkIndex3D.Z < 0) ? NumChunksInXYZ.Z : 0;

		// we don't record time stamp here, cause this chunk is added before
		DirtyChunk.Index1D = Index3DTo1DLinear(MovedChunkIndex3D, NumChunksInXYZ);
		VolumeInfo.PushUpdateChunk(DirtyChunk);
	}

	// 2. mark XZ plane's new coming chunks as dirty if volume move along Y axis
	// for 8x8x8 block, we may mark [0~8, 0~1, 0~8] as dirty when volume move 2 block in Y axis
	auto MarkChunkPlaneAsDirty = [this, NumChunksInXYZ, DeltaChunk, &VolumeInfo](int32 Axis)
	{
		int32 DeltaChunkInAxis = DeltaChunk[Axis];
		int32 NumChunksInX = NumChunksInXYZ[(Axis + 0) % 3];
		int32 NumChunksInY = NumChunksInXYZ[(Axis + 1) % 3];
		int32 NumChunksInZ = NumChunksInXYZ[(Axis + 2) % 3];

		if (DeltaChunkInAxis == 0)
		{
			return;
		}

		int32 Start = 0, End = 0;
		if (DeltaChunkInAxis > 0)	// [112 ~ 128]
		{
			Start = NumChunksInX - FMath::Abs(DeltaChunkInAxis);
			End = NumChunksInX - 1;
		}
		else	// [0 ~ 16]
		{
			Start = 0;
			End = FMath::Abs(DeltaChunkInAxis) - 1;
		}

		Start = FMath::Max(Start, 0);
		End = FMath::Min(End, NumChunksInX - 1);

		for (int32 X = Start; X <= End; X++)
		{
			for (int32 Y = 0; Y < NumChunksInY; Y++)
			{
				for (int32 Z = 0; Z < NumChunksInZ; Z++)
				{
					FIntVector ChunkIndex3D;
					ChunkIndex3D[(Axis + 0) % 3] = X;
					ChunkIndex3D[(Axis + 1) % 3] = Y;
					ChunkIndex3D[(Axis + 2) % 3] = Z;

					FUpdateChunk DirtyChunk;
					DirtyChunk.Index1D = Index3DTo1DLinear(ChunkIndex3D, NumChunksInXYZ);
					DirtyChunk.TimeStamp = FrameNumberRenderThread;
					VolumeInfo.PushUpdateChunk(DirtyChunk);
				}
			}
		}
	};

	MarkChunkPlaneAsDirty(0);	// move in X, dirty in YZ
	MarkChunkPlaneAsDirty(1);	// move in Y, dirty in XZ
	MarkChunkPlaneAsDirty(2);	// move in Z, dirty in XY

	// 3. mark chunks as dirty when primitive move
	for (const FBoxSphereBounds& Bound : Scene->RealtimeGIScene.DirtyPrimitiveBounds)
	{
		const FBox& Box = Bound.GetBox();
		FVector BoxMin = Box.Min - VoxelGridSize * 2;	// a little padding
		FVector BoxMax = Box.Max + VoxelGridSize * 2;
		FIntVector MinCornerChunk = FIntVector(FloorToInt3((BoxMin - VolumeInfo.Center) / ChunkSize)) + (NumChunksInXYZ / 2);
		FIntVector MaxCornerChunk = FIntVector(FloorToInt3((BoxMax - VolumeInfo.Center) / ChunkSize)) + (NumChunksInXYZ / 2);

		MinCornerChunk = Int3Clamp(MinCornerChunk, FIntVector(0, 0, 0), NumChunksInXYZ - FIntVector(1, 1, 1));
		MaxCornerChunk = Int3Clamp(MaxCornerChunk, FIntVector(0, 0, 0), NumChunksInXYZ - FIntVector(1, 1, 1));

		for (int32 X = MinCornerChunk.X; X <= MaxCornerChunk.X; X++)
		{
			for (int32 Y = MinCornerChunk.Y; Y <= MaxCornerChunk.Y; Y++)
			{
				for (int32 Z = MinCornerChunk.Z; Z <= MaxCornerChunk.Z; Z++)
				{
					FUpdateChunk DirtyChunk;
					DirtyChunk.Index1D = Index3DTo1DLinear(FIntVector(X, Y, Z), NumChunksInXYZ);
					DirtyChunk.TimeStamp = FrameNumberRenderThread;
					VolumeInfo.PushUpdateChunk(DirtyChunk);
				}
			}
		}
	}
}

void FRealtimeGIVoxelClipmap::PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	const int32 CVarNumClips = FMath::Clamp(CVarNumVoxelClips.GetValueOnRenderThread(), 1, MAX_CLIP_NUM);
	const float CVarCellSize = CVarVoxelCellSize.GetValueOnRenderThread() * 100.0f;
	const float CellSizeOld = (ClipmapInfos.Num() > 0) ? (ClipmapInfos[0].CoverRange.X / VolumeResolution.X) : 0;
	const int32 CVarVolumeResolution = GetVolumeResolution(CVarVolumeResolutionLevel.GetValueOnRenderThread());
	const int32 VolumeResolutionOld = VolumeResolution.X;

	bool NeedResetVolume = false;
	NeedResetVolume |= (NumClips == 0);
	NeedResetVolume |= (NumClips != CVarNumClips);
	NeedResetVolume |= (CellSizeOld != CVarCellSize);
	NeedResetVolume |= (VolumeResolutionOld != CVarVolumeResolution);

	if (NeedResetVolume)
	{
		NumClips = CVarNumClips;
		ClipmapInfos.SetNum(NumClips);
		VolumeResolution = FIntVector(CVarVolumeResolution, CVarVolumeResolution, CVarVolumeResolution);

		for (int32 ClipId = 0; ClipId < ClipmapInfos.Num(); ClipId++)
		{
			FRealtimeGIVolumeInfo& VolumeInfo = ClipmapInfos[ClipId];
			VolumeInfo.UpdateChunkResolution = VolumeResolution / UPDATE_CHUNK_NUM;

			FIntVector NumChunksInXYZ = Int3Div(VolumeResolution, VolumeInfo.UpdateChunkResolution);
			int32 UpdateChunkNum = NumChunksInXYZ.X * NumChunksInXYZ.Y * NumChunksInXYZ.Z;

			VolumeInfo.NumChunksInXYZ = NumChunksInXYZ;
			VolumeInfo.CoverRange = FVector(VolumeResolution) * CVarCellSize * (1 << ClipId);
			VolumeInfo.Scrolling = FIntVector(0, 0, 0);

			// mark all chunks as dirty
			for (int32 ChunkId = 0; ChunkId < UpdateChunkNum; ChunkId++)
			{
				FUpdateChunk DirtyChunk;
				DirtyChunk.Index1D = ChunkId;
				DirtyChunk.TimeStamp = FrameNumberRenderThread;
				VolumeInfo.PushUpdateChunk(DirtyChunk);
			}
		}

		// reset allocators when clip parameters change, if NumClips change from 3 to 2, we will lose those allocated pages
		if (VoxelPageFreeList.NumBytes > 0)
		{
			VoxelPageFreeList.Release();
		}
		if (VoxelPageClipmap.PooledTexture.IsValid())
		{
			VoxelPageClipmap.PooledTexture = nullptr;
		}
	}

	const int32 MaxObjectNumPerClip = Scene->RealtimeGIScene.ObjectIdAllocator.GetMaxNumElements();
	const int32 MaxUpdateChunkPerFrame = CVarNumChunksToUpdatePerFrame.GetValueOnRenderThread();
	const int32 MaxObjectNumPerUpdateChunk = CVarNumCullingObjectsPerChunk.GetValueOnRenderThread();

	const int32 UpdateChunkListNumBytes = sizeof(int32) * MaxUpdateChunkPerFrame;
	const int32 UpdateChunkCleanupListNumBytes = sizeof(int32) * UPDATE_CHUNK_NUM * UPDATE_CHUNK_NUM * UPDATE_CHUNK_NUM;

	for (int32 ClipId = 0; ClipId < MAX_CLIP_NUM; ClipId++)
	{
		if (UpdateChunkList[ClipId].NumBytes == 0 || UpdateChunkList[ClipId].NumBytes != UpdateChunkListNumBytes)
		{
			UpdateChunkList[ClipId].Initialize(
				UpdateChunkListNumBytes,
				BUF_Dynamic, *FString::Printf(TEXT("UpdateChunkList_Clip%d"), ClipId)
			);
		}

		if (UpdateChunkCleanupList[ClipId].NumBytes == 0 || UpdateChunkCleanupList[ClipId].NumBytes != UpdateChunkCleanupListNumBytes)
		{
			UpdateChunkCleanupList[ClipId].Initialize(
				UpdateChunkCleanupListNumBytes,
				BUF_Dynamic, *FString::Printf(TEXT("UpdateChunkCleanupList_Clip%d"), ClipId)
			);
		}
	}

	ClipmapObjectCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
		TEXT("ClipmapObjectCounter")
	);

	ClipmapCullingResult = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), MaxObjectNumPerClip),
		TEXT("ClipmapCullingResult")
	);

	UpdateChunkCullingIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("UpdateChunkCullingIndirectArgs")
	);

	UpdateChunkCullingResult = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), MaxUpdateChunkPerFrame * MaxObjectNumPerUpdateChunk),
		TEXT("UpdateChunkCullingResult")
	);

	UpdateChunkObjectCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), MaxUpdateChunkPerFrame),
		TEXT("UpdateChunkObjectCounter")
	);

	FIntVector ClipmapResolution = FIntVector(
		VolumeResolution.X / VOXEL_BLOCK_SIZE, 
		VolumeResolution.Y / VOXEL_BLOCK_SIZE,
		VolumeResolution.Z * NumClips / VOXEL_BLOCK_SIZE
	);

	// bit occupy map
	Create3DTexFn(GraphBuilder, VoxelBitOccupyClipmap, ClipmapResolution, PF_R32G32_UINT, TEXT("VoxelBitOccupyClipmap"));
	
	FIntVector DFTextureResolution = ClipmapResolution * VOXEL_BLOCK_SIZE;
	Create3DTexFn(GraphBuilder, DistanceFieldClipmap[0], DFTextureResolution, PF_R8, TEXT("DistanceFieldClipmap0"));
	Create3DTexFn(GraphBuilder, DistanceFieldClipmap[1], DFTextureResolution, PF_R8, TEXT("DistanceFieldClipmap1"));
	
	// page table
	bool IsFirstTimeCreate = Create3DTexFn(GraphBuilder, VoxelPageClipmap, ClipmapResolution, PF_R32_UINT, TEXT("VoxelPageClipmap"));
	if (IsFirstTimeCreate)
	{
		InitTexture3D(GraphBuilder, VoxelPageClipmap, ClipmapResolution, PAGE_ID_INVALID);
	}

	// @TODO: adaptive size, now for 128*128*128*4 clipmap we use 32x32x32 block pages (128x128x128 total)
	Create3DTexFn(GraphBuilder, VoxelPoolBaseColor, NumVoxelPagesInXYZ * VOXEL_BLOCK_SIZE, PF_R8G8B8A8, TEXT("VoxelPoolBaseColor"));
	Create3DTexFn(GraphBuilder, VoxelPoolNormal, NumVoxelPagesInXYZ * VOXEL_BLOCK_SIZE, PF_FloatRGB, TEXT("VoxelPoolNormal"));
	Create3DTexFn(GraphBuilder, VoxelPoolEmissive, NumVoxelPagesInXYZ * VOXEL_BLOCK_SIZE, PF_FloatRGB, TEXT("VoxelPoolEmissive"));


	// free list
	const int32 NumPages = NumVoxelPagesInXYZ.X * NumVoxelPagesInXYZ.Y * NumVoxelPagesInXYZ.Z;
	const int32 NumElementsFreeList = NumPages + 1;	// we use last element as allocator pointer
	const int32 NumBytesFreeList = sizeof(uint32) * NumElementsFreeList;
	if (VoxelPageFreeList.NumBytes == 0 || NeedResetVolume)
	{
		VoxelPageFreeList.Initialize(
			sizeof(int32), NumElementsFreeList,
			BUF_Static, TEXT("VoxelPageFreeList")
		);

		TArray<int32> InitData;
		InitData.SetNum(NumElementsFreeList);
		for (int32 i = 0; i < NumPages; i++)
		{
			InitData[i] = i;
		}

		FRHIStructuredBuffer* BufferRHI = VoxelPageFreeList.Buffer;
		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytesFreeList, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, InitData.GetData(), NumBytesFreeList);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}

	// release list
	if (VoxelPageReleaseList.NumBytes == 0)
	{
		VoxelPageReleaseList.Initialize(
			sizeof(int32), NumElementsFreeList,
			BUF_Static, TEXT("VoxelPageReleaseList")
		);
	}

	VoxelPageReleaseIndirectArgs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc(3),
		TEXT("VoxelPageReleaseIndirectArgs")
	);
}

void FRealtimeGIVoxelClipmap::UploadChunkIds(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	FRealtimeGIVolumeInfo& ClipmapInfo = ClipmapInfos[ClipId];

	// 1. populate array
	ClipmapInfo.PopulateUpdateChunkCleanupList(FrameNumberRenderThread);
	ClipmapInfo.PopulateUpdateChunkList();

	const TArray<int32>& ChunksToUpdate = ClipmapInfo.ChunksToUpdate;
	const TArray<int32>& ChunksToCleanup = ClipmapInfo.ChunksToCleanup;

#if DEBUG_CHUNK_ROLLING_UPDATE
	for (int32 i = 0; i < ChunksToUpdate.Num(); i++)
	{
		FUpdateChunk DirtyChunk;
		DirtyChunk.Index1D = ChunksToUpdate[i];
		DirtyChunk.TimeStamp = FrameNumberRenderThread + 1;
		ClipmapInfo.PushUpdateChunk(DirtyChunk);
	}
#endif	// DEBUG_CHUNK_ROLLING_UPDATE

	// 2. upload update chunk list
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (ClipmapInfo.ChunksToUpdate.Num() > 0)
#endif
	{
		const int32 NumBytes = ChunksToUpdate.Num() * sizeof(int32);
		FRHIStructuredBuffer* UploadBufferRHI = UpdateChunkList[ClipId].Buffer;

		void* MappedRawData = RHICmdList.LockStructuredBuffer(UploadBufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, ChunksToUpdate.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(UploadBufferRHI);
	}
	
	// 3. upload update chunk cleanup list
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (ClipmapInfo.ChunksToCleanup.Num() > 0)
#endif
	{
		const int32 NumBytes = ChunksToCleanup.Num() * sizeof(int32);
		FRHIStructuredBuffer* UploadBufferRHI = UpdateChunkCleanupList[ClipId].Buffer;

		void* MappedRawData = RHICmdList.LockStructuredBuffer(UploadBufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, ChunksToCleanup.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(UploadBufferRHI);
	}
}

void FRealtimeGIVoxelClipmap::CullObjectToClipmap(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVolumeInfo& ClipmapInfo = ClipmapInfos[ClipId];
	const int32 NumObjects = VoxelScene.GetObjectNum();

	TArray<int32> RejectFactors;
	RejectFactors.Add(CVarClipmapObjectCullingRejectFactor0.GetValueOnRenderThread());
	RejectFactors.Add(CVarClipmapObjectCullingRejectFactor1.GetValueOnRenderThread());
	RejectFactors.Add(CVarClipmapObjectCullingRejectFactor2.GetValueOnRenderThread());
	RejectFactors.Add(CVarClipmapObjectCullingRejectFactor3.GetValueOnRenderThread());

#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (ClipmapInfo.ChunksToUpdate.Num() == 0)
	{
		return;
	}
#endif

	ClearCounterBuffer(GraphBuilder, Scene, ClipmapObjectCounter, 1);

	// 1. cull object to clipmap
	TShaderMapRef<FCullObjectToClipmapCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FCullObjectToClipmapCS::FParameters>();
	PassParameters->NumObjects = NumObjects;
	PassParameters->RejectFactor = RejectFactors[ClipId];
	PassParameters->VoxelCellSize = ClipmapInfo.CoverRange.X / float(VolumeResolution.X);
	PassParameters->VolumeCenter = ClipmapInfo.Center;
	PassParameters->VolumeCoverRange = ClipmapInfo.CoverRange;
	PassParameters->MiniObjectInfoBuffer = VoxelScene.MiniObjectInfoBuffer.SRV;
	PassParameters->RWClipmapCullingResult = GraphBuilder.CreateUAV(ClipmapCullingResult);
	PassParameters->RWClipmapObjectCounter = GraphBuilder.CreateUAV(ClipmapObjectCounter);

	const int32 NumGroups = FMath::CeilToInt(float(NumObjects) / float(FCullObjectToClipmapCS::ThreadGroupSizeX));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("CullObjectToClipmap"),
		ComputeShader, PassParameters, FIntVector(NumGroups, 1, 1));
}

void FRealtimeGIVoxelClipmap::CullObjectToUpdateChunk(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVolumeInfo& ClipmapInfo = ClipmapInfos[ClipId];
	const int32 MaxObjectNumPerUpdateChunk = CVarNumCullingObjectsPerChunk.GetValueOnRenderThread();
	const int32 NumChunksToUpdate = CVarNumChunksToUpdatePerFrame.GetValueOnRenderThread();

#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (ClipmapInfo.ChunksToUpdate.Num() == 0)
	{
		return;
	}
#endif

	// 1. build indirect dispatch args
	{
		TShaderMapRef<FBuildUpdateChunkCullingIndirectArgsCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FBuildUpdateChunkCullingIndirectArgsCS::FParameters>();
		PassParameters->NumUpdateChunks = ClipmapInfo.ChunksToUpdate.Num();
		PassParameters->NumThreadsForCulling = FCullObjectToUpdateChunkCS::ThreadGroupSizeX;
		PassParameters->ClipmapObjectCounter = GraphBuilder.CreateSRV(ClipmapObjectCounter);
		PassParameters->RWIndirectArgs = GraphBuilder.CreateUAV(UpdateChunkCullingIndirectArgs);

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("BuildUpdateChunkCullingIndirectArgs"),
			ComputeShader, PassParameters, FIntVector(1, 1, 1));
	}

	// 2. dispatch culling thread
	{
		ClearCounterBuffer(GraphBuilder, Scene, UpdateChunkObjectCounter, NumChunksToUpdate);

		TShaderMapRef<FCullObjectToUpdateChunkCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FCullObjectToUpdateChunkCS::FParameters>();
		PassParameters->NumUpdateChunks = ClipmapInfo.ChunksToUpdate.Num();
		PassParameters->MaxObjectNumPerUpdateChunk = MaxObjectNumPerUpdateChunk;
		PassParameters->VolumeResolution = VolumeResolution;
		PassParameters->UpdateChunkResolution = ClipmapInfo.UpdateChunkResolution;
		PassParameters->VolumeCenter = ClipmapInfo.Center;
		PassParameters->VolumeCoverRange = ClipmapInfo.CoverRange;
		PassParameters->UpdateChunkList = UpdateChunkList[ClipId].SRV;
		PassParameters->MiniObjectInfoBuffer = VoxelScene.MiniObjectInfoBuffer.SRV;
		PassParameters->ClipmapCullingResult = GraphBuilder.CreateSRV(ClipmapCullingResult);
		PassParameters->ClipmapObjectCounter = GraphBuilder.CreateSRV(ClipmapObjectCounter);
		PassParameters->RWUpdateChunkCullingResult = GraphBuilder.CreateUAV(UpdateChunkCullingResult);
		PassParameters->RWUpdateChunkObjectCounter = GraphBuilder.CreateUAV(UpdateChunkObjectCounter);
		PassParameters->IndirectArgsBuffer = UpdateChunkCullingIndirectArgs;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("CullObjectToUpdateChunk"),
			ComputeShader, PassParameters, UpdateChunkCullingIndirectArgs, 0);
	}
}

void FRealtimeGIVoxelClipmap::VoxelInject(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIVolumeInfo& ClipmapInfo = ClipmapInfos[ClipId];
	const int32 MaxObjectNumPerUpdateChunk = CVarNumCullingObjectsPerChunk.GetValueOnRenderThread();

#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (ClipmapInfo.ChunksToUpdate.Num() == 0)
	{
		return;
	}
#endif

	FVoxelInjectCS::FPermutationDomain Permutation;
	Permutation.Set<FVoxelInjectCS::FUseDistanceField>(RealtimeGIUseDistanceField());

	TShaderMapRef<FVoxelInjectCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
	auto* PassParameters = GraphBuilder.AllocParameters<FVoxelInjectCS::FParameters>();
	PassParameters->SurfaceCacheAtlasResolution = VoxelScene.SurfaceCacheAtlasResolution;
	PassParameters->MaxObjectNumPerUpdateChunk = MaxObjectNumPerUpdateChunk;
	PassParameters->ClipIndex = ClipId;
	PassParameters->VolumeResolution = VolumeResolution;
	PassParameters->UpdateChunkResolution = ClipmapInfo.UpdateChunkResolution;
	PassParameters->VolumeCenter = ClipmapInfo.Center;
	PassParameters->VolumeCoverRange = ClipmapInfo.CoverRange;
	PassParameters->VolumeScrolling = ClipmapInfo.Scrolling;
	PassParameters->NumVoxelPagesInXYZ = NumVoxelPagesInXYZ;
	PassParameters->UpdateChunkList = UpdateChunkList[ClipId].SRV;
	PassParameters->ObjectInfoBuffer = VoxelScene.ObjectInfoBuffer.SRV;
	PassParameters->SurfaceCacheInfoBuffer = VoxelScene.SurfaceCacheInfoBuffer.SRV;
	PassParameters->CardInfoBuffer = VoxelScene.CardInfoBuffer.SRV;
	PassParameters->UpdateChunkCullingResult = GraphBuilder.CreateSRV(UpdateChunkCullingResult);
	PassParameters->UpdateChunkObjectCounter = GraphBuilder.CreateSRV(UpdateChunkObjectCounter);
	PassParameters->SurfaceCacheAtlasBaseColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelScene.SurfaceCacheAtlas[RT_BaseColor].RDGTexture));
	PassParameters->SurfaceCacheAtlasNormal = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelScene.SurfaceCacheAtlas[RT_Normal].RDGTexture));
	PassParameters->SurfaceCacheAtlasEmissive = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelScene.SurfaceCacheAtlas[RT_Emissive].RDGTexture));
	PassParameters->SurfaceCacheAtlasDepth = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(VoxelScene.SurfaceCacheAtlas[RT_Depth].RDGTexture));
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RWVoxelBitOccupyClipmap = GraphBuilder.CreateUAV(VoxelBitOccupyClipmap.RDGTexture);
	PassParameters->RWVoxelPageClipmap = GraphBuilder.CreateUAV(VoxelPageClipmap.RDGTexture);
	PassParameters->RWVoxelPageFreeList = VoxelPageFreeList.UAV;
	PassParameters->RWVoxelPageReleaseList = VoxelPageReleaseList.UAV;
	PassParameters->RWVoxelPoolBaseColor = GraphBuilder.CreateUAV(VoxelPoolBaseColor.RDGTexture);
	PassParameters->RWVoxelPoolNormal = GraphBuilder.CreateUAV(VoxelPoolNormal.RDGTexture);
	PassParameters->RWVoxelPoolEmissive = GraphBuilder.CreateUAV(VoxelPoolEmissive.RDGTexture);
	PassParameters->RWDistanceFieldClipmap = GraphBuilder.CreateUAV(GetDistanceFieldClipmap().RDGTexture);

	const FIntVector NumGroupsPerChunk = FIntVector(
		ClipmapInfo.UpdateChunkResolution.X / FVoxelInjectCS::ThreadGroupSizeX,
		ClipmapInfo.UpdateChunkResolution.Y / FVoxelInjectCS::ThreadGroupSizeY,
		ClipmapInfo.UpdateChunkResolution.Z / FVoxelInjectCS::ThreadGroupSizeZ
	);

	const FIntVector NumGroups = FIntVector(
		NumGroupsPerChunk.X * ClipmapInfo.ChunksToUpdate.Num(),
		NumGroupsPerChunk.Y,
		NumGroupsPerChunk.Z
	);

	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("VoxelInjectToClipMap"),
		ComputeShader, PassParameters, NumGroups);
}

void FRealtimeGIVoxelClipmap::ReleaseVoxelPage(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	bool HasChunkUpdateDispatch = false;
	for (FRealtimeGIVolumeInfo& ClipmapInfo : ClipmapInfos)
	{
		if (ClipmapInfo.ChunksToUpdate.Num() > 0)
		{
			HasChunkUpdateDispatch = true;
		}
	}
	if (!HasChunkUpdateDispatch)
	{
		return;
	}
#endif

	FRDGBufferRef NumPagesToReleaseCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
		TEXT("NumPagesToReleaseCounter")
	);

	// 1. build indirect dispatch args
	{
		TShaderMapRef<FBuildVoxelPageReleaseIndirectArgsCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FBuildVoxelPageReleaseIndirectArgsCS::FParameters>();
		PassParameters->NumThreadsForPageRelease = FVoxelPageReleaseCS::ThreadGroupSizeX;
		PassParameters->NumVoxelPagesInXYZ = NumVoxelPagesInXYZ;
		PassParameters->RWVoxelPageFreeList = VoxelPageFreeList.UAV;
		PassParameters->RWVoxelPageReleaseList = VoxelPageReleaseList.UAV;
		PassParameters->RWNumPagesToReleaseCounter = GraphBuilder.CreateUAV(NumPagesToReleaseCounter);
		PassParameters->RWIndirectArgs = GraphBuilder.CreateUAV(VoxelPageReleaseIndirectArgs);

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("BuildVoxelPageReleaseIndirectArgs"),
			ComputeShader, PassParameters, FIntVector(1, 1, 1));
	}

	// 2. do give empty voxel pages back to free list
	{
		TShaderMapRef<FVoxelPageReleaseCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FVoxelPageReleaseCS::FParameters>();
		PassParameters->NumVoxelPagesInXYZ = NumVoxelPagesInXYZ;
		PassParameters->RWVoxelPageFreeList = VoxelPageFreeList.UAV;
		PassParameters->RWVoxelPageReleaseList = VoxelPageReleaseList.UAV;
		PassParameters->NumPagesToReleaseCounter = GraphBuilder.CreateSRV(NumPagesToReleaseCounter);
		PassParameters->IndirectArgsBuffer = VoxelPageReleaseIndirectArgs;

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("VoxelPageRelease"),
			ComputeShader, PassParameters, VoxelPageReleaseIndirectArgs, 0);
	}
}

void FRealtimeGIVoxelClipmap::DistanceFieldPropagate(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId)
{
	if (!RealtimeGIUseDistanceField())
	{
		return;
	}

	FRealtimeGIVolumeInfo& ClipmapInfo = ClipmapInfos[ClipId];
	FPersistentTexture& DistanceFieldCurFrame = GetDistanceFieldClipmap();
	FPersistentTexture& DistanceFieldNextFrame = GetDistanceFieldClipmapNextFrame();

	TShaderMapRef<FDistanceFieldPropagateCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FDistanceFieldPropagateCS::FParameters>();
	PassParameters->ClipIndex = ClipId;
	PassParameters->VolumeResolution = VolumeResolution;
	PassParameters->VolumeScrolling = ClipmapInfo.Scrolling;
	PassParameters->VolumeCoverRange = ClipmapInfo.CoverRange;
	PassParameters->DistanceFieldClipmap = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DistanceFieldCurFrame.RDGTexture));
	PassParameters->RWDistanceFieldClipmap = GraphBuilder.CreateUAV(DistanceFieldNextFrame.RDGTexture);

	FIntVector NumGroups = FIntVector(
		VolumeResolution.X / FDistanceFieldPropagateCS::ThreadGroupSizeX,
		VolumeResolution.Y / FDistanceFieldPropagateCS::ThreadGroupSizeY,
		VolumeResolution.Z / FDistanceFieldPropagateCS::ThreadGroupSizeZ
	);

	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("DistanceFieldPropagate"),
		ComputeShader, PassParameters, NumGroups
	);
}

void FRealtimeGIVoxelClipmap::VisualizeVoxel(
	FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture)
{
	DECLARE_GPU_STAT(RealtimeGIVisualizeVoxel);
	RDG_GPU_STAT_SCOPE(GraphBuilder, RealtimeGIVisualizeVoxel);

	if (CVarVoxelVisualizeMode.GetValueOnRenderThread() <= 0)
	{
		return;
	}

	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	FRealtimeGIGPUScene& VoxelScene = Scene->RealtimeGIScene;
	FRealtimeGIRadianceCache* RadianceCache = View.RealtimeGIRadianceCache;
	const FScreenPassTextureViewport Viewport(SceneColorTexture);

	FVisualizeVoxelPS::FPermutationDomain Permutation;
	Permutation.Set<FVisualizeVoxelPS::FUseDistanceField>(RealtimeGIUseDistanceField());

	TShaderMapRef<FVisualizeVoxelPS> PixelShader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
	auto* PassParameters = GraphBuilder.AllocParameters<FVisualizeVoxelPS::FParameters>();
	PassParameters->VoxelRaytracingParameters = SetupVoxelRaytracingParameters(GraphBuilder, Scene, View);
	PassParameters->ProbeVolumeParameters = RadianceCache->SetupProbeVolumeParameters(GraphBuilder, Scene, View);

	PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::EClear);
	PassParameters->SceneDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepthTexture));

	PassParameters->VisualizeMode = CVarVoxelVisualizeMode.GetValueOnRenderThread();
	PassParameters->VisualizeClipmapLevel = CVarVoxelVisualizeClipmapLevel.GetValueOnRenderThread();

	const int32 VisualizeUpdateChunk = CVarVoxelVisualizeUpdateChunk.GetValueOnRenderThread();
	const int32 SelectClip = FMath::Clamp(VisualizeUpdateChunk - 1, 0, ClipmapInfos.Num() - 1);
	PassParameters->VisualizeUpdateChunk = FMath::Clamp(VisualizeUpdateChunk, 0, ClipmapInfos.Num());
	PassParameters->NumUpdateChunks = ClipmapInfos[SelectClip].ChunksToUpdate.Num();
	PassParameters->UpdateChunkResolution = FVector(ClipmapInfos[SelectClip].UpdateChunkResolution);
	PassParameters->UpdateChunkList = UpdateChunkList[SelectClip].SRV;

	AddDrawScreenPass(
		GraphBuilder, RDG_EVENT_NAME("VisualizeRealtimeGIVoxelScene"),
		View, Viewport, Viewport, PixelShader, PassParameters
	);
}

void RealtimeGIDebugVisualization(
	FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture)
{

	for (FViewInfo& View : Views)
	{
		FRealtimeGIVoxelClipmap* VoxelClipmap = View.RealtimeGIVoxelClipmap;
		VoxelClipmap->VisualizeVoxel(GraphBuilder, Scene, View, SceneColorTexture, SceneDepthTexture);

		VisualizeRealtimeGIScreenGather(GraphBuilder, Scene, View, SceneColorTexture, SceneDepthTexture);

		FRealtimeGIRadianceCache* RadianceCache = View.RealtimeGIRadianceCache;
		RadianceCache->VisualizeProbe(GraphBuilder, Scene, View, SceneColorTexture, SceneDepthTexture, VM_RadianceProbe);
		RadianceCache->VisualizeProbe(GraphBuilder, Scene, View, SceneColorTexture, SceneDepthTexture, VM_IrradianceProbe);
	}
}
