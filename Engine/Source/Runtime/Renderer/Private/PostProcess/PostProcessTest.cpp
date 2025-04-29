// Copyright Epic Games, Inc. All Rights Reserved.

#include "PostProcess/PostProcessTest.h"
#include "PixelShaderUtils.h"

namespace
{

TAutoConsoleVariable<int32> CVarPostProcessTestQuality(
	TEXT("r.PostProcessTest.Quality"),
	1,
	TEXT("Defines the quality in which the PostProcessTest passes. we might add more quality levels later.\n")
	TEXT(" 0: low quality\n")
	TEXT(">0: high quality (default: 1)\n"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

//BEGIN_SHADER_PARAMETER_STRUCT(FPostProcessTestParameters, )
//	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
//	SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Input)
//	SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Output)
//	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
//	SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
//END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FPostProcessTestParameters, )
SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, Input)
//SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, Output)
//SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
//SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

//FPostProcessTestParameters GetPostProcessTestParameters(const FViewInfo& View, FScreenPassTexture Output, FScreenPassTexture Input, /EPostProcessTestQuality /PostProcessTestMethod)
//{
//	check(Output.IsValid());
//	check(Input.IsValid());
//
//	const FScreenPassTextureViewportParameters InputParameters = GetScreenPassTextureViewportParameters(FScreenPassTextureViewport(Input));
//	const FScreenPassTextureViewportParameters OutputParameters = GetScreenPassTextureViewportParameters(FScreenPassTextureViewport(Output));
//
//	FPostProcessTestParameters Parameters;
//	Parameters.ViewUniformBuffer = View.ViewUniformBuffer;
//	Parameters.Input = InputParameters;
//	Parameters.Output = OutputParameters;
//	Parameters.InputTexture = Input.Texture;
//	Parameters.InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
//	return Parameters;
//}

class FPostProcessTestQualityDimension : SHADER_PERMUTATION_ENUM_CLASS("PostProcessTest_QUALITY", EPostProcessTestQuality);
using FPostProcessTestPermutationDomain = TShaderPermutationDomain<FPostProcessTestQualityDimension>;

class FPostProcessTestVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPostProcessTestVS);
	// FDrawRectangleParameters is filled by DrawScreenPass.
	SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FPostProcessTestVS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	using FParameters = FPostProcessTestParameters;
};

IMPLEMENT_GLOBAL_SHADER(FPostProcessTestVS, "/Engine/Private/PostProcessTest.usf", "PostProcessTestVS", SF_Vertex);

//class FPostProcessTestPS : public FGlobalShader
//{
//public:
//	DECLARE_GLOBAL_SHADER(FPostProcessTestPS);
//	SHADER_USE_PARAMETER_STRUCT(FPostProcessTestPS, FGlobalShader);
//
//	using FPermutationDomain = FPostProcessTestPermutationDomain;
//
//	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
//		SHADER_PARAMETER_STRUCT_INCLUDE(FPostProcessTestParameters, Common)
//		RENDER_TARGET_BINDING_SLOTS()
//	END_SHADER_PARAMETER_STRUCT()
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
//	{
//		return true;
//	}
//};
//
//IMPLEMENT_GLOBAL_SHADER(FPostProcessTestPS, "/Engine/Private/PostProcessTest.usf", "PostProcessTestPS", SF_Pixel);

class FPostProcessTestPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPostProcessTestPS);
	SHADER_USE_PARAMETER_STRUCT(FPostProcessTestPS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	//using FPermutationDomain = FPostProcessTestPermutationDomain;
	using FParameters = FPostProcessTestParameters;
};

IMPLEMENT_GLOBAL_SHADER(FPostProcessTestPS, "/Engine/Private/PostProcessTest.usf", "PostProcessTestPS", SF_Pixel);

} //! namespace

EPostProcessTestQuality GetPostProcessTestQuality()
{
	const int32 postProcessTest = FMath::Clamp(CVarPostProcessTestQuality.GetValueOnRenderThread(), 0, 1);

	return static_cast<EPostProcessTestQuality>(postProcessTest);
}

