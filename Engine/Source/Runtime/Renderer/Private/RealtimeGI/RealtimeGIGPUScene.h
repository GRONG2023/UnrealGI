#pragma once

#include "CoreMinimal.h"
#include "Math/IntVector.h"
#include "RHI.h"
#include "RenderResource.h"
#include "UniformBuffer.h"
#include "ShaderParameters.h"
#include "Shader.h"
#include "RendererInterface.h"
#include "MeshDrawCommands.h"
#include "StupidAllocator.h"
#include "RealtimeGIShared.h"
#include "RealtimeGIVoxelClipmap.h"
#include "RealtimeGIVoxelLighting.h"


class FSurfaceCacheKey
{
public:
	FSurfaceCacheKey() {};
	FSurfaceCacheKey(const FPrimitiveSceneInfo* InPrimitive);

	bool operator == (const FSurfaceCacheKey& Other) const;
	bool operator != (const FSurfaceCacheKey& Other) const;

	friend uint32 GetTypeHash(FSurfaceCacheKey Key)
	{
		return Key.HashValue;
	}

	TArray<void*> VertexFactoryPointers;
	TArray<void*> MaterialPointers;
	uint32 HashValue;
};

class FObjectInfo
{
public:
	FObjectInfo() {};
	FObjectInfo(const FPrimitiveSceneInfo* InPrimitive, int32 InObjectId)
		: ObjectId(InObjectId)
	{
		UpdatePrimitive(InPrimitive);
	}

	void UpdatePrimitive(const FPrimitiveSceneInfo* InPrimitive);

	int32 ObjectId = OBJECT_ID_INVALID;
	FPrimitiveComponentId PrimitiveComponentId;
	const FPrimitiveSceneInfo* Primitive = nullptr;

	// primitive has same vertex factory and material can share surface cache
	int32 SurfaceCacheId = OBJECT_ID_INVALID;
	FSurfaceCacheKey SurfaceCacheKey;
};

class FSurfaceCacheInfo
{
public:
	FSurfaceCacheInfo() {};
	FSurfaceCacheInfo(int32 InSurfaceCacheId);

	void Empty();
	void AddObjectReference(const FObjectInfo& ObjectInfo);
	void RemoveObjectReference(const FObjectInfo& ObjectInfo);
	int32 GetRefCount() { return ReferenceHolders.Num(); };
	TArray<int32> GetReferenceHolders() { return ReferenceHolders.Array(); };

	const FPrimitiveSceneInfo* Primitive = nullptr;
	int32 SurfaceCacheId = 0;
	int32 NumMeshCards = 0;
	int32 MeshCardResolution = 0;
	TArray<FMeshBatch> CardCaptureMeshBatches;
	TArray<FMatrix> LocalToCardMatrixs;
	TArray<FVector4> CardUVTransforms;	// float4(SizeX, SizeY, OffsetX, OffsetY)

protected:
	TSet<int32> ReferenceHolders;	// object id share same surface cache in GI scene
};

class FRealtimeGIGPUScene
{
public:
	FRealtimeGIGPUScene() {};
	~FRealtimeGIGPUScene() {};

	void OnRendererSceneUpdatePrimitives(const TArray<FPrimitiveSceneInfo*>& AddedPrimitiveSceneInfos, const TArray<FPrimitiveSceneInfo*>& RemovedPrimitiveSceneInfos);
	void PreUpdate(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views);
	void Update(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views);
	void PostUpdate(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views);
	int32 GetObjectNum() { return ObjectIdAllocator.GetAllocatedElementNums(); };

	FRealtimeGIVoxelClipmap* GetVoxelClipmap(const uint32& ViewId);

protected:
	void FlushPrimitiveUpdateQueue();
	void PrepareSurfaceCacheCapture();
	void PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene);
	void SyncObjectInfosToGPU(FRDGBuilder& GraphBuilder, FScene* Scene);
	void SyncSurfaceCacheInfosToGPU(FRDGBuilder& GraphBuilder, FScene* Scene);
	void RealtimeGISurfaceCacheCapturePass(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);

	FVector4 CalcViewportInfo(const FSurfaceCacheInfo& SurfaceCacheInfo, const int32& CardIndex);
	void ReferenceSurfaceCache(FObjectInfo& ObjectInfo);
	void DeReferenceSurfaceCache(FObjectInfo& ObjectInfo);
	void AllocateSurfaceCacheTextureSpace(const int32& SurfaceCacheId);
	void ReleaseSurfaceCacheTextureSpace(const int32& SurfaceCacheId);
	void GatherCardCaptureMeshElements(const int32& SurfaceCacheId);
	void AdjustSurfaceCacheResolution(const int32& SurfaceCacheId);

	// key is view id
	TMap<uint32, FRealtimeGIVoxelClipmap> VoxelClipmapViewMap;
	TMap<uint32, FRealtimeGIRadianceCache> RadianceCacheViewMap;
	TMap<uint32, FRealtimeGIScreenGatherPipeline> ScreenGatherPipelineViewMap;

	TMap<FPrimitiveComponentId, FPrimitiveSceneInfo*> PendingPrimitivesToAdd;
	TMap<FPrimitiveComponentId, FBoxSphereBounds> PendingPrimitivesToRemove;

	FStupidLinearAllocator<FPrimitiveComponentId> ObjectIdAllocator;
	FStupidLinearAllocator<FSurfaceCacheKey> SurfaceCacheAllocator;	// allocate mesh card descriptor in cpu
	FStupidQuadTreeAllocator SurfaceCacheAtlasAllocator;			// allocate texture space in gpu

	TArray<FObjectInfo> ObjectInfos;
	TArray<FSurfaceCacheInfo> SurfaceCacheInfos;

	// commands to sync object info to gpu, only store object id
	TSet<int32> ObjectAddCommands;
	TSet<int32> ObjectRemoveCommands;
	TSet<int32> ObjectUpdateCommands;

	// commands to capture or clear surface cache, only store surface cache id
	TSet<int32> SurfaceCacheCaptureCommands;
	TArray<FVector4> SurfaceCacheClearCommands;

	TArray<FBoxSphereBounds> DirtyPrimitiveBounds;

	FRWBufferStructured ObjectInfoBuffer;
	FByteAddressBuffer ObjectInfoUploadBuffer;
	FByteAddressBuffer RemovedObjectIdBuffer;

	// card placed in local space, so the update frequency of cards is lower than ObjectInfo
	// for example: an object change transform, but card's placement will not change
	FRWBufferStructured SurfaceCacheInfoBuffer;
	FByteAddressBuffer SurfaceCacheInfoUploadBuffer;
	FRWBufferStructured CardInfoBuffer;
	FByteAddressBuffer CardInfoUploadBuffer;
	FByteAddressBuffer CardClearQuadUVTransformBuffer;

	// compact buffer for culling
	FRDGBufferRef ObjectInfoCounter;
	FRWBufferStructured MiniObjectInfoBuffer;

	bool SurfaceCacheAtlasNeedClear = false;
	int32 SurfaceCacheAtlasResolution = 2048;
	FPersistentTexture SurfaceCacheAtlas[RT_Num];
	FRDGTextureRef CardCaptureDepthStencil;

	friend class FRealtimeGIVoxelClipmap;
	friend class FRealtimeGIRadianceCache;
};

extern void RealtimeGISceneUpdate(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views);
