#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "MeshPassProcessor.h"
#include "SceneRendering.h"
#include "RealtimeGIShared.h"


BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FRealtimeGICardCaptureParams, )
	SHADER_PARAMETER_ARRAY(FMatrix, ViewProjectionMatrixs, [MAX_CARDS_PER_MESH])
	SHADER_PARAMETER_ARRAY(FVector4, ViewportInfos, [MAX_CARDS_PER_MESH])
END_GLOBAL_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FCardClearParameters, )
	SHADER_PARAMETER(float, CardAtlasResolution)
	SHADER_PARAMETER_SRV(StructuredBuffer, CardClearQuadUVTransformBuffer)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FRealtimeGICardCapturePassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FCardClearParameters, ClearStruct)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FRealtimeGICardCaptureVS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FRealtimeGICardCaptureVS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return Parameters.VertexFactoryType == FindVertexFactoryType(FName(TEXT("FLocalVertexFactory")))
			|| Parameters.VertexFactoryType == FindVertexFactoryType(FName(TEXT("FSplineMeshVertexFactory")));
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
	
	FRealtimeGICardCaptureVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FMeshMaterialShader(Initializer)
	{
		PassUniformBuffer.Bind(Initializer.ParameterMap, FRealtimeGICardCaptureParams::StaticStructMetadata.GetShaderVariableName());
	}
	
	FRealtimeGICardCaptureVS()
	{

	}
};

class FRealtimeGICardCapturePS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FRealtimeGICardCapturePS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return FRealtimeGICardCaptureVS::ShouldCompilePermutation(Parameters);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	FRealtimeGICardCapturePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FMeshMaterialShader(Initializer)
	{
		PassUniformBuffer.Bind(Initializer.ParameterMap, FRealtimeGICardCaptureParams::StaticStructMetadata.GetShaderVariableName());
	}

	FRealtimeGICardCapturePS()
	{

	}
};

class FRealtimeGICardCaptureMeshProcessor : public FMeshPassProcessor
{
public:

	FRealtimeGICardCaptureMeshProcessor(
		const FScene* Scene,
		const FSceneView* InViewIfDynamicMeshCommand,
		FMeshPassDrawListContext* InDrawListContext, 
		FRHIUniformBuffer* InPassUniformBuffer);

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final;

	FMeshPassProcessorRenderState PassDrawRenderState;

private:
	void Process(
		const FMeshBatch& MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		int32 StaticMeshId,
		const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
		const FMaterial& RESTRICT MaterialResource,
		ERasterizerFillMode MeshFillMode,
		ERasterizerCullMode MeshCullMode);
};

class FRealtimeGICardClearVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRealtimeGICardClearVS);
	SHADER_USE_PARAMETER_STRUCT(FRealtimeGICardClearVS, FGlobalShader);

	using FParameters = FCardClearParameters;

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

class FRealtimeGICardClearPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FRealtimeGICardClearPS);
	SHADER_USE_PARAMETER_STRUCT(FRealtimeGICardClearPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
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
