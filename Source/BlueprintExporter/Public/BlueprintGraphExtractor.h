#pragma once

#include "CoreMinimal.h"
#include "BlueprintExporterTypes.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

using FConfigExtractFunc = void(*)(UObject* CDO, UObject* ParentCDO, TArray<TPair<FString, FString>>& OutConfig);

struct FConfigExtractorEntry
{
	UClass* ParentClass;
	const TCHAR* ConfigType;
	FConfigExtractFunc ExtractFunc;
};

class FBlueprintGraphExtractor
{
public:
	FExportedBlueprint Extract(UBlueprint* Blueprint);

	FExportedGraph ExtractSelectedNodes(
		const TSet<UEdGraphNode*>& SelectedNodes,
		const UEdGraph* OwningGraph);

private:
	FExportedGraph ExtractGraph(UEdGraph* Graph, const FString& GraphType);
	FExportedNode ExtractNode(UEdGraphNode* Node, const FString& GraphName);
	void ExtractNodeProperties(UEdGraphNode* Node, FExportedNode& OutNode);
	FExportedPin ExtractPin(UEdGraphPin* Pin);

	static FString ResolveVariableType(const FEdGraphPinType& PinType);

	// Generic CDO diff: exports non-default properties as flattened dot-notation
	static void ExtractCDOProperties(
		UObject* CDO, UObject* ParentCDO,
		const TSet<FName>& BlueprintVarNames,
		TArray<TPair<FString, FString>>& OutProperties);

	static void FlattenProperty(
		FProperty* Prop, const void* ValuePtr, const void* DefaultPtr,
		const FString& Prefix,
		TArray<TPair<FString, FString>>& OutProperties);

	static FString ExportLeafValue(FProperty* Prop, const void* ValuePtr);
};
