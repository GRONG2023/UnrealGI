// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ScreenPass.h"

enum class EPostProcessTestQuality : uint8
{
	// Single filtered sample (2x2 tap).
	Low,

	// Four filtered samples (4x4 tap).
	High,

	MAX
};

struct PostProcessTestInput {
	FScreenPassRenderTarget OverrideOutput;
	FScreenPassTexture inputTexture;
};

struct PostProcessTestOutPut {
	FScreenPassTexture outputTexture;
};

FScreenPassTexture AddPostProcessTestPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, const PostProcessTestInput& Inputs);