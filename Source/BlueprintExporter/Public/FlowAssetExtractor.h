#pragma once

#include "CoreMinimal.h"
#include "BlueprintExporterTypes.h"

class UFlowAsset;
class UFlowNode;

class FFlowAssetExtractor
{
public:
	FExportedBlueprint Extract(UFlowAsset* FlowAsset);

private:
	FExportedNode ExtractNode(UFlowNode* Node, const TMap<FGuid, UFlowNode*>& AllNodes);
	FExportedPin ExtractFlowPin(const struct FFlowPin& FlowPin, const FString& Direction,
		UFlowNode* OwnerNode, const TMap<FGuid, UFlowNode*>& AllNodes);

	static FString GetNodeDisplayName(UFlowNode* Node);
	static FString GetNodeClassName(UFlowNode* Node);

	void ExtractNodeProperties(UFlowNode* Node, FExportedNode& OutNode);
	static FString ExportPropertyValue(FProperty* Prop, const void* ValuePtr);
	static bool IsBaseFlowNodeProperty(const FString& PropName);
};
