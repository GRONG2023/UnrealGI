#include "RealtimeGIShared.h"
#include "RenderCore/Public/RenderTargetPool.h"
#include "RenderCore/Public/RenderGraphBuilder.h"
#include "RenderCore/Public/GlobalShader.h"
#include "RenderCore/Public/ShaderParameterStruct.h"
#include "Renderer/Private/ScenePrivate.h"


extern TAutoConsoleVariable<int32> CVarRealtimeGIEnable(
	TEXT("r.RealtimeGI.Enable"),
	1,
	TEXT("Enable Realtime GI"),
	ECVF_RenderThreadSafe
);

bool RealtimeGIEnable()
{
	return CVarRealtimeGIEnable.GetValueOnRenderThread() > 0;
};

TAutoConsoleVariable<int32> CVarRealtimeGIUseDistanceField(
	TEXT("r.RealtimeGI.UseDistanceField"),
	1,
	TEXT("Use distance field for raytracing"),
	ECVF_RenderThreadSafe
);

bool RealtimeGIUseDistanceField() 
{ 
	return CVarRealtimeGIUseDistanceField.GetValueOnRenderThread() > 0; 
};

int32 GetVolumeResolution(int32 ResolutionLevel)
{
	check(0 <= ResolutionLevel && ResolutionLevel < VR_Num);
	static const int32 VolumeResolutionLevels[VR_Num] = { 64, 96, 128 };
	return VolumeResolutionLevels[ResolutionLevel];
}

FMatrix CalcCardCaptureViewRotationMatrix(ECubeFace Face)
{
	FMatrix Result(FMatrix::Identity);

	static const FVector XAxis(1.f, 0.f, 0.f);
	static const FVector YAxis(0.f, 1.f, 0.f);
	static const FVector ZAxis(0.f, 0.f, 1.f);

	// vectors we will need for our basis
	FVector vUp(ZAxis);
	FVector vDir;

	switch (Face)
	{
	case CubeFace_PosX:
		vDir = XAxis;
		break;
	case CubeFace_NegX:
		vDir = -XAxis;
		break;
	case CubeFace_PosY:
		vDir = YAxis;
		break;
	case CubeFace_NegY:
		vDir = -YAxis;
		break;
	case CubeFace_PosZ:
		vUp = -YAxis;
		vDir = ZAxis;
		break;
	case CubeFace_NegZ:
		vUp = YAxis;
		vDir = -ZAxis;
		break;
	}

	// derive right vector
	FVector vRight(vUp ^ vDir);
	// create matrix from the 3 axes
	Result = FBasisVectorMatrix(vRight, vUp, vDir, FVector::ZeroVector);

	return Result;
}

FMatrix CalcCardCaptureViewProjectionMatrix(FVector CardCenter, FVector Size, ECubeFace Face)
{
	float Width = Size.GetMax();
	float Height = Size.GetMax();
	float Depth = Size.GetMax();

	if (Face == CubeFace_NegX || Face == CubeFace_PosX)
	{
		Width = Size.Y;
		Height = Size.Z;
	}

	if (Face == CubeFace_NegY || Face == CubeFace_PosY)
	{
		Width = Size.X;
		Height = Size.Z;
	}

	if (Face == CubeFace_NegZ || Face == CubeFace_PosZ)
	{
		Width = Size.X;
		Height = Size.Y;
	}

	float NearPlane = Depth * -0.5;
	float FarPlane = Depth * 0.5;
	float ZScale = 1.0f / (FarPlane - NearPlane);
	float ZOffset = -NearPlane;

	FViewMatrices::FMinimalInitializer CaptureViewInitOptions;
	CaptureViewInitOptions.ViewRotationMatrix = CalcCardCaptureViewRotationMatrix(Face);
	CaptureViewInitOptions.ViewOrigin = CardCenter;
	CaptureViewInitOptions.ProjectionMatrix = FReversedZOrthoMatrix(Width * 0.5, Height * 0.5, ZScale, ZOffset);
	// CaptureViewInitOptions.ProjectionMatrix = GetCubeProjectionMatrix(90.0 * 0.5f, 128, 0.1f);	// for debug
	FViewMatrices CaptureViewMatrices = FViewMatrices(CaptureViewInitOptions);

	return CaptureViewMatrices.GetViewProjectionMatrix();
}

int32 Index3DTo1DLinear(const FIntVector& Index3D, FIntVector Size3D)
{
	int32 Res = 0;
	Res += Index3D.X * 1;
	Res += Index3D.Y * Size3D.X;
	Res += Index3D.Z * (Size3D.X * Size3D.Y);
	return Res;
}

FIntVector Index1DTo3DLinear(int32 Index1D, FIntVector Size3D)
{
	FIntVector Res;

	Res.Z = Index1D / (Size3D.X * Size3D.Y);
	Index1D -= Res.Z * (Size3D.X * Size3D.Y);

	Res.Y = Index1D / Size3D.X;
	Index1D -= Res.Y * Size3D.X;

	Res.X = Index1D;

	return Res;
}

FVector CeilToInt3(FVector InVec3)
{
	return FVector(FMath::CeilToInt(InVec3.X), FMath::CeilToInt(InVec3.Y), FMath::CeilToInt(InVec3.Z));
};

FVector FloorToInt3(FVector InVec3)
{
	return FVector(FMath::FloorToInt(InVec3.X), FMath::FloorToInt(InVec3.Y), FMath::FloorToInt(InVec3.Z));
};

