#pragma once

#include "CoreMinimal.h"
#include "BlueprintExporterTypes.h"

class FBlueprintTextFormatter
{
public:
	FString Format(const FExportedBlueprint& Blueprint);
	FString FormatSelectedNodes(const FExportedGraph& Graph, const FString& BlueprintName);
	FString FormatGraphOnly(const FExportedGraph& Graph);
	FString FormatSummary(const FExportedBlueprint& Blueprint);
	FString FormatCompactBlueprint(const FExportedBlueprint& Blueprint);

private:
	FString FormatVariables(const TArray<FExportedVariable>& Variables);
	FString FormatGraph(const FExportedGraph& Graph);
	FString FormatNode(const FExportedNode& Node, const TMap<FString, const FExportedNode*>& NodeMap);
	FString FormatPin(const FExportedPin& Pin, const TMap<FString, const FExportedNode*>& NodeMap);

	FString GetReadableType(const FString& NodeClass) const;
	FString GetSemanticTitle(const FExportedNode& Node) const;
	static FString GetShortNodeId(const FString& NodeName);

	TArray<FExportedNode> TopologicalSort(const TArray<FExportedNode>& Nodes);
	TArray<FExportedPin> GetMeaningfulPins(const FExportedNode& Node);

	bool IsTrivialDefault(const FString& Value);

	TPair<FString, FString> ResolveRerouteChain(const FString& NodeName, const FString& OriginalPinName, const TMap<FString, const FExportedNode*>& NodeMap);
	TPair<FString, FString> ResolveRerouteChainBackward(const FString& NodeName, const FString& OriginalPinName, const TMap<FString, const FExportedNode*>& NodeMap);

	FString FormatExecutionFlow(const TArray<FExportedNode>& Nodes, const TMap<FString, const FExportedNode*>& NodeMap);

	// Compact mode
	FString FormatCompactGraph(const FExportedGraph& Graph);
	FString ExtractFunctionSignature(const FExportedGraph& Graph);
	FString FormatCompactChain(
		const FString& NodeName,
		const TMap<FString, const FExportedNode*>& NodeMap,
		int32 Indent,
		TSet<FString>& Visited);
	bool HasExecPins(const FExportedNode& Node) const;

	TSet<FString> SelectionContext; // Empty = full export; non-empty = selection mode
};
