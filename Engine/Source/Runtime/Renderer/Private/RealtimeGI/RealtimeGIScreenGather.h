#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "RealtimeGIShared.h"

enum ETraceMode
{
	TM_Diffuse = 0,
	TM_Specular,
	TM_Num
};

enum EReservoirSource
{
	RS_Temporal = 0,
	RS_Spatial,
	RS_Num
};

class FRealtimeGIScreenGatherPipeline : public IPerViewObject
{
public:
	~FRealtimeGIScreenGatherPipeline() {};
	FRealtimeGIScreenGatherPipeline() {};
	FRealtimeGIScreenGatherPipeline(uint32 InFrameNumber)
		: IPerViewObject(InFrameNumber)
	{

	};

	void Setup(TRDGUniformBufferRef<FSceneTextureUniformParameters> InSceneTexturesUniformBuffer);
	void Update(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void DiffuseComposite(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture);
	void SpecularComposite(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture);

	void VisualizeRealtimeGIScreenGather(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture);
	void RealtimeGICacheSceneColor(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, FRDGTextureRef SceneColorTexture);

protected:
	void PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void NormalDepthDownsample(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void InitialSampleScreenTrace(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, ETraceMode TraceMode);
	void InitialSampleVoxelTrace(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, ETraceMode TraceMode);
	void ReservoirTemporalReuse(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void ReservoirSpatialReuse(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, EReservoirSource ReservoirSource);
	void ReservoirEvaluateIrradiance(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View, EReservoirSource ReservoirSource);
	void RenderFilterGuidanceSSAO(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void DiffuseTemporalFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void DiffuseSpatialFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void SpecularResolve(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void SpecularTemporalFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);
	void SpecularSpatialFilter(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View);

	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer;

	FIntPoint SceneTextureRTSize;	// input size
	FIntPoint ScreenGatherRTSize;	// output size
	FRDGTextureRef NormalDepthTexture;
	FRDGTextureRef MiniDepthTexture;
	FPersistentTexture NormalDepthHistory;
	FPersistentTexture SceneColorHistory;

	FRDGTextureRef InitialSampleRadiance;
	FRDGTextureRef InitialSampleHitInfo;

	// ping pong buffer between two frame
	FPersistentTexture TemporalReservoirDataA[2];
	FPersistentTexture TemporalReservoirDataB[2];
	FPersistentTexture TemporalReservoirDataC[2];
	FPersistentTexture TemporalReservoirDataD[2];

	FRDGTextureRef SpatialReservoirDataA[2];
	FRDGTextureRef SpatialReservoirDataB[2];
	FRDGTextureRef SpatialReservoirDataC[2];
	FRDGTextureRef SpatialReservoirDataD[2];

	FRDGBufferRef VoxelTraceRayCounter;
	FRDGBufferRef VoxelTraceRayCompactBuffer;
	FRDGBufferRef VoxelTraceIndirectArgs;

	// for diffuse
	FRDGTextureRef TemporalReservoirIrradiance;
	FRDGTextureRef SpatialReservoirIrradiance;
	FRDGTextureRef DiffuseResolveOutputTexture;	// a pointer to TemporalReservoirIrradiance or SpatialReservoirIrradiance
	FRDGTextureRef DiffuseTemporalFilterOutput;
	FRDGTextureRef DiffuseSpatialFilterOutput;
	FPersistentTexture DiffuseIndirectHistory;

	FRDGTextureRef IndirectShadowTexture;
	FRDGTextureRef IndirectShadowTemporalFilterOutput;
	FRDGTextureRef IndirectShadowSpatialFilterOutput;
	FPersistentTexture IndirectShadowHistory;

	// for specular
	FRDGTextureRef SpecularResolveOutputTexture;
	FRDGTextureRef SpecularTemporalFilterOutput;
	FRDGTextureRef SpecularSpatialFilterOutput;
	FPersistentTexture SpecularIndirectHistory;
};

extern void RealtimeGIScreenGather(
	FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer
);

extern void RealtimeGIDiffuseComposite(
	FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture
);

extern void RealtimeGISpecularComposite(
	FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture
);

extern void RealtimeGICacheSceneColor(
	FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views, 
	FRDGTextureRef SceneColorTexture
);

extern void VisualizeRealtimeGIScreenGather(
	FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
	FRDGTextureRef SceneColorTexture, FRDGTextureRef SceneDepthTexture
);

