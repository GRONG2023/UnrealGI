#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "Core/Public/Math/IntVector.h"
#include "ReflectionEnvironment.h"

#define OBJECT_ID_INVALID (-114514)

#define MAX_CARDS_PER_MESH (12)

// we use uint64 to represent 4x4x4 voxel, so single block's size is 4
#define VOXEL_BLOCK_SIZE (4)
#define NUM_VOXEL_PER_BLOCK (VOXEL_BLOCK_SIZE * VOXEL_BLOCK_SIZE * VOXEL_BLOCK_SIZE)

#define MAX_CLIP_NUM (4)

// we divide volume into 8^3 region to update voxel
// also is min scroll step when camera move
#define UPDATE_CHUNK_NUM (8)

#define PAGE_ID_INVALID (0x3FFFFFFF)

#define PROBE_ID_INVALID (0x3FFFFFFF)

#define ALLOW_EMPTY_DISPATCH_FOR_DEBUG 0

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelRaytracingParameters, )
	SHADER_PARAMETER(int32, ClipIndex)
	SHADER_PARAMETER(int32, NumClips)
	SHADER_PARAMETER(FIntVector, VolumeResolution)
	SHADER_PARAMETER_ARRAY(FVector, VolumeCenterArray, [MAX_CLIP_NUM])
	SHADER_PARAMETER_ARRAY(FVector, VolumeCoverRangeArray, [MAX_CLIP_NUM])
	SHADER_PARAMETER_ARRAY(FIntVector, VolumeScrollingArray, [MAX_CLIP_NUM])
	SHADER_PARAMETER(FIntVector, NumVoxelPagesInXYZ)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, VoxelBitOccupyClipmap)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, VoxelPageClipmap)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, VoxelPoolBaseColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, VoxelPoolNormal)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, VoxelPoolEmissive)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, VoxelPoolRadiance)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, DistanceFieldClipmap)
	SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
	SHADER_PARAMETER_SAMPLER(SamplerState, PointSampler)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_REF(FReflectionUniformParameters, ReflectionsParameters)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FProbeVolumeParameters, )
	SHADER_PARAMETER(int32, RadianceProbeResolution)
	SHADER_PARAMETER(FIntPoint, NumRadianceProbesInAtlasXY)
	SHADER_PARAMETER(int32, RadianceProbeMinClipLevel)
	SHADER_PARAMETER(int32, IrradianceProbeMinClipLevel)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, ProbeOffsetClipmap)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, IrradianceProbeClipmap)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D, RadianceProbeIdClipmap)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, RadianceProbeAtlas)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, RadianceProbeDistanceAtlas)
END_SHADER_PARAMETER_STRUCT()


enum ECardCaptureRTSlot
{
	RT_BaseColor = 0,
	RT_Normal,
	RT_Emissive,
	RT_Depth,
	RT_Num
};

enum EVolumeResolution
{
	VR_64 = 0,
	VR_96,
	VR_128,
	VR_Num
};

extern TAutoConsoleVariable<int32> CVarRealtimeGIEnable;
bool RealtimeGIEnable();

extern TAutoConsoleVariable<int32> CVarRealtimeGIUseDistanceField;
bool RealtimeGIUseDistanceField();

int32 GetVolumeResolution(int32 ResolutionLevel);

FMatrix CalcCardCaptureViewRotationMatrix(ECubeFace Face);

FMatrix CalcCardCaptureViewProjectionMatrix(FVector CardCenter, FVector Size, ECubeFace Face);

int32 Index3DTo1DLinear(const FIntVector& Index3D, FIntVector Size3D);
FIntVector Index1DTo3DLinear(int32 Index1D, FIntVector Size3D);

FVector CeilToInt3(FVector InVec3);
FVector FloorToInt3(FVector InVec3);
FIntVector Int3Div(FIntVector A, FIntVector B);
FIntVector Int3Mul(FIntVector A, FIntVector B);
FIntVector Int3Clamp(FIntVector Val, FIntVector MinVal, FIntVector MaxVal);

struct FPersistentTexture
{
	FRDGTextureRef RDGTexture;							// register in every frame
	TRefCountPtr<IPooledRenderTarget> PooledTexture;	// create once
};

bool Create2DTexFn(FRDGBuilder& GraphBuilder, FPersistentTexture& ExternalTex, FIntPoint Resolution, EPixelFormat PixelFormat, const TCHAR* DebugName, bool HasMip = false);
bool Create3DTexFn(FRDGBuilder& GraphBuilder, FPersistentTexture& ExternalTex, FIntVector Resolution, EPixelFormat PixelFormat, const TCHAR* DebugName);

template<typename ElementType>
void InitTexture3D(FRDGBuilder& GraphBuilder, FPersistentTexture& ExternalTex, FIntVector Size, ElementType InitValue)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;

	TArray<ElementType> InitData;
	InitData.SetNum(Size.X * Size.Y * Size.Z);
	for (int32 i = 0; i < InitData.Num(); i++)
	{
		InitData[i] = InitValue;
	}

	FUpdateTextureRegion3D UpdateRegion(0, 0, 0, 0, 0, 0, Size.X, Size.Y, Size.Z);
	RHICmdList.UpdateTexture3D(
		(FTexture3DRHIRef&)ExternalTex.PooledTexture->GetRenderTargetItem().ShaderResourceTexture,
		0, UpdateRegion,
		Size.X * sizeof(ElementType),
		Size.X * Size.Y * sizeof(ElementType),
		(const uint8*)InitData.GetData()
	);
}

void ClearCounterBuffer(FRDGBuilder& GraphBuilder, FScene* Scene, FRDGBufferRef Buffer, int32 NumElements);

void SimpleBlit(
	FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
	FRDGTextureRef InputTexture, FRDGTextureRef OutputTexture,
	int32 InputMip = 0, int32 OutputMip = 0
);

class IPerViewObject
{
public:
	~IPerViewObject() {};
	IPerViewObject() {};
	IPerViewObject(uint32 InFrameNumber) :
		FrameNumberRenderThread(InFrameNumber)
	{

	}

	uint32 FrameNumberRenderThread;
};

// this allow us to keep the per view c++ object cross multiple frame
// if store c++ object in FViewInfo, it will be flush very frequently
// so we store it in ObjectViewMap in FScene (or using global var)
// see detail at FHierarchicalStaticMeshSceneProxy::AcceptOcclusionResults
template<typename FPerViewObject>
FPerViewObject* GetOrCreatePerViewObject(TMap<uint32, FPerViewObject>& ObjectViewMap, int32 ViewId)
{
	FPerViewObject* Result = ObjectViewMap.Find(ViewId);
	if (Result)
	{
		Result->FrameNumberRenderThread = GFrameNumberRenderThread;
	}
	else
	{
		// now is a good time to clean up any stale entries
		for (auto Iter = ObjectViewMap.CreateIterator(); Iter; ++Iter)
		{
			if (Iter.Value().FrameNumberRenderThread != GFrameNumberRenderThread)
			{
				Iter.RemoveCurrent();
			}
		}
		ObjectViewMap.Add(ViewId, FPerViewObject(GFrameNumberRenderThread));
		Result = &ObjectViewMap[ViewId];
	}

	return Result;
}

