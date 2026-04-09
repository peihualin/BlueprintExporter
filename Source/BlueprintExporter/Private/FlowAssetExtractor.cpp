#include "FlowAssetExtractor.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/FlowPin.h"
#include "Nodes/Graph/FlowNode_SubGraph.h"
#include "GameplayTagContainer.h"
#include "Misc/PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlowAssetExtractor, Log, All);

static const TSet<FString> GBaseFlowNodeProperties = {
	TEXT("InputPins"), TEXT("OutputPins"), TEXT("Connections"),
	TEXT("NodeGuid"), TEXT("SignalMode"), TEXT("AllowedSignalModes"),
	TEXT("ActivationState"), TEXT("bPreloaded"),
	TEXT("AddOns"), TEXT("GraphNode"),
	TEXT("bDisplayNodeTitleWithoutPrefix"), TEXT("bCanDelete"), TEXT("bCanDuplicate"),
	TEXT("bNodeDeprecated"), TEXT("ReplacedBy"),
	TEXT("Category"), TEXT("NodeDisplayStyle"), TEXT("NodeStyle"), TEXT("NodeColor"),
	TEXT("DevNodeConfigText"),
	TEXT("AllowedAssetClasses"), TEXT("DeniedAssetClasses"),
	TEXT("PinNameToBoundPropertyNameMap"),
	TEXT("AutoInputDataPins"), TEXT("AutoOutputDataPins"),
	TEXT("OnReconstructionRequested"), TEXT("ValidationLog"),
};

FExportedBlueprint FFlowAssetExtractor::Extract(UFlowAsset* FlowAsset)
{
	FExportedBlueprint Result;
	if (!FlowAsset)
	{
		return Result;
	}

	Result.BlueprintName = FlowAsset->GetName();
	Result.ConfigType = TEXT("FlowAsset");

	// Asset path
	UPackage* Pkg = FlowAsset->GetOutermost();
	if (Pkg)
	{
		Result.AssetPath = Pkg->GetName();
	}

	// ExpectedOwnerClass → stored in ParentClass field for display
	if (UClass* OwnerClass = FlowAsset->GetExpectedOwnerClass())
	{
		FString ClassName = OwnerClass->GetName();
		ClassName.RemoveFromEnd(TEXT("_C"));
		Result.ParentClass = ClassName;
	}

	// FlowAsset-level properties as CDO config
	if (FlowAsset->bWorldBound)
	{
		Result.CDOProperties.Add(TPair<FString, FString>(TEXT("WorldBound"), TEXT("true")));
	}

	const TMap<FGuid, UFlowNode*>& Nodes = FlowAsset->GetNodes();

	// Single graph containing all flow nodes
	FExportedGraph Graph;
	Graph.GraphName = TEXT("Flow Graph");
	Graph.GraphType = TEXT("FlowGraph");

	for (const auto& Pair : Nodes)
	{
		UFlowNode* Node = Pair.Value;
		if (!Node)
		{
			continue;
		}

		FExportedNode ExportedNode = ExtractNode(Node, Nodes);
		Graph.Nodes.Add(MoveTemp(ExportedNode));
	}

	if (Graph.Nodes.Num() > 0)
	{
		Result.Graphs.Add(MoveTemp(Graph));
	}

	return Result;
}

FExportedNode FFlowAssetExtractor::ExtractNode(UFlowNode* Node, const TMap<FGuid, UFlowNode*>& AllNodes)
{
	FExportedNode Result;

	Result.NodeName = Node->GetGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Result.NodeClass = GetNodeClassName(Node);
	Result.GraphName = TEXT("Flow Graph");

	// Display name as primary property
	FString DisplayName = GetNodeDisplayName(Node);
	if (!DisplayName.IsEmpty())
	{
		Result.Properties.Add(TPair<FString, FString>(TEXT("DisplayName"), DisplayName));
	}

	// SubGraph: record asset reference
	if (UFlowNode_SubGraph* SubGraphNode = Cast<UFlowNode_SubGraph>(Node))
	{
		UObject* AssetToEdit = nullptr;
#if WITH_EDITOR
		AssetToEdit = SubGraphNode->GetAssetToEdit();
#endif
		if (AssetToEdit)
		{
			FString AssetName = AssetToEdit->GetName();
			Result.Properties.Add(TPair<FString, FString>(TEXT("SubGraphAsset"), AssetName));
		}
	}

	// Extract user-defined properties via reflection
	ExtractNodeProperties(Node, Result);

	// Extract input pins
	const TArray<FFlowPin>& InputPins = Node->GetInputPins();
	for (const FFlowPin& FlowPin : InputPins)
	{
		FExportedPin Pin = ExtractFlowPin(FlowPin, TEXT("Input"), Node, AllNodes);
		Result.Pins.Add(MoveTemp(Pin));
	}

	// Extract output pins with connections
	const TArray<FFlowPin>& OutputPins = Node->GetOutputPins();
	for (const FFlowPin& FlowPin : OutputPins)
	{
		FExportedPin Pin = ExtractFlowPin(FlowPin, TEXT("Output"), Node, AllNodes);
		Result.Pins.Add(MoveTemp(Pin));
	}

	return Result;
}

