#include "RealtimeGIGPUScene.h"

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "Misc/MemStack.h"
#include "HAL/IConsoleManager.h"
#include "EngineDefines.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "ConvexVolume.h"
#include "SceneTypes.h"
#include "SceneInterface.h"
#include "RendererInterface.h"
#include "PrimitiveViewRelevance.h"
#include "SceneManagement.h"
#include "ScenePrivateBase.h"
#include "PostProcess/SceneRenderTargets.h"
#include "Math/GenericOctree.h"
#include "LightSceneInfo.h"
#include "ShadowRendering.h"
#include "TextureLayout.h"
#include "SceneRendering.h"
#include "DynamicPrimitiveDrawing.h"
#include "LightPropagationVolume.h"
#include "ScenePrivate.h"
#include "RendererModule.h"
#include "LightPropagationVolumeSettings.h"
#include "CapsuleShadowRendering.h"
#include "Async/ParallelFor.h"
#include "RenderCore/Private/RenderGraphResourcePool.h"
#include "RealtimeGI/RealtimeGICardCapture.h"

// #pragma optimize("", off)


static TAutoConsoleVariable<int32> CVarRealtimeGIMaxObjectNum(
	TEXT("r.RealtimeGI.MaxObjectNum"),
	16384,
	TEXT("Number of primitives in GPU"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRealtimeGIMaxSurfaceCacheNum(
	TEXT("r.RealtimeGI.MaxSurfaceCacheNum"),
	4096,
	TEXT("Number of surface cache in GPU"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRealtimeGIMeshCardDefaultResolution(
	TEXT("r.RealtimeGI.MeshCardDefaultResolution"),
	32,
	TEXT("Default resolution of each surface cache"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRealtimeGIMeshCardResolutionThreshold1(
	TEXT("r.RealtimeGI.MeshCardResolutionThreshold1"),
	400.0,
	TEXT("If primitive world size lower than threshold, will use 1x base resolution"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRealtimeGIMeshCardResolutionThreshold2(
	TEXT("r.RealtimeGI.MeshCardResolutionThreshold2"),
	1600.0,
	TEXT("If primitive world size lower than threshold, will use 2x base resolution"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarRealtimeGIMeshCardResolutionThreshold4(
	TEXT("r.RealtimeGI.MeshCardResolutionThreshold4"),
	6400.0,
	TEXT("If primitive world size lower than threshold, will use 4x base resolution"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRealtimeGIAdaptiveSurfaceCacheNumPerFrame(
	TEXT("r.RealtimeGI.AdaptiveSurfaceCacheNumPerFrame"),
	8,
	TEXT("Num of surface caches to do adaptive resolution check per frame"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRealtimeGISurfaceCacheForceCapture(
	TEXT("r.RealtimeGI.SurfaceCacheForceCapture"),
	0,
	TEXT("Capture all surface caches per frame (for debug)"),
	ECVF_RenderThreadSafe
);


class FObjectInfoUpdateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FObjectInfoUpdateCS);
	SHADER_USE_PARAMETER_STRUCT(FObjectInfoUpdateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int, NumUpdateObjects)
		SHADER_PARAMETER_SRV(StructuredBuffer, ObjectInfoUploadBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWObjectInfoBuffer)
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
IMPLEMENT_GLOBAL_SHADER(FObjectInfoUpdateCS, "/Engine/Private/RealtimeGI/RealtimeGISceneUpdate.usf", "ObjectInfoUpdateCS", SF_Compute);

class FObjectInfoRemoveCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FObjectInfoRemoveCS);
	SHADER_USE_PARAMETER_STRUCT(FObjectInfoRemoveCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int, NumRemovedObjects)
		SHADER_PARAMETER_SRV(StructuredBuffer, RemovedObjectIdBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWObjectInfoBuffer)
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
IMPLEMENT_GLOBAL_SHADER(FObjectInfoRemoveCS, "/Engine/Private/RealtimeGI/RealtimeGISceneUpdate.usf", "ObjectInfoRemoveCS", SF_Compute);

class FObjectInfoCompactCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FObjectInfoCompactCS);
	SHADER_USE_PARAMETER_STRUCT(FObjectInfoCompactCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer, ObjectInfoBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWMiniObjectInfoBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, RWObjectInfoCounter)
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
IMPLEMENT_GLOBAL_SHADER(FObjectInfoCompactCS, "/Engine/Private/RealtimeGI/RealtimeGISceneUpdate.usf", "ObjectInfoCompactCS", SF_Compute);

class FSurfaceCacheInfoUpdateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfaceCacheInfoUpdateCS);
	SHADER_USE_PARAMETER_STRUCT(FSurfaceCacheInfoUpdateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int, SurfaceCacheNum)
		SHADER_PARAMETER_SRV(StructuredBuffer, SurfaceCacheInfoUploadBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer, CardInfoUploadBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWSurfaceCacheInfoBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer, RWCardInfoBuffer)
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
IMPLEMENT_GLOBAL_SHADER(FSurfaceCacheInfoUpdateCS, "/Engine/Private/RealtimeGI/RealtimeGISceneUpdate.usf", "SurfaceCacheInfoUpdateCS", SF_Compute);

// -------------------------------------------------------------------- //

struct FObjectInfoGPUData
{
public:
	FObjectInfoGPUData()
		: AllocationInfo(OBJECT_ID_INVALID, OBJECT_ID_INVALID, OBJECT_ID_INVALID, OBJECT_ID_INVALID)
	{

	};

	FObjectInfoGPUData(const FObjectInfo& InObjectInfo)
	{
		const FPrimitiveSceneInfo* Primitive = InObjectInfo.Primitive;
		AllocationInfo.X = InObjectInfo.ObjectId;
		AllocationInfo.Y = InObjectInfo.SurfaceCacheId;
		AllocationInfo.Z = 0;
		AllocationInfo.W = 0;

		LocalToWorldMatrix = Primitive->Proxy->GetLocalToWorld();
		WorldToLocalMatrix = LocalToWorldMatrix.Inverse();

		auto Vec3To4 = [](const FVector& V3)
		{
			return FVector4(V3.X, V3.Y, V3.Z, 1);
		};

		FBox LocalBounds = Primitive->Proxy->GetLocalBounds().GetBox();
		LocalBoundsMin = Vec3To4(LocalBounds.Min);
		LocalBoundsMax = Vec3To4(LocalBounds.Max);

		FBox WorldBounds = Primitive->Proxy->GetBounds().GetBox();
		WorldBoundsMin = Vec3To4(WorldBounds.Min);
		WorldBoundsMax = Vec3To4(WorldBounds.Max);
	};

	FIntVector4 AllocationInfo;	// int4(ObjectId, NumMeshCards, MeshCardResolution, ___)
	FVector4 LocalBoundsMin;
	FVector4 LocalBoundsMax;
	FVector4 WorldBoundsMin;
	FVector4 WorldBoundsMax;
	FMatrix LocalToWorldMatrix;
	FMatrix WorldToLocalMatrix;
};

// for object culling
struct FMiniObjectInfoGPUData
{
public:
	FVector4 WorldBoundsMinAndObjectId;
	FVector4 WorldBoundsMax;
};

struct FSurfaceCacheInfoGPUData
{
public:
	FSurfaceCacheInfoGPUData() {};
	FSurfaceCacheInfoGPUData(const FSurfaceCacheInfo& SurfaceCacheInfo)
	{
		SurfaceCacheId = SurfaceCacheInfo.SurfaceCacheId;
		NumMeshCards = SurfaceCacheInfo.NumMeshCards;
		MeshCardResolution = SurfaceCacheInfo.MeshCardResolution;
	}

	int32 SurfaceCacheId;
	int32 NumMeshCards;
	int32 MeshCardResolution;
	int32 Padding00;
};

struct FCardInfoGPUData
{
public:
	FMatrix LocalToCardMatrix;
	FVector4 CardUVTransform;
};

// -------------------------------------------------------------------- //

TArray<const FStaticMeshBatch*> GatherMeshBatch(const FPrimitiveSceneInfo* PrimitiveSceneInfo)
{
	TArray<const FStaticMeshBatch*> Result;
	const FPrimitiveSceneProxy* PrimitiveSceneProxy = PrimitiveSceneInfo->Proxy;

	int8 MinLOD = 0, MaxLOD = 0;
	PrimitiveSceneInfo->GetStaticMeshesLODRange(MinLOD, MaxLOD);

	for (int32 MeshIndex = 0; MeshIndex < PrimitiveSceneInfo->StaticMeshes.Num(); MeshIndex++)
	{
		const FStaticMeshBatch& MeshBatch = PrimitiveSceneInfo->StaticMeshes[MeshIndex];
		const FStaticMeshBatchRelevance& Relevance = PrimitiveSceneInfo->StaticMeshRelevances[MeshIndex];

		if (MeshBatch.LODIndex != MinLOD || !MeshBatch.bUseForMaterial /* || Relevance.bUseSkyMaterial */)
		{
			continue;
		}

		Result.Add(&MeshBatch);
	}

	return Result;
}

// https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x
void HashCombine(uint32& OutHashValue, void* Pointer)
{
	std::size_t PointerHash = std::hash<void*>{}(Pointer);
	OutHashValue ^= PointerHash + 0x9e3779b9 + (OutHashValue << 6) + (OutHashValue >> 2);
}

FSurfaceCacheKey::FSurfaceCacheKey(const FPrimitiveSceneInfo* InPrimitive)
{
	HashValue = 0;

	// if all vertex factory and material are same, we assume it share same surface cache
	TArray<const FStaticMeshBatch*> MeshBatches = GatherMeshBatch(InPrimitive);
	for (const FStaticMeshBatch* Mesh : MeshBatches)
	{
		HashCombine(HashValue, (void*)Mesh->VertexFactory);
		HashCombine(HashValue, (void*)Mesh->MaterialRenderProxy);

		VertexFactoryPointers.Add((void*)Mesh->VertexFactory);
		MaterialPointers.Add((void*)Mesh->MaterialRenderProxy);
	}
}

bool FSurfaceCacheKey::operator == (const FSurfaceCacheKey& Other) const
{
	if (HashValue != Other.HashValue)
	{
		return false;
	}

	if (VertexFactoryPointers.Num() != VertexFactoryPointers.Num() ||
		MaterialPointers.Num() != MaterialPointers.Num())
	{
		return false;
	}

	// hash same, we still need to check all mesh's vf and material
	for (int32 MeshId = 0; MeshId < MaterialPointers.Num(); MeshId++)
	{
		if (VertexFactoryPointers[MeshId] != Other.VertexFactoryPointers[MeshId] ||
			MaterialPointers[MeshId] != Other.MaterialPointers[MeshId])
		{
			return false;
		}
	}

	return true;
}

bool FSurfaceCacheKey::operator != (const FSurfaceCacheKey& Other) const
{
	return HashValue != Other.HashValue;
}

void FObjectInfo::UpdatePrimitive(const FPrimitiveSceneInfo* InPrimitive)
{
	Primitive = InPrimitive;
	PrimitiveComponentId = Primitive->Proxy->GetPrimitiveComponentId();
}

FSurfaceCacheInfo::FSurfaceCacheInfo(int32 InSurfaceCacheId)
{
	SurfaceCacheId = InSurfaceCacheId;
}

void FSurfaceCacheInfo::Empty()
{
	Primitive = nullptr;
	SurfaceCacheId = 0;
	NumMeshCards = 0;
	MeshCardResolution = 0;
	CardCaptureMeshBatches.Empty();
	LocalToCardMatrixs.Empty();
	CardUVTransforms.Empty();
	ReferenceHolders.Empty();
}

void FSurfaceCacheInfo::AddObjectReference(const FObjectInfo& ObjectInfo)
{
	ReferenceHolders.Add(ObjectInfo.ObjectId);
}

void FSurfaceCacheInfo::RemoveObjectReference(const FObjectInfo& ObjectInfo)
{
	ReferenceHolders.Remove(ObjectInfo.ObjectId);
}

void FRealtimeGIGPUScene::OnRendererSceneUpdatePrimitives(const TArray<FPrimitiveSceneInfo*>& AddedPrimitiveSceneInfos, const TArray<FPrimitiveSceneInfo*>& RemovedPrimitiveSceneInfos)
{
	for (FPrimitiveSceneInfo* Primitive : RemovedPrimitiveSceneInfos)
	{
		const FPrimitiveComponentId& PrimitiveId = Primitive->Proxy->GetPrimitiveComponentId();

		// just record the first dirty bound, cause same primitive will remove twice when ctrl+z
		if (!PendingPrimitivesToRemove.Find(PrimitiveId))
		{
			PendingPrimitivesToRemove.Add(PrimitiveId, Primitive->Proxy->GetBounds());
		}

		// if update, it will be add back
		PendingPrimitivesToAdd.Remove(PrimitiveId);

		// UE_LOG(LogTemp, Warning, TEXT("[FRealtimeGIGPUScene] remove primitive: %llx, id is: %d"), Primitive, PrimitiveId.PrimIDValue);
	}

	for (FPrimitiveSceneInfo* Primitive : AddedPrimitiveSceneInfos)
	{
		const FPrimitiveComponentId& PrimitiveId = Primitive->Proxy->GetPrimitiveComponentId();
		PendingPrimitivesToAdd.Add(PrimitiveId, Primitive);

		// UE_LOG(LogTemp, Warning, TEXT("[FRealtimeGIGPUScene] add primitive: %llx, id is: %d"), Primitive, PrimitiveId.PrimIDValue);
	}
}

void FRealtimeGIGPUScene::FlushPrimitiveUpdateQueue()
{
	for (TMap<FPrimitiveComponentId, FBoxSphereBounds>::TIterator Iter = PendingPrimitivesToRemove.CreateIterator(); Iter; ++Iter)
	{
		const FPrimitiveComponentId& PrimitiveId = Iter->Key;
		const FBoxSphereBounds PrimitiveWorldBound = Iter->Value;

		// not in GI scene? maybe other primitives not visible for GI, like particle
		if (!ObjectIdAllocator.Find(PrimitiveId))
		{
			continue;
		}

		// same primitive remove and add at same time, we assume it just need update
		auto FindResult = PendingPrimitivesToAdd.Find(PrimitiveId);
		const FPrimitiveSceneInfo* Primitive = FindResult ? *FindResult : nullptr;
		if (FindResult && Primitive->Proxy->IsVisibleInRealtimeGI())
		{
			// do update
			int32 DirtyObjectId = OBJECT_ID_INVALID;
			if (ObjectIdAllocator.Find(PrimitiveId, DirtyObjectId))
			{
				ObjectInfos[DirtyObjectId].UpdatePrimitive(Primitive);
				ObjectUpdateCommands.Add(DirtyObjectId);

				DirtyPrimitiveBounds.Add(PrimitiveWorldBound);				// old bound
				DirtyPrimitiveBounds.Add(Primitive->Proxy->GetBounds());	// new bound
			}

			PendingPrimitivesToAdd.Remove(PrimitiveId);
			continue;
		}
		
		// do remove
		int32 FreeId = ObjectIdAllocator.ReleaseElement(PrimitiveId);
		ObjectRemoveCommands.Add(FreeId);
		DirtyPrimitiveBounds.Add(PrimitiveWorldBound);
	}

	for (TMap<FPrimitiveComponentId, FPrimitiveSceneInfo*>::TIterator Iter = PendingPrimitivesToAdd.CreateIterator(); Iter; ++Iter)
	{
		const FPrimitiveComponentId& PrimitiveId = Iter->Key;
		const FPrimitiveSceneInfo* Primitive = Iter->Value;

		if (!Primitive->Proxy->IsVisibleInRealtimeGI())
		{
			continue;
		}

		int32 NewId = ObjectIdAllocator.AllocateElement(PrimitiveId);
		ObjectInfos[NewId] = FObjectInfo(Primitive, NewId);
		ObjectAddCommands.Add(NewId);
		DirtyPrimitiveBounds.Add(Primitive->Proxy->GetBounds());
	}
}

void FRealtimeGIGPUScene::PrepareSurfaceCacheCapture()
{
	// 1. handle object add, check if surface cache exist and do allocation
	for (int32 ObjectId : ObjectAddCommands)
	{
		ReferenceSurfaceCache(ObjectInfos[ObjectId]);
	}

	// 2. handle object update, check if material or vf change, if change we re allocate
	for (int32 ObjectId : ObjectUpdateCommands)
	{
		ReferenceSurfaceCache(ObjectInfos[ObjectId]);
	}

	// 3. handle object remove
	for (int32 ObjectId : ObjectRemoveCommands)
	{
		DeReferenceSurfaceCache(ObjectInfos[ObjectId]);
	}

	// 4. loop and see if surface cache need change size
	TArray<int32> SurfaceCacheIds = SurfaceCacheAllocator.GetAllocatedElements();
	const int32 NumSurfaceCacheToCheck = FMath::Min(SurfaceCacheIds.Num(), CVarRealtimeGIAdaptiveSurfaceCacheNumPerFrame.GetValueOnRenderThread());
	const int32 StartIndex = FMath::RandRange(0, SurfaceCacheIds.Num());
	for (int32 i = 0; i < NumSurfaceCacheToCheck; i++)
	{
		int32 Index = (StartIndex + i) % SurfaceCacheIds.Num();
		int32 SurfaceCacheId = SurfaceCacheIds[Index];

		AdjustSurfaceCacheResolution(SurfaceCacheId);
	}

	// 5. gather mesh batches (step 1,2 will populate SurfaceCacheCaptureCommands)
	for (int32 SurfaceCacheId : SurfaceCacheCaptureCommands)
	{
		GatherCardCaptureMeshElements(SurfaceCacheId);
		AllocateSurfaceCacheTextureSpace(SurfaceCacheId);
	}
}

void FRealtimeGIGPUScene::PreUpdate(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views)
{
	// init all view
	for (FViewInfo& View : Views)
	{
		View.RealtimeGIVoxelClipmap = GetOrCreatePerViewObject(VoxelClipmapViewMap, View.GetViewKey());
		View.RealtimeGIRadianceCache = GetOrCreatePerViewObject(RadianceCacheViewMap, View.GetViewKey());
		View.RealtimeGIScreenGatherPipeline = GetOrCreatePerViewObject(ScreenGatherPipelineViewMap, View.GetViewKey());
	}

	// do some init
	const int32 MaxObjectNum = CVarRealtimeGIMaxObjectNum.GetValueOnRenderThread();
	ObjectIdAllocator.Init(MaxObjectNum);

	// @TODO: change size
	if (ObjectInfos.Num() == 0)
	{
		ObjectInfos.SetNum(MaxObjectNum);
	}

	const int32 MaxSurfaceCacheNum = CVarRealtimeGIMaxSurfaceCacheNum.GetValueOnRenderThread();
	SurfaceCacheAllocator.Init(MaxSurfaceCacheNum);

	// @TODO: change size
	if (SurfaceCacheInfos.Num() == 0)
	{
		SurfaceCacheInfos.SetNum(MaxSurfaceCacheNum);
	}

	SurfaceCacheAtlasAllocator.Init(SurfaceCacheAtlasResolution);
}

void FRealtimeGIGPUScene::Update(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views)
{
	PreUpdate(GraphBuilder, Scene, Views);

	FlushPrimitiveUpdateQueue();

	PrepareSurfaceCacheCapture();

	PrepareRenderResources(GraphBuilder, Scene);

	SyncObjectInfosToGPU(GraphBuilder, Scene);

	SyncSurfaceCacheInfosToGPU(GraphBuilder, Scene);

	RealtimeGISurfaceCacheCapturePass(GraphBuilder, Scene, Views[0]);	// Views[0] just for provide view uniform buffer

	for (FViewInfo& View : Views)
	{
		View.RealtimeGIVoxelClipmap->Update(GraphBuilder, Scene, View);
	}

	PostUpdate(GraphBuilder, Scene, Views);
}

void FRealtimeGIGPUScene::PostUpdate(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views)
{
	for (const int32& ObjectId : ObjectRemoveCommands)
	{
		ObjectInfos[ObjectId] = FObjectInfo();	// reset to null
	}

	// @TODO: use frame allocator
	ObjectRemoveCommands.Empty();
	ObjectUpdateCommands.Empty();
	ObjectAddCommands.Empty();
	SurfaceCacheCaptureCommands.Empty();
	SurfaceCacheClearCommands.Empty();
	DirtyPrimitiveBounds.Empty();
	PendingPrimitivesToAdd.Empty();
	PendingPrimitivesToRemove.Empty();
}

void FRealtimeGIGPUScene::PrepareRenderResources(FRDGBuilder& GraphBuilder, FScene* Scene)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	const int32 MaxObjectNum = CVarRealtimeGIMaxObjectNum.GetValueOnRenderThread();
	const int32 MaxSurfaceCacheNum = CVarRealtimeGIMaxSurfaceCacheNum.GetValueOnRenderThread();

	// create buffer
	if (ObjectInfoBuffer.NumBytes == 0)
	{
		ObjectInfoBuffer.Initialize(
			sizeof(FObjectInfoGPUData), MaxObjectNum,
			BUF_Static,
			TEXT("ObjectInfoBuffer")
		);

		TArray<FObjectInfoGPUData> InitData;
		for (int32 i = 0; i < MaxObjectNum; i++)
		{
			InitData.Add(FObjectInfoGPUData());
		}

		const int32 NumBytes = MaxObjectNum * sizeof(FObjectInfoGPUData);
		FRHIStructuredBuffer* BufferRHI = ObjectInfoBuffer.Buffer;

		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, InitData.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}

	if (ObjectInfoUploadBuffer.NumBytes == 0)
	{
		ObjectInfoUploadBuffer.Initialize(
			sizeof(FObjectInfoGPUData) * MaxObjectNum,
			BUF_Dynamic,
			TEXT("ObjectInfoUploadBuffer")
		);
	}

	if (RemovedObjectIdBuffer.NumBytes == 0)
	{
		RemovedObjectIdBuffer.Initialize(
			sizeof(int32) * MaxObjectNum,
			BUF_Dynamic,
			TEXT("RemovedObjectIdBuffer")
		);
	}

	if (CardInfoBuffer.NumBytes == 0)
	{
		CardInfoBuffer.Initialize(
			sizeof(FCardInfoGPUData), MaxSurfaceCacheNum * MAX_CARDS_PER_MESH,
			BUF_Static,
			TEXT("CardInfoBuffer")
		);
	}

	if (CardInfoUploadBuffer.NumBytes == 0)
	{
		CardInfoUploadBuffer.Initialize(
			sizeof(FCardInfoGPUData) * MaxSurfaceCacheNum * MAX_CARDS_PER_MESH,
			BUF_Dynamic,
			TEXT("CardInfoUploadBuffer")
		);
	}

	if (CardClearQuadUVTransformBuffer.NumBytes == 0)
	{
		CardClearQuadUVTransformBuffer.Initialize(
			sizeof(FVector4) * MaxSurfaceCacheNum * MAX_CARDS_PER_MESH,
			BUF_Dynamic,
			TEXT("CardClearQuadUVTransformBuffer")
		);
	}

	if (SurfaceCacheInfoBuffer.NumBytes == 0)
	{
		SurfaceCacheInfoBuffer.Initialize(
			sizeof(FSurfaceCacheInfoGPUData), MaxSurfaceCacheNum,
			BUF_Static,
			TEXT("SurfaceCacheInfoBuffer")
		);
	}

	if (SurfaceCacheInfoUploadBuffer.NumBytes == 0)
	{
		SurfaceCacheInfoUploadBuffer.Initialize(
			sizeof(FSurfaceCacheInfoGPUData) * MaxSurfaceCacheNum,
			BUF_Dynamic,
			TEXT("SurfaceCacheInfoUploadBuffer")
		);
	}

	ObjectInfoCounter = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
		TEXT("ObjectInfoCounter")
	);

	if (MiniObjectInfoBuffer.NumBytes == 0)
	{
		MiniObjectInfoBuffer.Initialize(
			sizeof(FMiniObjectInfoGPUData), MaxObjectNum,
			BUF_Static,
			TEXT("MiniObjectInfoBuffer")
		);
	}
	
	SurfaceCacheAtlasNeedClear = false;
	FIntPoint RTResolution = FIntPoint(SurfaceCacheAtlasResolution, SurfaceCacheAtlasResolution);
	SurfaceCacheAtlasNeedClear |= Create2DTexFn(GraphBuilder, SurfaceCacheAtlas[RT_BaseColor], RTResolution, PF_R8G8B8A8, TEXT("SurfaceCacheBaseColor"));
	SurfaceCacheAtlasNeedClear |= Create2DTexFn(GraphBuilder, SurfaceCacheAtlas[RT_Normal], RTResolution, PF_FloatRGB, TEXT("SurfaceCacheNormal"));
	SurfaceCacheAtlasNeedClear |= Create2DTexFn(GraphBuilder, SurfaceCacheAtlas[RT_Emissive], RTResolution, PF_FloatRGB, TEXT("SurfaceCacheEmissive"));
	SurfaceCacheAtlasNeedClear |= Create2DTexFn(GraphBuilder, SurfaceCacheAtlas[RT_Depth], RTResolution, PF_R16F, TEXT("SurfaceCacheDepth"));

	FRDGTextureDesc DSDesc = FRDGTextureDesc::Create2D(RTResolution, PF_DepthStencil, FClearValueBinding::DepthFar, TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);
	CardCaptureDepthStencil = GraphBuilder.CreateTexture(DSDesc, TEXT("MeshCardCaptureDS"));
}

void FRealtimeGIGPUScene::SyncObjectInfosToGPU(FRDGBuilder& GraphBuilder, FScene* Scene)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	const int32 MaxObjectNum = CVarRealtimeGIMaxObjectNum.GetValueOnRenderThread();
	const int32 NumUpdateObjects = ObjectUpdateCommands.Num() + ObjectAddCommands.Num();
	const int32 NumRemovedObjects = ObjectRemoveCommands.Num();

	// 1. copy removed object id to gpu
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if(NumRemovedObjects > 0)
#endif
	{
		FRHIStructuredBuffer* BufferRHI = RemovedObjectIdBuffer.Buffer;
		int32 NumBytes = NumRemovedObjects * sizeof(int32);
		TArray<int32> ObjectsToRemove = ObjectRemoveCommands.Array();

		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, ObjectsToRemove.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}

	// 2. remove and clear object infos in GPU
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (NumRemovedObjects > 0)
#endif
	{
		TShaderMapRef<FObjectInfoRemoveCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FObjectInfoRemoveCS::FParameters>();
		PassParameters->NumRemovedObjects = NumRemovedObjects;
		PassParameters->RemovedObjectIdBuffer = RemovedObjectIdBuffer.SRV;
		PassParameters->RWObjectInfoBuffer = ObjectInfoBuffer.UAV;

		int32 NumGroups = FMath::CeilToInt(float(NumRemovedObjects) / float(FObjectInfoRemoveCS::ThreadGroupSizeX));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("ObjectInfoRemove"),
			ComputeShader, PassParameters, FIntVector(NumGroups, 1, 1));
	}

	// 3. copy object infos need to update & add to gpu
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (NumUpdateObjects > 0)
#endif
	{
		TArray<FObjectInfoGPUData> UpdateObjectInfos;

		for (const int32& ObjectId : ObjectUpdateCommands)
		{
			UpdateObjectInfos.Add(FObjectInfoGPUData(ObjectInfos[ObjectId]));
		}
		for (const int32& ObjectId : ObjectAddCommands)
		{
			UpdateObjectInfos.Add(FObjectInfoGPUData(ObjectInfos[ObjectId]));
		}

		FRHIStructuredBuffer* BufferRHI = ObjectInfoUploadBuffer.Buffer;
		int32 NumBytes = NumUpdateObjects * sizeof(FObjectInfoGPUData);

		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, UpdateObjectInfos.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}

	// 4. update new & modified object infos to GPU
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (NumUpdateObjects > 0)
#endif
	{
		TShaderMapRef<FObjectInfoUpdateCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		auto* PassParameters = GraphBuilder.AllocParameters<FObjectInfoUpdateCS::FParameters>();
		PassParameters->NumUpdateObjects = NumUpdateObjects;
		PassParameters->ObjectInfoUploadBuffer = ObjectInfoUploadBuffer.SRV;
		PassParameters->RWObjectInfoBuffer = ObjectInfoBuffer.UAV;

		const int32 NumGroups = FMath::CeilToInt(float(NumUpdateObjects) / float(FObjectInfoUpdateCS::ThreadGroupSizeX));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("ObjectInfosUpdate"),
			ComputeShader, PassParameters, FIntVector(NumGroups, 1, 1));
	}

	// 5. shrink and compact the array
	// [_, B, _, C, A, _, D, _, E] --> [B, C, A, D, E]
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (NumRemovedObjects > 0 || NumUpdateObjects > 0)
#endif
	{
		ClearCounterBuffer(GraphBuilder, Scene, ObjectInfoCounter, 1);

		TShaderMapRef<FObjectInfoCompactCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		FObjectInfoCompactCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FObjectInfoCompactCS::FParameters>();
		PassParameters->ObjectInfoBuffer = ObjectInfoBuffer.SRV;
		PassParameters->RWMiniObjectInfoBuffer = MiniObjectInfoBuffer.UAV;
		PassParameters->RWObjectInfoCounter = GraphBuilder.CreateUAV(ObjectInfoCounter);

		const int32 NumGroups = FMath::CeilToInt(float(MaxObjectNum) / float(FObjectInfoCompactCS::ThreadGroupSizeX));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("ObjectInfoCompact"),
			ComputeShader, PassParameters, FIntVector(NumGroups, 1, 1));
	}
}

void FRealtimeGIGPUScene::SyncSurfaceCacheInfosToGPU(FRDGBuilder& GraphBuilder, FScene* Scene)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	const int32 SurfaceCacheNum = SurfaceCacheCaptureCommands.Num();

	// 1. fill data: surface cache info
	TArray<FSurfaceCacheInfoGPUData> SurfaceCacheInfoUploadData;
	for (const int32& SurfaceCacheId : SurfaceCacheCaptureCommands)
	{
		const FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[SurfaceCacheId];
		SurfaceCacheInfoUploadData.Add(FSurfaceCacheInfoGPUData(SurfaceCacheInfo));
	}

	// 2. upload surface cache info
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (SurfaceCacheNum > 0)
#endif
	{
		FRHIStructuredBuffer* BufferRHI = SurfaceCacheInfoUploadBuffer.Buffer;
		int32 NumBytes = SurfaceCacheNum * sizeof(FSurfaceCacheInfoGPUData);

		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, SurfaceCacheInfoUploadData.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}

	// 3. fill data: mesh card data for each surface cache
	int32 UploadDataOffset = 0;
	TArray<FCardInfoGPUData> CardInfoUploadData;
	CardInfoUploadData.SetNum(SurfaceCacheNum * MAX_CARDS_PER_MESH);

	for (const int32& SurfaceCacheId : SurfaceCacheCaptureCommands)
	{
		const FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[SurfaceCacheId];
		
		for (int32 CardIndex = 0; CardIndex < SurfaceCacheInfo.NumMeshCards; CardIndex++)
		{
			FCardInfoGPUData& CardInfo = CardInfoUploadData[UploadDataOffset + CardIndex];
			CardInfo.LocalToCardMatrix = SurfaceCacheInfo.LocalToCardMatrixs[CardIndex];
			CardInfo.CardUVTransform = SurfaceCacheInfo.CardUVTransforms[CardIndex];
		}

		UploadDataOffset += MAX_CARDS_PER_MESH;
	}

	// 4. upload mesh card info
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (SurfaceCacheNum > 0)
#endif
	{
		FRHIStructuredBuffer* BufferRHI = CardInfoUploadBuffer.Buffer;
		int32 NumBytes = SurfaceCacheNum * sizeof(FCardInfoGPUData) * MAX_CARDS_PER_MESH;

		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, CardInfoUploadData.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}

	// 5. copy data from transient buffer to RW buffer
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (SurfaceCacheNum > 0)
#endif
	{
		TShaderMapRef<FSurfaceCacheInfoUpdateCS> ComputeShader(GetGlobalShaderMap(Scene->GetFeatureLevel()));
		FSurfaceCacheInfoUpdateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSurfaceCacheInfoUpdateCS::FParameters>();
		PassParameters->SurfaceCacheNum = SurfaceCacheNum;
		PassParameters->SurfaceCacheInfoUploadBuffer = SurfaceCacheInfoUploadBuffer.SRV;
		PassParameters->CardInfoUploadBuffer = CardInfoUploadBuffer.SRV;
		PassParameters->RWSurfaceCacheInfoBuffer = SurfaceCacheInfoBuffer.UAV;
		PassParameters->RWCardInfoBuffer = CardInfoBuffer.UAV;

		const int32 NumGroups = FMath::CeilToInt(float(SurfaceCacheNum) / float(FSurfaceCacheInfoUpdateCS::ThreadGroupSizeX));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("SurfaceCacheInfoUpdate"),
			ComputeShader, PassParameters, FIntVector(NumGroups, 1, 1));
	}

	// 6. upload card clear list
	// note: we don't use indirect draw cause [newly added object's data] will override [removed object's data] in SurfaceCacheInfoBuffer
#if !ALLOW_EMPTY_DISPATCH_FOR_DEBUG
	if (SurfaceCacheClearCommands.Num() > 0)
#endif
	{
		FRHIStructuredBuffer* BufferRHI = CardClearQuadUVTransformBuffer.Buffer;
		int32 NumBytes = SurfaceCacheClearCommands.Num() * sizeof(FVector4);

		void* MappedRawData = RHICmdList.LockStructuredBuffer(BufferRHI, 0, NumBytes, RLM_WriteOnly);
		FMemory::Memcpy(MappedRawData, SurfaceCacheClearCommands.GetData(), NumBytes);
		RHICmdList.UnlockStructuredBuffer(BufferRHI);
	}
}

FVector4 FRealtimeGIGPUScene::CalcViewportInfo(const FSurfaceCacheInfo& SurfaceCacheInfo, const int32& CardIndex)
{
	FVector4 CardSizeAndOffset = SurfaceCacheInfo.CardUVTransforms[CardIndex];
	FVector4 UVTransform = CardSizeAndOffset / float(SurfaceCacheAtlasResolution);

	float PaddingScale = (float(SurfaceCacheInfo.MeshCardResolution) - 1.0) / float(SurfaceCacheInfo.MeshCardResolution);	// padding 1 texel
	
	// viewport center is (0, 0) but uv center is (0.5, 0.5)
	float OffsetX = 0.5 * UVTransform.X;
	float OffsetY = 0.5 * UVTransform.Y;

	FVector4 Result = FVector4(
		UVTransform.X * PaddingScale,
		UVTransform.Y * PaddingScale,
		(UVTransform.Z + OffsetX) * 2.0 - 1.0,	// using this offset in clip space [-1, 1]
		(UVTransform.W + OffsetY) * 2.0 - 1.0
	);

	// @TODO: fetch info from RHI
	bool IsRHITextureOriginInTopLeft = true;
	if (IsRHITextureOriginInTopLeft)
	{
		Result.Y = Result.Y * -1.0;
		Result.W = Result.W * -1.0;
	}

	return Result;

}

void FRealtimeGIGPUScene::ReferenceSurfaceCache(FObjectInfo& ObjectInfo)
{
	FSurfaceCacheKey SurfaceCacheKey = FSurfaceCacheKey(ObjectInfo.Primitive);
	int32 SurfaceCacheId = OBJECT_ID_INVALID;

	// if not exist we allocate it
	if (!SurfaceCacheAllocator.Find(SurfaceCacheKey, SurfaceCacheId))
	{
		SurfaceCacheId = SurfaceCacheAllocator.AllocateElement(SurfaceCacheKey);
		SurfaceCacheInfos[SurfaceCacheId] = FSurfaceCacheInfo(SurfaceCacheId);

		// build capture command 
		SurfaceCacheCaptureCommands.Add(SurfaceCacheId);
	}

	// same object will not ref twice, cause we use TSet in FSurfaceCacheInfo
	SurfaceCacheInfos[SurfaceCacheId].AddObjectReference(ObjectInfo);

	// if surface cache change, release old item
	if (ObjectInfo.SurfaceCacheKey != SurfaceCacheKey && ObjectInfo.SurfaceCacheId != OBJECT_ID_INVALID)
	{
		DeReferenceSurfaceCache(ObjectInfo);
	}

	// sync info to object
	ObjectInfo.SurfaceCacheKey = SurfaceCacheKey;
	ObjectInfo.SurfaceCacheId = SurfaceCacheId;
}

void FRealtimeGIGPUScene::DeReferenceSurfaceCache(FObjectInfo& ObjectInfo)
{
	FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[ObjectInfo.SurfaceCacheId];
	SurfaceCacheInfo.RemoveObjectReference(ObjectInfo);

	// if nobody use surface cache, we release it
	if (SurfaceCacheInfo.GetRefCount() == 0)
	{
		SurfaceCacheAllocator.ReleaseElement(ObjectInfo.SurfaceCacheKey);
		ReleaseSurfaceCacheTextureSpace(ObjectInfo.SurfaceCacheId);
		SurfaceCacheInfo.Empty();
	}

	// sync info to object
	ObjectInfo.SurfaceCacheKey = FSurfaceCacheKey();	// reset to null
	ObjectInfo.SurfaceCacheId = OBJECT_ID_INVALID;
}

void FRealtimeGIGPUScene::AllocateSurfaceCacheTextureSpace(const int32& SurfaceCacheId)
{
	FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[SurfaceCacheId];

	// if first time allocate, we give a default size, will apply adaptive size later
	if (SurfaceCacheInfo.MeshCardResolution == 0)
	{
		SurfaceCacheInfo.MeshCardResolution = CVarRealtimeGIMeshCardDefaultResolution.GetValueOnRenderThread();
	}

	for (int32 CardIndex = 0; CardIndex < SurfaceCacheInfo.NumMeshCards; CardIndex++)
	{
		FStupidQuadTreeNode Node = SurfaceCacheAtlasAllocator.AllocateElement(SurfaceCacheInfo.MeshCardResolution);
		FVector4 CardSizeAndOffset = FVector4(Node.Size, Node.Size, Node.Min.X, Node.Min.Y);
		SurfaceCacheInfo.CardUVTransforms.Add(CardSizeAndOffset);
	}
}

void FRealtimeGIGPUScene::ReleaseSurfaceCacheTextureSpace(const int32& SurfaceCacheId)
{
	FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[SurfaceCacheId];
	if (SurfaceCacheInfo.MeshCardResolution == 0)
	{
		return;
	}

	for (int32 CardIndex = 0; CardIndex < SurfaceCacheInfo.NumMeshCards; CardIndex++)
	{
		FVector4 CardSizeAndOffset = SurfaceCacheInfo.CardUVTransforms[CardIndex];
		SurfaceCacheClearCommands.Add(CardSizeAndOffset);

		FStupidQuadTreeNode FreeNode;
		FreeNode.Size = CardSizeAndOffset.X;	// x == y always
		FreeNode.Min = FIntPoint(CardSizeAndOffset.Z, CardSizeAndOffset.W);
		FreeNode.Max = FreeNode.Min + FIntPoint(CardSizeAndOffset.X, CardSizeAndOffset.Y);
		FreeNode.Center = (FreeNode.Max + FreeNode.Min) / 2;

		SurfaceCacheAtlasAllocator.ReleaseElement(FreeNode);
	}

	SurfaceCacheInfo.MeshCardResolution = 0;
	SurfaceCacheInfo.CardUVTransforms.Empty();
}

void FRealtimeGIGPUScene::GatherCardCaptureMeshElements(const int32& SurfaceCacheId)
{
	FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[SurfaceCacheId];

	check(SurfaceCacheInfo.GetRefCount() > 0);
	int32 ObjectId = SurfaceCacheInfo.GetReferenceHolders()[0];
	const FObjectInfo& ObjectInfo = ObjectInfos[ObjectId];
	SurfaceCacheInfo.Primitive = ObjectInfo.Primitive;

	// 1. setup mesh card placement and capture matrix
	// @TODO: precomputed card placement
	SurfaceCacheInfo.NumMeshCards = CubeFace_MAX;

	const FBox& LocalBounds = SurfaceCacheInfo.Primitive->Proxy->GetLocalBounds().GetBox();
	FVector LocalBoundsCenter = (LocalBounds.Max + LocalBounds.Min) * 0.5;
	FVector LocalBoundsSize = LocalBounds.GetExtent() * 2.0;
	LocalBoundsSize *= (1.0 + 1e-3);	// a little padding

	SurfaceCacheInfo.LocalToCardMatrixs.Empty();
	for (int32 CardIndex = 0; CardIndex < SurfaceCacheInfo.NumMeshCards; CardIndex++)
	{
		FMatrix LocalToCard = CalcCardCaptureViewProjectionMatrix(LocalBoundsCenter, LocalBoundsSize, (ECubeFace)CardIndex);
		SurfaceCacheInfo.LocalToCardMatrixs.Add(LocalToCard);
	}

	// 2. gather card capture mesh elements
	SurfaceCacheInfo.CardCaptureMeshBatches.Empty();
	TArray<const FStaticMeshBatch*> MeshBatches = GatherMeshBatch(SurfaceCacheInfo.Primitive);
	for (const FStaticMeshBatch* Mesh : MeshBatches)
	{
		SurfaceCacheInfo.CardCaptureMeshBatches.Add(*Mesh);
	}

	// single instance represent a mesh card
	for (FMeshBatch& MeshBatch : SurfaceCacheInfo.CardCaptureMeshBatches)
	{
		for (FMeshBatchElement& Element : MeshBatch.Elements)
		{
			Element.NumInstances = SurfaceCacheInfo.NumMeshCards;
		}
	}
}

void FRealtimeGIGPUScene::AdjustSurfaceCacheResolution(const int32& SurfaceCacheId)
{
	FSurfaceCacheInfo& SurfaceCacheInfo = SurfaceCacheInfos[SurfaceCacheId];
	TArray<int32> ObjectIds = SurfaceCacheInfo.GetReferenceHolders();

	const int32 MinResolution = SurfaceCacheAtlasAllocator.GetMinNodeSize();
	const int32 MaxResolution = SurfaceCacheAtlasAllocator.GetMaxNodeSize();
	int32 RequireResolution = 0;

	// loop all reference objects to calculate max resolution
	for (int32 ObjectId : ObjectIds)
	{
		const FObjectInfo& ObjectInfo = ObjectInfos[ObjectId];
		const FPrimitiveSceneProxy* Primitive = ObjectInfo.Primitive->Proxy;
		const FBox& LocalBounds = Primitive->GetLocalBounds().GetBox();
		const FVector LocalSizeXYZ = LocalBounds.Max - LocalBounds.Min;
		const FVector WorldScale = Primitive->GetLocalToWorld().GetScaleVector();

		// calculate card resolution based on primitive's size
		float WorldSize = 0;
		WorldSize = FMath::Max(WorldSize, WorldScale[0] * LocalSizeXYZ[0]);
		WorldSize = FMath::Max(WorldSize, WorldScale[1] * LocalSizeXYZ[1]);
		WorldSize = FMath::Max(WorldSize, WorldScale[2] * LocalSizeXYZ[2]);

		int32 Size = 0;
		if (WorldSize < CVarRealtimeGIMeshCardResolutionThreshold1.GetValueOnRenderThread())
		{
			Size = MinResolution * 1;
		}
		else if (WorldSize < CVarRealtimeGIMeshCardResolutionThreshold2.GetValueOnRenderThread())
		{
			Size = MinResolution * 2;
		}
		else if (WorldSize < CVarRealtimeGIMeshCardResolutionThreshold4.GetValueOnRenderThread())
		{
			Size = MinResolution * 4;
		}
		else
		{
			Size = MinResolution * 8;
		}

		RequireResolution = FMath::Max(RequireResolution, Size);
		RequireResolution = FMath::Clamp(RequireResolution, MinResolution, MaxResolution);
	}

	// if need change size or in debug mode
	if (SurfaceCacheInfo.MeshCardResolution != RequireResolution ||
		CVarRealtimeGISurfaceCacheForceCapture.GetValueOnRenderThread() > 0)
	{
		// need re-capture
		ReleaseSurfaceCacheTextureSpace(SurfaceCacheId);
		SurfaceCacheCaptureCommands.Add(SurfaceCacheId);

		// mark all primitives as dirty
		for (int32 ObjectId : ObjectIds)
		{
			const FObjectInfo& ObjectInfo = ObjectInfos[ObjectId];
			const FBoxSphereBounds& WorldBound = ObjectInfo.Primitive->Proxy->GetBounds();
			DirtyPrimitiveBounds.Add(WorldBound);
		}

		SurfaceCacheInfo.MeshCardResolution = RequireResolution;
	}
}

void RealtimeGISceneUpdate(FRDGBuilder& GraphBuilder, FScene* Scene, TArray<FViewInfo>& Views)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RealtimeGISceneUpdate");
	DECLARE_GPU_STAT(RealtimeGISceneUpdate);
	RDG_GPU_STAT_SCOPE(GraphBuilder, RealtimeGISceneUpdate);

	Scene->RealtimeGIScene.Update(GraphBuilder, Scene, Views);
}
