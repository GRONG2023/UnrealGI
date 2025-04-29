#pragma once

#include "CoreMinimal.h"
#include "Math/IntVector.h"
#include "PrimitiveSceneInfo.h"
#include <set>

template<typename KeyType>
class FStupidLinearAllocator
{
public:
	FStupidLinearAllocator() {};

	void Init(int32 NumElements)
	{
		if (MaxNumElements == 0)
		{
			MaxNumElements = NumElements;
			for (int32 i = 0; i < NumElements; i++)
			{
				UnusedElementIds.Enqueue(i);
			}
		}

		// @TODO: change size
	}

	int32 ReleaseElement(const KeyType& KeyValue)
	{
		int32 FreeId = -1;
		bool RemoveSuccess = AllocatedElementIds.RemoveAndCopyValue(KeyValue, FreeId);
		check(RemoveSuccess);

		UnusedElementIds.Enqueue(FreeId);
		return FreeId;
	}

	int32 AllocateElement(const KeyType& KeyValue)
	{
		int32 Id = -1;
		bool AllocateSuccess = UnusedElementIds.Dequeue(Id);
		check(AllocateSuccess);

		AllocatedElementIds.Add(KeyValue, Id);
		return Id;
	}

	bool Find(const KeyType& KeyValue, int32& OutId)
	{
		int32* Id = AllocatedElementIds.Find(KeyValue);
		if (Id == nullptr)
		{
			return false;
		}

		OutId = *Id;
		return true;
	}

	bool Find(const KeyType& KeyValue)
	{
		int32 OutId = 0;
		return Find(KeyValue, OutId);
	}

	int32 GetMaxNumElements() { return MaxNumElements; };
	int32 GetAllocatedElementNums() { return AllocatedElementIds.Num(); };
	TArray<int32> GetAllocatedElements()
	{
		TArray<int32> Result;
		AllocatedElementIds.GenerateValueArray(Result);
		return Result;
	};

protected:
	TQueue<int32> UnusedElementIds;
	TMap<KeyType, int32> AllocatedElementIds;
	int32 MaxNumElements = 0;
};

enum EStupidQuadTreeChild
{
	TC_TopLeft = 0,
	TC_TopRight,
	TC_BottomLeft,
	TC_BottomRight,
	Child_Num
};

struct FNodeAllocationInfo
{
	FNodeAllocationInfo()
		: IsEmpty(true)
		, IsFull(false)
	{

	};

	uint8 IsEmpty : 1;
	uint8 IsFull : 1;
};

struct FStupidQuadTreeNode
{
	int32 Index = 0;
	int32 Size = 0;
	FIntPoint Min = { 0, 0 };
	FIntPoint Max = { 0, 0 };
	FIntPoint Center = { 0, 0 };

	bool operator == (const FStupidQuadTreeNode& Other) const
	{
		return Min == Other.Min && Max == Other.Max;
	}
};

class FStupidQuadTreeAllocator
{
public:
	FStupidQuadTreeAllocator() {};

	int32 GetMinNodeSize() { return MinNodeSize; };
	int32 GetMaxNodeSize() { return MaxNodeSize; };

	void Init(int32 InAtlasResolution);
	FStupidQuadTreeNode AllocateElement(int32 TargetSize);
	void ReleaseElement(const FStupidQuadTreeNode& FreeNode);

protected:
	FStupidQuadTreeNode GetRootNode();
	void UpdateAllocationInfoFromChild(const FStupidQuadTreeNode& ParentNode);
	FStupidQuadTreeNode AllocateElementRecursive(const FStupidQuadTreeNode& CurLevelNode, int32 TargetSize);
	void ReleaseElementRecursive(const FStupidQuadTreeNode& CurLevelNode, const FStupidQuadTreeNode& FreeNode);

	TArray<FNodeAllocationInfo> NodeAllocationInfos;
	int32 AtlasResolution = 0;
	const int32 MinNodeSize = 16;
	const int32 MaxNodeSize = 128;
};