FExportedPin FFlowAssetExtractor::ExtractFlowPin(
	const FFlowPin& FlowPin, const FString& Direction,
	UFlowNode* OwnerNode, const TMap<FGuid, UFlowNode*>& AllNodes)
{
	FExportedPin Result;

	// Pin name: prefer friendly name, fall back to PinName
	if (!FlowPin.PinFriendlyName.IsEmpty())
	{
		Result.Name = FlowPin.PinFriendlyName.ToString();
	}
	else
	{
		Result.Name = FlowPin.PinName.ToString();
	}

	Result.Direction = Direction;

	// Pin category from PinType
	const FName PinCategory = FFlowPin::GetPinCategoryFromPinType(FlowPin.GetPinType());
	Result.Category = PinCategory.ToString();

	// SubType from PinSubCategoryObject
	if (FlowPin.GetPinSubCategoryObject().IsValid())
	{
		FString SubTypeName = FlowPin.GetPinSubCategoryObject()->GetName();
		SubTypeName.RemoveFromEnd(TEXT("_C"));
		Result.SubType = SubTypeName;
	}

	// Connections: only output pins have connections in the FlowNode model
	if (Direction == TEXT("Output"))
	{
		FConnectedPin Connection = OwnerNode->GetConnection(FlowPin.PinName);
		if (Connection.NodeGuid.IsValid())
		{
			UFlowNode* TargetNode = AllNodes.FindRef(Connection.NodeGuid);
			if (TargetNode)
			{
				FString TargetNodeName = Connection.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
				FString TargetPinName = Connection.PinName.ToString();
				Result.LinkedTo.Add(TPair<FString, FString>(TargetNodeName, TargetPinName));
			}
		}
	}

	return Result;
}

FString FFlowAssetExtractor::GetNodeDisplayName(UFlowNode* Node)
{
	if (!Node)
	{
		return FString();
	}

#if WITH_EDITOR
	FText NodeTitle = Node->GetNodeTitle();
	if (!NodeTitle.IsEmpty())
	{
		return NodeTitle.ToString();
	}
#endif

	// Fallback: try UCLASS DisplayName metadata
	UClass* NodeClass = Node->GetClass();
	if (NodeClass)
	{
		const FString& DisplayName = NodeClass->GetMetaData(TEXT("DisplayName"));
		if (!DisplayName.IsEmpty())
		{
			return DisplayName;
		}
	}

	// Last resort: strip prefix from class name and insert spaces
	FString ClassName = NodeClass ? NodeClass->GetName() : TEXT("Unknown");
	ClassName.RemoveFromStart(TEXT("FlowNode_"));
	ClassName.RemoveFromStart(TEXT("RFlowNode_"));

	FString Result;
	for (int32 i = 0; i < ClassName.Len(); ++i)
	{
		TCHAR Ch = ClassName[i];
		if (i > 0 && FChar::IsUpper(Ch) && !FChar::IsUpper(ClassName[i - 1]))
		{
			Result.AppendChar(TEXT(' '));
		}
		Result.AppendChar(Ch);
	}

	return Result;
}

FString FFlowAssetExtractor::GetNodeClassName(UFlowNode* Node)
{
	if (!Node)
	{
		return TEXT("Unknown");
	}

	// UClass::GetName() already returns the name without U/A prefix
	return Node->GetClass()->GetName();
}

