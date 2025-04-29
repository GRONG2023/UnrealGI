#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "MeshPassProcessor.h"
#include <deque>
#include "RealtimeGIShared.h"

struct FUpdateChunk
{
	int32 Index1D = 0;
	uint32 TimeStamp = 0;
};

class FRealtimeGIVolumeInfo
{
public:
	FRealtimeGIVolumeInfo() {};
	~FRealtimeGIVolumeInfo() {};

	bool HasChunksToUpdate();
	bool PopUpdateChunk(FUpdateChunk& OutElement);
	void PushUpdateChunk(const FUpdateChunk& InElement);

	void PopulateUpdateChunkList();
	void PopulateUpdateChunkCleanupList(uint32 FrameIndex);

	FVector Center;
	FVector CoverRange = { 3200.0f, 3200.0f, 3200.0f };
	FIntVector Scrolling;
	FIntVector NumChunksInXYZ;
	FIntVector DeltaChunk;	// if last frame in chunk (3, 4, 6), cur frame in (1, 3, 9), delta chunk = (-2, -1, 3)
	FIntVector UpdateChunkResolution = { 16, 16, 16 };

	TArray<int32> ChunksToUpdate;	// chunks to update in current frame, chunk may dirty in several frames ago
	TArray<int32> ChunksToCleanup;	// chunks to cleanup, when a chunk dirty it will be clean at cur frame

protected:
	std::deque<FUpdateChunk> PendingUpdateChunks;
	TSet<int32> UpdateChunksLookUp;
};

// per View's data
class FRealtimeGIVoxelClipmap : public IPerViewObject
{
public:
	~FRealtimeGIVoxelClipmap() {};
	FRealtimeGIVoxelClipmap() {};
	FRealtimeGIVoxelClipmap(uint32 InFrameNumber)
		: IPerViewObject(InFrameNumber)
	{

	};

	void Update(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);

	void VisualizeVoxel(
		FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
		FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture
	);

	FVoxelRaytracingParameters SetupVoxelRaytracingParameters(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId = 0);

protected:
	void UpdateVolumePosition(FViewInfo& View, int32 ClipId);
	void MarkDirtyChunksToUpdate(FScene* Scene, FViewInfo& View, int32 ClipId);
	void PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void UploadChunkIds(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void CullObjectToClipmap(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void CullObjectToUpdateChunk(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void VoxelInject(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void DistanceFieldPropagate(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void ReleaseVoxelPage(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);

	FPersistentTexture& GetDistanceFieldClipmap() { return DistanceFieldClipmap[(FrameNumberRenderThread + 0) % 2]; };
	FPersistentTexture& GetDistanceFieldClipmapNextFrame() { return DistanceFieldClipmap[(FrameNumberRenderThread + 1) % 2]; };

	int32 NumClips = 0;
	FIntVector VolumeResolution = { 128, 128, 128 };

	TArray<FRealtimeGIVolumeInfo> ClipmapInfos;

	FByteAddressBuffer UpdateChunkList[MAX_CLIP_NUM];			// see FRealtimeGIVolumeInfo.ChunksToUpdate
	FByteAddressBuffer UpdateChunkCleanupList[MAX_CLIP_NUM];	// see FRealtimeGIVolumeInfo.ChunksToCleanup

	FRDGBufferRef ClipmapObjectCounter;
	FRDGBufferRef ClipmapCullingResult;
	FRDGBufferRef UpdateChunkCullingIndirectArgs;

	// 2D array as 1D, single row represent an update chunk, example:
	// Chunk 114 intersect objects: [ 3,  5,  7,  8,  9,  _,  _,  _]
	// Chunk 514 intersect objects: [ 1,  2,  4, 11, 14,  _,  _,  _]
	// Chunk 191 intersect objects: [19, 12, _ ,  _,  _,  _,  _,  _]
	FRDGBufferRef UpdateChunkCullingResult;
	FRDGBufferRef UpdateChunkObjectCounter;

	FPersistentTexture VoxelBitOccupyClipmap;
	FPersistentTexture VoxelPageClipmap;	// store address in VoxelPagePool

	FPersistentTexture DistanceFieldClipmap[2];	// ping-pang swap texture

	// last element in list is pointer to next read write position
	FRWBufferStructured VoxelPageFreeList;		// empty page id list
	FRWBufferStructured VoxelPageReleaseList;	// pending pages to release at this frame
	FRDGBufferRef VoxelPageReleaseIndirectArgs;

	// sparse store per-voxel material attribute, all clips share same physic texture
	// note: per-mesh material attribute is store in surface cache atlas, like BLAS
	// voxel pool will store per-instance material attribute, like TLAS
	FIntVector NumVoxelPagesInXYZ = FIntVector(32, 32, 32);
	FPersistentTexture VoxelPoolBaseColor;
	FPersistentTexture VoxelPoolNormal;
	FPersistentTexture VoxelPoolEmissive;

	friend class FRealtimeGIRadianceCache;
};

extern void RealtimeGIDebugVisualization(
	FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views, 
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture
);