FIntVector Int3Div(FIntVector A, FIntVector B)
{
	return FIntVector(A.X / B.X, A.Y / B.Y, A.Z / B.Z);
};

FIntVector Int3Mul(FIntVector A, FIntVector B)
{
	return FIntVector(A.X * B.X, A.Y * B.Y, A.Z * B.Z);
};

FIntVector Int3Clamp(FIntVector Val, FIntVector MinVal, FIntVector MaxVal)
{
	return FIntVector(
		FMath::Clamp(Val.X, MinVal.X, MaxVal.X),
		FMath::Clamp(Val.Y, MinVal.Y, MaxVal.Y),
		FMath::Clamp(Val.Z, MinVal.Z, MaxVal.Z)
	);
}

bool Create2DTexFn(FRDGBuilder& GraphBuilder, FPersistentTexture& ExternalTex, FIntPoint Resolution, EPixelFormat PixelFormat, const TCHAR* DebugName, bool HasMip)
{
	bool IsFirstTimeCreate = false;
	TRefCountPtr<IPooledRenderTarget>& PooledTex = ExternalTex.PooledTexture;

	int32 NumMips = HasMip ? FMath::Log2(float(FMath::Max(Resolution.X, Resolution.Y))) + 1 : 1;

	FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::Create2DDesc(
		Resolution, PixelFormat, FClearValueBinding::Transparent,
		TexCreate_ShaderResource, TexCreate_UAV | TexCreate_RenderTargetable, false, NumMips
	);

	if (!PooledTex.IsValid() ||
		PooledTex->GetDesc().GetSize().X != Resolution.X ||
		PooledTex->GetDesc().GetSize().Y != Resolution.Y)
	{
		GRenderTargetPool.FindFreeElement(GraphBuilder.RHICmdList, Desc, PooledTex, DebugName);
		IsFirstTimeCreate = true;
	}

	ExternalTex.RDGTexture = GraphBuilder.RegisterExternalTexture(PooledTex);
	return IsFirstTimeCreate;
};

bool Create3DTexFn(FRDGBuilder& GraphBuilder, FPersistentTexture& ExternalTex, FIntVector Resolution, EPixelFormat PixelFormat, const TCHAR* DebugName)
{
	bool IsFirstTimeCreate = false;
	TRefCountPtr<IPooledRenderTarget>& PooledTex = ExternalTex.PooledTexture;

	FPooledRenderTargetDesc Desc = FPooledRenderTargetDesc::CreateVolumeDesc(
		Resolution.X, Resolution.Y, Resolution.Z,
		PixelFormat, FClearValueBinding::Transparent,
		TexCreate_ShaderResource, TexCreate_UAV, false
	);

	if (!PooledTex.IsValid() || PooledTex->GetDesc().GetSize() != Resolution)
	{
		GRenderTargetPool.FindFreeElement(GraphBuilder.RHICmdList, Desc, PooledTex, DebugName);
		IsFirstTimeCreate = true;
	}

	ExternalTex.RDGTexture = GraphBuilder.RegisterExternalTexture(PooledTex);
	return IsFirstTimeCreate;
}

class FCounterInitCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCounterInitCS);
	SHADER_USE_PARAMETER_STRUCT(FCounterInitCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, NumElements)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWCounterBuffer)
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
IMPLEMENT_GLOBAL_SHADER(FCounterInitCS, "/Engine/Private/RealtimeGI/RealtimeGICommon.usf", "CounterInitCS", SF_Compute);

void ClearCounterBuffer(FRDGBuilder& GraphBuilder, FScene* Scene, FRDGBufferRef Buffer, int32 NumElements)
{
	TShaderMapRef<FCounterInitCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
	auto* PassParameters = GraphBuilder.AllocParameters<FCounterInitCS::FParameters>();
	PassParameters->NumElements = NumElements;
	PassParameters->RWCounterBuffer = GraphBuilder.CreateUAV(Buffer);

	const int32 NumGroups = FMath::CeilToInt(float(NumElements) / float(FCounterInitCS::ThreadGroupSizeX));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("ClearCounterBuffer"),
		ComputeShader, PassParameters, FIntVector(NumGroups, 1, 1));
}

class FSimpleBlitPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSimpleBlitPS);
	SHADER_USE_PARAMETER_STRUCT(FSimpleBlitPS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) 
	{
		return true;
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FSimpleBlitPS, "/Engine/Private/RealtimeGI/RealtimeGICommon.usf", "SimpleBlitPS", SF_Pixel);

void SimpleBlit(
	FRDGBuilder& GraphBuilder, FScene* Scene, FViewInfo& View,
	FRDGTextureRef InputTexture, FRDGTextureRef OutputTexture,
	int32 InputMip, int32 OutputMip
)
{
	const FIntPoint& InputExtent = InputTexture->Desc.Extent / (1 << InputMip);
	const FIntPoint& OutputExtent = OutputTexture->Desc.Extent / (1 << OutputMip);
	const FScreenPassTextureViewport InputViewport(InputExtent, FIntRect(FIntPoint::ZeroValue, InputExtent));
	const FScreenPassTextureViewport OutputViewport(OutputExtent, FIntRect(FIntPoint::ZeroValue, OutputExtent));

	TShaderMapRef<FSimpleBlitPS> PixelShader(View.ShaderMap);
	auto* Parameters = GraphBuilder.AllocParameters<FSimpleBlitPS::FParameters>();
	Parameters->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(InputTexture, InputMip));
	Parameters->InputSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ENoAction, OutputMip);

	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("SimpleBlit"), View, OutputViewport, InputViewport, PixelShader, Parameters);
}