void FFlowAssetExtractor::ExtractNodeProperties(UFlowNode* Node, FExportedNode& OutNode)
{
	if (!Node)
	{
		return;
	}

	UClass* NodeClass = Node->GetClass();
	UObject* CDO = NodeClass->GetDefaultObject();

	for (TFieldIterator<FProperty> PropIt(NodeClass); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;

		// Skip properties from base FlowNode classes
		if (IsBaseFlowNodeProperty(Prop->GetName()))
		{
			continue;
		}

		// Only export properties visible in the editor
		bool bIsEditable = Prop->HasAnyPropertyFlags(
			CPF_Edit | CPF_EditConst | CPF_BlueprintVisible);
		if (!bIsEditable)
		{
			continue;
		}

		// Skip transient runtime-only properties
		if (Prop->HasAnyPropertyFlags(CPF_Transient))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
		const void* DefaultPtr = CDO ? Prop->ContainerPtrToValuePtr<void>(CDO) : nullptr;

		// Skip if value equals default
		if (DefaultPtr && Prop->Identical(ValuePtr, DefaultPtr))
		{
			continue;
		}

		FString ValueStr = ExportPropertyValue(Prop, ValuePtr);
		if (!ValueStr.IsEmpty())
		{
			OutNode.Properties.Add(TPair<FString, FString>(Prop->GetName(), ValueStr));
		}
	}
}

FString FFlowAssetExtractor::ExportPropertyValue(FProperty* Prop, const void* ValuePtr)
{
	if (!Prop || !ValuePtr)
	{
		return FString();
	}

	FString Result;

	// Handle soft object references
	if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
	{
		const FSoftObjectPtr& SoftPtr = *static_cast<const FSoftObjectPtr*>(ValuePtr);
		if (!SoftPtr.IsNull())
		{
			Result = SoftPtr.GetAssetName();
		}
		return Result;
	}

	// Handle class references (must be before FObjectPropertyBase since FClassProperty inherits from it)
	if (const FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		UClass* ClassVal = Cast<UClass>(ClassProp->GetObjectPropertyValue(ValuePtr));
		if (ClassVal)
		{
			Result = ClassVal->GetName();
			Result.RemoveFromEnd(TEXT("_C"));
		}
		return Result;
	}

	// Handle object references
	if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (Obj)
		{
			Result = Obj->GetName();
		}
		return Result;
	}

	// Handle arrays
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
		if (ArrayHelper.Num() == 0)
		{
			return FString();
		}

		TArray<FString> Elements;
		for (int32 i = 0; i < ArrayHelper.Num(); ++i)
		{
			FString ElemStr = ExportPropertyValue(ArrayProp->Inner, ArrayHelper.GetRawPtr(i));
			if (!ElemStr.IsEmpty())
			{
				Elements.Add(ElemStr);
			}
		}
		return TEXT("[") + FString::Join(Elements, TEXT(", ")) + TEXT("]");
	}

	// Handle structs: export as compact key=value
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		// GameplayTag special handling
		if (StructProp->Struct == FGameplayTag::StaticStruct())
		{
			const FGameplayTag* Tag = static_cast<const FGameplayTag*>(ValuePtr);
			return Tag->IsValid() ? Tag->ToString() : FString();
		}

		// GameplayTagContainer special handling
		if (StructProp->Struct == FGameplayTagContainer::StaticStruct())
		{
			const FGameplayTagContainer* Container = static_cast<const FGameplayTagContainer*>(ValuePtr);
			if (Container->Num() == 0)
			{
				return FString();
			}
			return Container->ToStringSimple();
		}

		// Generic struct: use ExportText
		FString StructText;
		StructProp->ExportTextItem_Direct(StructText, ValuePtr, nullptr, nullptr, PPF_None);
		// Truncate very long struct text
		if (StructText.Len() > 200)
		{
			StructText = StructText.Left(197) + TEXT("...");
		}
		return StructText;
	}

	// Handle enums
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		const UEnum* Enum = EnumProp->GetEnum();
		const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
		int64 Val = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
		FText DisplayName = Enum->GetDisplayNameTextByValue(Val);
		if (!DisplayName.IsEmpty())
		{
			return DisplayName.ToString();
		}
		return Enum->GetNameStringByValue(Val);
	}

	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			uint8 Val = *static_cast<const uint8*>(ValuePtr);
			FText DisplayName = ByteProp->Enum->GetDisplayNameTextByValue(Val);
			if (!DisplayName.IsEmpty())
			{
				return DisplayName.ToString();
			}
			return ByteProp->Enum->GetNameStringByValue(Val);
		}
	}

	// Fallback: use ExportTextItem_Direct
	Prop->ExportTextItem_Direct(Result, ValuePtr, nullptr, nullptr, PPF_None);

	return Result;
}

bool FFlowAssetExtractor::IsBaseFlowNodeProperty(const FString& PropName)
{
	return GBaseFlowNodeProperties.Contains(PropName);
}
