#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "RealtimeGIShared.h"
#include "RealtimeGIScreenGather.h"

enum EProbeVisualizeMode
{
	VM_RadianceProbe = 0,
	VM_IrradianceProbe,
	VM_Num
};

class FRealtimeGIRadianceCache : public IPerViewObject
{
public:
	~FRealtimeGIRadianceCache() {};
	FRealtimeGIRadianceCache() {};
	FRealtimeGIRadianceCache(uint32 InFrameNumber)
		: IPerViewObject(InFrameNumber)
	{

	};

	void Update(FRDGBuilder& GraphBuilder, FSceneRenderer& SceneRenderer, FScene* Scene, FViewInfo& View);

	void VisualizeProbe(
		FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
		FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture, EProbeVisualizeMode VisualizeMode
	);

	FProbeVolumeParameters SetupProbeVolumeParameters(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId = 0);

protected:
	void PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void PickValidVoxel(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void VoxelLighting(FRDGBuilder& GraphBuilder, FSceneRenderer& SceneRenderer, FScene* Scene, FViewInfo& View, int32 ClipId);
	void PickValidProbe(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void RadianceProbeCapture(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void RadianceToIrradiance(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void IrradianceProbeGather(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);
	void CleanupDirtyUpdateChunk(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, int32 ClipId);

	int32 VoxelLightingCheckerBoardSize = 2;
	FRDGBufferRef ValidVoxelCounter;
	FRDGBufferRef ValidVoxelBuffer;
	FRDGBufferRef VoxelLightingIndirectArgs;
	FPersistentTexture VoxelPoolRadiance;

	// radiance and irradiance probe share same probe placement
	int32 ProbeUpdateCheckerBoardSize = 2;
	FRDGBufferRef ValidProbeCounter;
	FRDGBufferRef ValidProbeBuffer;
	FPersistentTexture ProbeOffsetClipmap;

	// gather irradiance for all clip
	FRDGBufferRef IrradianceProbeGatherIndirectArgs;
	FPersistentTexture IrradianceProbeClipmap;
	
	// just capture radiance probe for highest clip
	int32 RadianceProbeResolution = 16;
	FIntPoint NumRadianceProbesInAtlasXY = FIntPoint(128, 128);
	FPersistentTexture RadianceProbeAtlas;
	FPersistentTexture RadianceProbeDistanceAtlas;
	FPersistentTexture RadianceProbeOutput;
	FPersistentTexture RadianceProbeDistanceOutput;

	// radiance probe need large resolution to store radiance in atlas, so we sparse allocate it
	// last element in list is pointer to next read write position
	FRWBufferStructured RadianceProbeFreeList;		// empty probe id list
	FRWBufferStructured RadianceProbeReleaseList;	// pending probes to release at this frame
	FPersistentTexture RadianceProbeIdClipmap;		// store index to RadianceProbeAtlas
	FRDGBufferRef RadianceProbeReleaseIndirectArgs;
	FRDGBufferRef RadianceProbeCaptureIndirectArgs;
	FRDGBufferRef RadianceProbeOutputMergeIndirectArgs;

	friend class FRealtimeGIVoxelClipmap;
};

extern void RealtimeGIVoxelLighting(FRDGBuilder& GraphBuilder, FSceneRenderer& SceneRenderer, FScene* Scene, TArray<FViewInfo>& Views);
