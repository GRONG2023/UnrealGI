#include "RealtimeGICardCapture.h"

#include "CoreMinimal.h"
#include "RHI.h"
#include "SceneUtils.h"
#include "ScenePrivate.h"
#include "UnifiedBuffer.h"
#include "SpriteIndexBuffer.h"
#include "SceneFilterRendering.h"
#include "ClearQuad.h"
#include "RendererModule.h"
#include "RenderResource.h"
#include "ShaderParameters.h"
#include "RendererInterface.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "RHIStaticStates.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "SceneRenderTargetParameters.h"
#include "DeferredShadingRenderer.h"
#include "ReflectionEnvironmentCapture.h"
#include "MeshPassProcessor.inl"


IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FRealtimeGICardCaptureParams, "CardCaptureParams");
IMPLEMENT_MATERIAL_SHADER_TYPE(, FRealtimeGICardCaptureVS, TEXT("/Engine/Private/RealtimeGI/RealtimeGICardCapture.usf"), TEXT("RealtimeGICardCaptureVS"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(, FRealtimeGICardCapturePS, TEXT("/Engine/Private/RealtimeGI/RealtimeGICardCapture.usf"), TEXT("RealtimeGICardCapturePS"), SF_Pixel);

IMPLEMENT_GLOBAL_SHADER(FRealtimeGICardClearVS, "/Engine/Private/RealtimeGI/RealtimeGICardClear.usf", "MainVertexShader", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FRealtimeGICardClearPS, "/Engine/Private/RealtimeGI/RealtimeGICardClear.usf", "MainPixelShader", SF_Pixel);

void RenderCardClearQuads(FRHICommandList& RHICmdList, FRealtimeGICardCapturePassParameters* PassParameters, int32 NumCardClearQuads)
{
	if (NumCardClearQuads == 0)
	{
		return;
	}

	auto* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FRealtimeGICardClearVS> VertexShader(ShaderMap);
	TShaderMapRef<FRealtimeGICardClearPS> PixelShader(ShaderMap);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.BlendState = TStaticBlendStateWriteMask<CW_RGBA, CW_RGBA, CW_RGBA, CW_RGBA>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None, true>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->ClearStruct);

	RHICmdList.SetStreamSource(0, GClearVertexBuffer.VertexBufferRHI, 0);
	RHICmdList.DrawPrimitive(0, 2, NumCardClearQuads);
}

FRealtimeGICardCaptureMeshProcessor::FRealtimeGICardCaptureMeshProcessor(
	const FScene* Scene,
	const FSceneView* InView,
	FMeshPassDrawListContext* InDrawListContext,
	FRHIUniformBuffer* InPassUniformBuffer)
	: 
	FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InView, InDrawListContext),
	PassDrawRenderState(*InView, InPassUniformBuffer)
{
	PassDrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());
}

void FRealtimeGICardCaptureMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
	const FMaterial& Material = MeshBatch.MaterialRenderProxy->GetMaterialWithFallback(FeatureLevel, FallbackMaterialRenderProxyPtr);

	const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
	const ERasterizerFillMode MeshFillMode = FM_Solid;	// ComputeMeshFillMode(MeshBatch, Material, OverrideSettings);
	const ERasterizerCullMode MeshCullMode = CM_None;	// ComputeMeshCullMode(MeshBatch, Material, OverrideSettings);
	const FMaterialRenderProxy& MaterialRenderProxy = FallbackMaterialRenderProxyPtr ? *FallbackMaterialRenderProxyPtr : *MeshBatch.MaterialRenderProxy;
	
	if (!MeshBatch.bUseForMaterial || 
		Material.IsSky() ||
		IsTranslucentBlendMode(Material.GetBlendMode()))
	{
		return;
	}

	Process(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material, MeshFillMode, MeshCullMode);
}

bool GetRealtimeGICardCaptureShaders(
	const FMaterial& Material,
	const FVertexFactory* VertexFactory,
	ERHIFeatureLevel::Type FeatureLevel,
	TShaderRef<FRealtimeGICardCaptureVS>& VertexShader,
	TShaderRef<FRealtimeGICardCapturePS>& PixelShader)
{
	const FVertexFactoryType* VFType = VertexFactory->GetType();

	FMaterialShaderTypes ShaderTypes;
	ShaderTypes.AddShaderType<FRealtimeGICardCaptureVS>();
	ShaderTypes.AddShaderType<FRealtimeGICardCapturePS>();

	FMaterialShaders Shaders;
	if (!Material.TryGetShaders(ShaderTypes, VFType, Shaders))
	{
		return false;
	}
	
	Shaders.TryGetVertexShader(VertexShader);
	Shaders.TryGetPixelShader(PixelShader);

	return true;
}

void FRealtimeGICardCaptureMeshProcessor::Process(
	const FMeshBatch& MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
	const FMaterial& RESTRICT MaterialResource,
	ERasterizerFillMode MeshFillMode,
	ERasterizerCullMode MeshCullMode)
{
	const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;
	const FVertexFactoryType* VFType = VertexFactory->GetType();

	TMeshProcessorShaders<
		FRealtimeGICardCaptureVS,
		FMeshMaterialShader,
		FMeshMaterialShader,
		FRealtimeGICardCapturePS> PassShaders;

	/*
	if (!GetRealtimeGICardCaptureShaders(
		MaterialResource,
		VertexFactory,
		FeatureLevel,
		PassShaders.VertexShader,
		PassShaders.PixelShader
	))
	{
		return;
	}
	*/

	PassShaders.VertexShader = MaterialResource.GetShader<FRealtimeGICardCaptureVS>(VertexFactory->GetType());
	PassShaders.PixelShader = MaterialResource.GetShader<FRealtimeGICardCapturePS>(VertexFactory->GetType());

	const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(PassShaders.VertexShader, PassShaders.PixelShader);

	FMeshMaterialShaderElementData ShaderElementData;
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

	BuildMeshDrawCommands(
		MeshBatch,
		BatchElementMask,
		PrimitiveSceneProxy,
		MaterialRenderProxy,
		MaterialResource,
		PassDrawRenderState,
		PassShaders,
		MeshFillMode,
		MeshCullMode,
		SortKey,
		EMeshPassFeatures::Default,
		ShaderElementData
	);
}

