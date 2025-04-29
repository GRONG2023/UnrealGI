#include "StupidAllocator.h"

#define NODE_INDEX_ROOT (0)
#define INVALID_NODE (FStupidQuadTreeNode());

bool IsNodeValid(const FStupidQuadTreeNode& InNode)
{
	return InNode.Size > 0;
}

void FStupidQuadTreeAllocator::Init(int32 InAtlasResolution)
{
	if (AtlasResolution == 0)
	{
		AtlasResolution = InAtlasResolution;

		// 64 + 16 + 4 + 1
		int32 NumNodes = 0;
		const int32 MinLevelNodeNum = FMath::Pow(AtlasResolution / MinNodeSize, 2);
		for (int32 i = MinLevelNodeNum; i >= 1; i /= 4)
		{
			NumNodes += i;
		}

		NodeAllocationInfos.SetNum(NumNodes);
	}

	// @TODO: change size
}

void FStupidQuadTreeAllocator::ReleaseElement(const FStupidQuadTreeNode& FreeNode)
{
	// invalid node
	if (FreeNode.Size == 0)
	{
		return;
	}

	FStupidQuadTreeNode RootNode = GetRootNode();
	ReleaseElementRecursive(RootNode, FreeNode);
}

FStupidQuadTreeNode FStupidQuadTreeAllocator::AllocateElement(int32 TargetSize)
{
	FStupidQuadTreeNode RootNode = GetRootNode();

	FStupidQuadTreeNode Result = AllocateElementRecursive(RootNode, TargetSize);
	return Result;
}

FStupidQuadTreeNode FStupidQuadTreeAllocator::GetRootNode()
{
	FStupidQuadTreeNode RootNode;
	RootNode.Index = NODE_INDEX_ROOT;
	RootNode.Size = AtlasResolution;
	RootNode.Min = FIntPoint(0, 0);
	RootNode.Max = FIntPoint(AtlasResolution, AtlasResolution);
	RootNode.Center = (RootNode.Max + RootNode.Min) / 2;

	return RootNode;
}

TArray<FStupidQuadTreeNode> GetChildNodes(const FStupidQuadTreeNode& ParentNode)
{
	const int32 HalfSize = ParentNode.Size / 2;
	const int32 NextLevelNodeIndexBase = ParentNode.Index * 4 + 1;

	FStupidQuadTreeNode TopLeft;
	TopLeft.Index = NextLevelNodeIndexBase + TC_TopLeft;
	TopLeft.Size = HalfSize;
	TopLeft.Min = ParentNode.Min + FIntPoint(0, 0) * HalfSize;
	TopLeft.Max = ParentNode.Min + FIntPoint(1, 1) * HalfSize;
	TopLeft.Center = (TopLeft.Max + TopLeft.Min) / 2;

	FStupidQuadTreeNode TopRight;
	TopRight.Index = NextLevelNodeIndexBase + TC_TopRight;
	TopRight.Size = HalfSize;
	TopRight.Min = ParentNode.Min + FIntPoint(1, 0) * HalfSize;
	TopRight.Max = ParentNode.Min + FIntPoint(2, 1) * HalfSize;
	TopRight.Center = (TopRight.Max + TopRight.Min) / 2;

	FStupidQuadTreeNode BottomLeft;
	BottomLeft.Index = NextLevelNodeIndexBase + TC_BottomLeft;
	BottomLeft.Size = HalfSize;
	BottomLeft.Min = ParentNode.Min + FIntPoint(0, 1) * HalfSize;
	BottomLeft.Max = ParentNode.Min + FIntPoint(1, 2) * HalfSize;
	BottomLeft.Center = (BottomLeft.Max + BottomLeft.Min) / 2;

	FStupidQuadTreeNode BottomRight;
	BottomRight.Index = NextLevelNodeIndexBase + TC_BottomRight;
	BottomRight.Size = HalfSize;
	BottomRight.Min = ParentNode.Min + FIntPoint(1, 1) * HalfSize;
	BottomRight.Max = ParentNode.Min + FIntPoint(2, 2) * HalfSize;
	BottomRight.Center = (BottomRight.Max + BottomRight.Min) / 2;

	TArray<FStupidQuadTreeNode> Result;
	Result.Add(TopLeft);
	Result.Add(TopRight);
	Result.Add(BottomLeft);
	Result.Add(BottomRight);
	return Result;
}