FScreenPassTexture AddPostProcessTestPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const PostProcessTestInput& Inputs)
{
	//check(Inputs.inputTexture.IsValid());
	//
	//bool bIsComputePass = View.bUseComputePasses;
	//
	//FScreenPassRenderTarget Output;
	//
	//// Construct the output texture to be half resolution (rounded up to even) with an optional format override.
	//{
	//	FRDGTextureDesc Desc = Inputs.inputTexture.Texture->Desc;
	//	Desc.Reset();
	//	Desc.Extent = FIntPoint::DivideAndRoundUp(Desc.Extent, 2);
	//	Desc.Extent.X = FMath::Max(1, Desc.Extent.X);
	//	Desc.Extent.Y = FMath::Max(1, Desc.Extent.Y);
	//	Desc.Flags &= ~(TexCreate_RenderTargetable | TexCreate_UAV);
	//	Desc.Flags |= bIsComputePass ? TexCreate_UAV : TexCreate_RenderTargetable;
	//	Desc.ClearValue = FClearValueBinding(FLinearColor(0, 0, 0, 0));
	//
	//	Output.Texture = GraphBuilder.CreateTexture(Desc, TEXT("PostProcessTestTargetTexture"));
	//	Output.ViewRect = FIntRect::DivideAndRoundUp(Inputs.inputTexture.ViewRect, 2);
	//	Output.LoadAction = ERenderTargetLoadAction::ENoAction;
	//}
	//
	//FPostProcessTestPermutationDomain PermutationVector;
	//PermutationVector.Set<FPostProcessTestQualityDimension>(EPostProcessTestQuality::High);
	//
	//const FScreenPassTextureViewport SceneColorViewport(Inputs.inputTexture);
	//const FScreenPassTextureViewport OutputViewport(Output);
	//
	//{*/
	//	FPostProcessTestPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPostProcessTestPS::FParameters>();
	//	PassParameters->Common = GetPostProcessTestParameters(View, Output, Inputs.inputTexture, EPostProcessTestQuality::High);
	//	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();
	//
	//	TShaderMapRef<FPostProcessTestPS> PixelShader(View.ShaderMap, PermutationVector);
	//
	//	FPixelShaderUtils::AddFullscreenPass(
	//		GraphBuilder,
	//		View.ShaderMap,
	//		RDG_EVENT_NAME("PostProcessTest %dx%d (PS)", Inputs.inputTexture.ViewRect.Width(), Inputs.inputTexture.ViewRect.Height()),
	//		PixelShader,
	//		PassParameters,
	//		OutputViewport.Rect);
	////}
	//
	//return MoveTemp(Output);

	check(Inputs.inputTexture.IsValid());

	FScreenPassRenderTarget Output = Inputs.OverrideOutput;

	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, Inputs.inputTexture, View.GetOverwriteLoadAction(), TEXT("PostProcessTest"));
	}

	const FVector2D OutputExtentInverse = FVector2D(1.0f / (float)Output.Texture->Desc.Extent.X, 1.0f / (float)Output.Texture->Desc.Extent.Y);

	FRHISamplerState* BilinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	FPostProcessTestParameters* PassParameters = GraphBuilder.AllocParameters<FPostProcessTestParameters>();
	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();
	PassParameters->Input = GetScreenPassTextureInput(Inputs.inputTexture, BilinearClampSampler);

	//FPostProcessTestPS::FPermutationDomain PixelPermutationVector;
	//PixelPermutationVector.Set<FPostProcessTestPS::FQualityDimension>(Inputs.Quality);

	//FPostProcessTestPermutationDomain PixelPermutationVector;
	//PixelPermutationVector.Set<FPostProcessTestQualityDimension>(EPostProcessTestQuality::High);

	TShaderMapRef<FPostProcessTestVS> VertexShader(View.ShaderMap);
	//TShaderMapRef<FPostProcessTestPS> PixelShader(View.ShaderMap, PixelPermutationVector);
	TShaderMapRef<FPostProcessTestPS> PixelShader(View.ShaderMap);

	const FScreenPassTextureViewport OutputViewport(Output);

	AddDrawScreenPass(
		GraphBuilder,
		RDG_EVENT_NAME("PostProcessTest %dx%d (PS)", OutputViewport.Rect.Width(), OutputViewport.Rect.Height()),
		View,
		OutputViewport,
		FScreenPassTextureViewport(Inputs.inputTexture),
		FScreenPassPipelineState(VertexShader, PixelShader),
		PassParameters,
		EScreenPassDrawFlags::AllowHMDHiddenAreaMask,
		[VertexShader, PixelShader, PassParameters](FRHICommandList& RHICmdList)
		{
			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *PassParameters);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);
		});

	return MoveTemp(Output);
}