void FRealtimeGIGPUScene::RealtimeGISurfaceCacheCapturePass(FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View)
{
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (SurfaceCacheCaptureCommands.Num() == 0 && SurfaceCacheClearCommands.Num() == 0)
	{
		return;
	}
#endif

	const int32 RTResolution = SurfaceCacheAtlasResolution;
	TArray<FSurfaceCacheInfo*> SurfaceCacheInfosRef;
	TArray<TUniformBufferRef<FRealtimeGICardCaptureParams>> PerDrawCallUniformBuffers;

	// create uniform buffers for each single drawcall
	for (const int32& SurfaceCacheId : SurfaceCacheCaptureCommands)
	{
		const FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[SurfaceCacheId];
		SurfaceCacheInfosRef.Add(&SurfaceCacheInfos[SurfaceCacheId]);

		auto* PassStruct = GraphBuilder.AllocParameters<FRealtimeGICardCaptureParams>();

		for (int32 CardIndex = 0; CardIndex < SurfaceCacheInfo.NumMeshCards; CardIndex++)
		{
			PassStruct->ViewProjectionMatrixs[CardIndex] = SurfaceCacheInfo.LocalToCardMatrixs[CardIndex];
			PassStruct->ViewportInfos[CardIndex] = CalcViewportInfo(SurfaceCacheInfo, CardIndex);
		}

		TUniformBufferRef<FRealtimeGICardCaptureParams> PassUniformBuffer = TUniformBufferRef<FRealtimeGICardCaptureParams>::CreateUniformBufferImmediate(*PassStruct, UniformBuffer_SingleFrame);
		PerDrawCallUniformBuffers.Add(PassUniformBuffer);
	}

	const int32 NumObjects = PerDrawCallUniformBuffers.Num();
	ERenderTargetLoadAction LoadAction = SurfaceCacheAtlasNeedClear ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;

	// render target parameters
	auto* PassParameters = GraphBuilder.AllocParameters<FRealtimeGICardCapturePassParameters>();
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SurfaceCacheAtlas[RT_BaseColor].RDGTexture, LoadAction);
	PassParameters->RenderTargets[1] = FRenderTargetBinding(SurfaceCacheAtlas[RT_Normal].RDGTexture, LoadAction);
	PassParameters->RenderTargets[2] = FRenderTargetBinding(SurfaceCacheAtlas[RT_Emissive].RDGTexture, LoadAction);
	PassParameters->RenderTargets[3] = FRenderTargetBinding(SurfaceCacheAtlas[RT_Depth].RDGTexture, LoadAction);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(CardCaptureDepthStencil, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);

	// clear quad parameters
	PassParameters->ClearStruct.CardAtlasResolution = SurfaceCacheAtlasResolution;
	PassParameters->ClearStruct.CardClearQuadUVTransformBuffer = CardClearQuadUVTransformBuffer.SRV;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("RealtimeGIMeshCardCapture"),
		PassParameters,
		ERDGPassFlags::Raster,
		[&View, SurfaceCacheInfosRef, PerDrawCallUniformBuffers, NumObjects, RTResolution, NumQuads=SurfaceCacheClearCommands.Num(), PassParameters](FRHICommandList& RHICmdList)
	{
		RHICmdList.SetViewport(0, 0, 0.0f, RTResolution, RTResolution, 1.0f);

		RenderCardClearQuads(RHICmdList, PassParameters, NumQuads);

		DrawDynamicMeshPass(View, RHICmdList,
			[&View, SurfaceCacheInfosRef, PerDrawCallUniformBuffers, NumObjects](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
		{
			for (int32 i = 0; i < NumObjects; i++)
			{
				FSurfaceCacheInfo* SurfaceCacheInfo = SurfaceCacheInfosRef[i];
				TUniformBufferRef<FRealtimeGICardCaptureParams> UniformBuffer = PerDrawCallUniformBuffers[i];

				FRealtimeGICardCaptureMeshProcessor PassMeshProcessor(
					View.Family->Scene->GetRenderScene(),
					&View,
					DynamicMeshPassContext,
					UniformBuffer
				);

				const TArray<FMeshBatch>& MeshElements = SurfaceCacheInfo->CardCaptureMeshBatches;
				for (int32 MeshId = 0; MeshId < MeshElements.Num(); MeshId++)
				{
					const FMeshBatch* Mesh = &(MeshElements[MeshId]);
					const FPrimitiveSceneProxy* PrimitiveSceneProxy = SurfaceCacheInfo->Primitive->Proxy;
					const uint64 DefaultBatchElementMask = ~0ull;

					PassMeshProcessor.AddMeshBatch(*Mesh, DefaultBatchElementMask, PrimitiveSceneProxy);
				}
			}

		}, true);

	});
}