void FStupidQuadTreeAllocator::UpdateAllocationInfoFromChild(const FStupidQuadTreeNode& ParentNode)
{
	FNodeAllocationInfo& Parent = NodeAllocationInfos[ParentNode.Index];

	const int32 NextLevelNodeIndexBase = ParentNode.Index * 4 + 1;
	const FNodeAllocationInfo& Child0 = NodeAllocationInfos[NextLevelNodeIndexBase + 0];
	const FNodeAllocationInfo& Child1 = NodeAllocationInfos[NextLevelNodeIndexBase + 1];
	const FNodeAllocationInfo& Child2 = NodeAllocationInfos[NextLevelNodeIndexBase + 2];
	const FNodeAllocationInfo& Child3 = NodeAllocationInfos[NextLevelNodeIndexBase + 3];

	bool IsAllChildsEmpty = Child0.IsEmpty && Child1.IsEmpty && Child2.IsEmpty && Child3.IsEmpty;
	bool IsAllChildsFull = Child0.IsFull && Child1.IsFull && Child2.IsFull && Child3.IsFull;

	Parent.IsEmpty = IsAllChildsEmpty;
	Parent.IsFull = IsAllChildsFull;
}

FStupidQuadTreeNode FStupidQuadTreeAllocator::AllocateElementRecursive(const FStupidQuadTreeNode& CurLevelNode, int32 TargetSize)
{
	FNodeAllocationInfo& NodeAllocationInfo = NodeAllocationInfos[CurLevelNode.Index];

	if (NodeAllocationInfo.IsFull)
	{
		return INVALID_NODE;
	}

	// allocate self
	if (CurLevelNode.Size == TargetSize)
	{
		if (NodeAllocationInfo.IsEmpty)
		{
			NodeAllocationInfo.IsEmpty = false;
			NodeAllocationInfo.IsFull = true;
			return CurLevelNode;
		}
		return INVALID_NODE;
	}

	// try allocate from childs
	TArray<FStupidQuadTreeNode> ChildNodes = GetChildNodes(CurLevelNode);
	for (FStupidQuadTreeNode& ChildNode : ChildNodes)
	{
		FStupidQuadTreeNode Result = AllocateElementRecursive(ChildNode, TargetSize);
		
		// child allocate fail
		if (!IsNodeValid(Result))
		{
			continue;
		}

		// if success, mark current node 's allocation flag
		UpdateAllocationInfoFromChild(CurLevelNode);
		return Result;
	}

	return INVALID_NODE;
}

void FStupidQuadTreeAllocator::ReleaseElementRecursive(const FStupidQuadTreeNode& CurLevelNode, const FStupidQuadTreeNode& FreeNode)
{
	FNodeAllocationInfo& NodeAllocationInfo = NodeAllocationInfos[CurLevelNode.Index];

	// release self
	if (FreeNode == CurLevelNode)
	{
		check(NodeAllocationInfo.IsFull);

		NodeAllocationInfo.IsEmpty = true;
		NodeAllocationInfo.IsFull = false;
		return;
	}

	// try release to childs
	int32 ChildId = 0;
	ChildId += (FreeNode.Center.X > CurLevelNode.Center.X) ? (0x01 << 0) : 0;	// left or right (爆了)
	ChildId += (FreeNode.Center.Y > CurLevelNode.Center.Y) ? (0x01 << 1) : 0;	// top or bottom

	TArray<FStupidQuadTreeNode> ChildNodes = GetChildNodes(CurLevelNode);
	const FStupidQuadTreeNode& ChildNode = ChildNodes[ChildId];

	ReleaseElementRecursive(ChildNode, FreeNode);

	// update allocate flags
	UpdateAllocationInfoFromChild(CurLevelNode);
}
