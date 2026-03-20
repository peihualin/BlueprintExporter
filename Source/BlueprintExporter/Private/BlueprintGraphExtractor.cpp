#include "BlueprintGraphExtractor.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"

#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"

#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTypes.h"
#include "GameplayEffect.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayModMagnitudeCalculation.h"
#include "GameplayEffectComponents/AbilitiesGameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"
#include "GameplayEffectComponents/RemoveOtherGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayTagContainer.h"
#include "UObject/UnrealType.h"

static const TArray<FConfigExtractorEntry>& GetConfigExtractorRegistry();
static void ExtractGameplayEffectConfigRaw(UObject* CDO, TArray<TPair<FString, FString>>& Out);
static void ExtractGameplayAbilityConfigRaw(UObject* CDO, TArray<TPair<FString, FString>>& Out);

static FString StripGeneratedClassSuffix(FString Name)
{
	if (Name.EndsWith(TEXT("_C")))
	{
		Name.LeftChopInline(2);
	}
	return Name;
}

template<typename TClass>
static FString ExportClassName(const TSubclassOf<TClass>& Class)
{
	return StripGeneratedClassSuffix(GetNameSafe(Class.Get()));
}

static FString GetTopLevelConfigKey(const FString& Key)
{
	int32 DotIndex = INDEX_NONE;
	int32 BracketIndex = INDEX_NONE;
	Key.FindChar(TEXT('.'), DotIndex);
	Key.FindChar(TEXT('['), BracketIndex);

	int32 EndIndex = INDEX_NONE;
	if (DotIndex != INDEX_NONE && BracketIndex != INDEX_NONE)
	{
		EndIndex = FMath::Min(DotIndex, BracketIndex);
	}
	else if (DotIndex != INDEX_NONE)
	{
		EndIndex = DotIndex;
	}
	else if (BracketIndex != INDEX_NONE)
	{
		EndIndex = BracketIndex;
	}

	return EndIndex == INDEX_NONE ? Key : Key.Left(EndIndex);
}

template<typename StructType>
static const StructType* GetStructFieldPtr(const void* Container, const UScriptStruct* OwnerStruct, const TCHAR* FieldName)
{
	const FStructProperty* StructProp = FindFProperty<FStructProperty>(OwnerStruct, FieldName);
	if (!StructProp)
	{
		return nullptr;
	}

	return StructProp->ContainerPtrToValuePtr<StructType>(Container);
}

template<typename StructType>
static const StructType* GetObjectStructFieldPtr(const UObject* Object, const TCHAR* FieldName)
{
	const FStructProperty* StructProp = FindFProperty<FStructProperty>(Object->GetClass(), FieldName);
	if (!StructProp)
	{
		return nullptr;
	}

	return StructProp->ContainerPtrToValuePtr<StructType>(Object);
}

template<typename ElementType>
static const TArray<ElementType>* GetObjectArrayFieldPtr(const UObject* Object, const TCHAR* FieldName)
{
	const FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(Object->GetClass(), FieldName);
	if (!ArrayProp)
	{
		return nullptr;
	}

	return ArrayProp->ContainerPtrToValuePtr<TArray<ElementType>>(Object);
}

static bool GetObjectBoolField(const UObject* Object, const TCHAR* FieldName)
{
	const FBoolProperty* BoolProp = FindFProperty<FBoolProperty>(Object->GetClass(), FieldName);
	return BoolProp ? BoolProp->GetPropertyValue_InContainer(Object) : false;
}

static FString GetObjectClassFieldName(const UObject* Object, const TCHAR* FieldName)
{
	const FClassProperty* ClassProp = FindFProperty<FClassProperty>(Object->GetClass(), FieldName);
	if (!ClassProp)
	{
		return FString();
	}

	return StripGeneratedClassSuffix(GetNameSafe(ClassProp->GetPropertyValue_InContainer(Object)));
}

static void AppendConfigDiff(
	const TArray<TPair<FString, FString>>& CurrentConfig,
	const TArray<TPair<FString, FString>>& ParentConfig,
	TArray<TPair<FString, FString>>& OutConfig)
{
	TMap<FString, FString> ParentValues;
	for (const TPair<FString, FString>& Pair : ParentConfig)
	{
		ParentValues.Add(Pair.Key, Pair.Value);
	}

	for (const TPair<FString, FString>& Pair : CurrentConfig)
	{
		const FString* ParentValue = ParentValues.Find(Pair.Key);
		if (!ParentValue || *ParentValue != Pair.Value)
		{
			OutConfig.Add(Pair);
		}
	}
}

FExportedBlueprint FBlueprintGraphExtractor::Extract(UBlueprint* Blueprint)
{
	FExportedBlueprint Result;

	if (!Blueprint)
	{
		return Result;
	}

	Result.BlueprintName = Blueprint->GetName();
	Result.ConfigType = TEXT("Generic");

	if (Blueprint->ParentClass)
	{
		FString ParentName = Blueprint->ParentClass->GetName();
		// Remove _C suffix from blueprint generated classes
		if (ParentName.EndsWith(TEXT("_C")))
		{
			ParentName.LeftChopInline(2);
		}
		Result.ParentClass = ParentName;
	}

	// Variables
	TSet<FName> BPVarNames;
	for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
	{
		BPVarNames.Add(VarDesc.VarName);

		FExportedVariable Var;
		Var.Name = VarDesc.VarName.ToString();
		Var.Type = ResolveVariableType(VarDesc.VarType);

		// Container type
		switch (VarDesc.VarType.ContainerType)
		{
		case EPinContainerType::Array:
			Var.ContainerType = TEXT("Array");
			break;
		case EPinContainerType::Set:
			Var.ContainerType = TEXT("Set");
			break;
		case EPinContainerType::Map:
			Var.ContainerType = TEXT("Map");
			break;
		default:
			break;
		}

		// Default value with enum resolution
		if (!VarDesc.DefaultValue.IsEmpty())
		{
			Var.DefaultValue = VarDesc.DefaultValue;

			if (VarDesc.VarType.PinSubCategoryObject.IsValid())
			{
				UEnum* EnumPtr = Cast<UEnum>(VarDesc.VarType.PinSubCategoryObject.Get());
				if (EnumPtr)
				{
					int64 EnumValue = EnumPtr->GetValueByNameString(Var.DefaultValue);
					if (EnumValue != INDEX_NONE)
					{
						FText DisplayName = EnumPtr->GetDisplayNameTextByValue(EnumValue);
						if (!DisplayName.IsEmpty())
						{
							Var.DefaultValue = DisplayName.ToString();
						}
					}
				}
			}
		}

		// Fallback: read default value from CDO when FBPVariableDescription::DefaultValue is empty
		if (Var.DefaultValue.IsEmpty() && Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
			if (CDO)
			{
				FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(VarDesc.VarName);
				if (Prop)
				{
					FString ExportedValue;
					Prop->ExportText_InContainer(0, ExportedValue, CDO, nullptr, nullptr, PPF_None);
					if (!ExportedValue.IsEmpty())
					{
						Var.DefaultValue = ExportedValue;

						// Resolve enum display names from exported string
						// ExportText may produce "E_AI_State::NewEnumerator0" — strip prefix first
						UEnum* EnumPtr = Cast<UEnum>(VarDesc.VarType.PinSubCategoryObject.Get());
						if (EnumPtr)
						{
							FString RawName = Var.DefaultValue;
							int32 ColonIdx;
							if (RawName.FindLastChar(':', ColonIdx))
							{
								RawName = RawName.Mid(ColonIdx + 1);
							}
							int64 EnumValue = EnumPtr->GetValueByNameString(RawName);
							if (EnumValue != INDEX_NONE)
							{
								FText DisplayName = EnumPtr->GetDisplayNameTextByValue(EnumValue);
								if (!DisplayName.IsEmpty())
								{
									Var.DefaultValue = DisplayName.ToString();
								}
							}
						}
					}
				}
			}
		}

		// Flags from PropertyFlags
		if (VarDesc.PropertyFlags & CPF_Edit)
		{
			if (VarDesc.PropertyFlags & CPF_DisableEditOnInstance)
			{
				Var.Flags.Add(TEXT("EditDefaultsOnly"));
			}
			else
			{
				Var.Flags.Add(TEXT("EditAnywhere"));
			}
		}

		if (VarDesc.PropertyFlags & CPF_BlueprintVisible)
		{
			if (VarDesc.PropertyFlags & CPF_BlueprintReadOnly)
			{
				Var.Flags.Add(TEXT("BlueprintReadOnly"));
			}
			else
			{
				Var.Flags.Add(TEXT("BlueprintReadWrite"));
			}
		}

		if (VarDesc.PropertyFlags & CPF_Net)
		{
			Var.Flags.Add(TEXT("Replicated"));
		}

		Result.Variables.Add(MoveTemp(Var));
	}

	// Helper: add graph only if it has nodes after filtering
	auto AddGraphIfNonEmpty = [&](UEdGraph* Graph, const FString& Type)
	{
		FExportedGraph ExGraph = ExtractGraph(Graph, Type);
		if (ExGraph.Nodes.Num() > 0)
		{
			Result.Graphs.Add(MoveTemp(ExGraph));
		}
	};

	// EventGraph(s)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			AddGraphIfNonEmpty(Graph, TEXT("EventGraph"));
		}
	}

	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			AddGraphIfNonEmpty(Graph, TEXT("Function"));
		}
	}

	// Macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
		{
			AddGraphIfNonEmpty(Graph, TEXT("Macro"));
		}
	}

	// Interface graphs
	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		for (UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (Graph)
			{
				AddGraphIfNonEmpty(Graph, TEXT("Interface"));
			}
		}
	}

	// CDO configuration extraction
	if (Blueprint->GeneratedClass)
	{
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
		UObject* ParentCDO = Blueprint->ParentClass
			? Blueprint->ParentClass->GetDefaultObject(false) : nullptr;

		if (CDO && ParentCDO)
		{
			// Try type-specific extractors first
			bool bHandled = false;
			for (const auto& Entry : GetConfigExtractorRegistry())
			{
				if (Blueprint->ParentClass->IsChildOf(Entry.ParentClass))
				{
					Entry.ExtractFunc(CDO, ParentCDO, Result.CDOProperties);
					Result.ConfigType = Entry.ConfigType;

					if (UBlueprint* ParentBlueprint = Cast<UBlueprint>(Blueprint->ParentClass->ClassGeneratedBy))
					{
						Result.ParentConfigSource = ParentBlueprint->GetName();
					}

					bHandled = true;
					break;
				}
			}

			if (bHandled && Result.ConfigType == TEXT("GameplayAbility"))
			{
				TArray<TPair<FString, FString>> GenericProperties;
				ExtractCDOProperties(CDO, ParentCDO, BPVarNames, GenericProperties);

				const TSet<FString> ExcludedTopLevelKeys = {
					TEXT("AbilityTags"),
					TEXT("ReplicationPolicy"),
					TEXT("InstancingPolicy"),
					TEXT("bServerRespectsRemoteAbilityCancellation"),
					TEXT("bRetriggerInstancedAbility"),
					TEXT("bReplicateInputDirectly"),
					TEXT("NetExecutionPolicy"),
					TEXT("NetSecurityPolicy"),
					TEXT("CostGameplayEffectClass"),
					TEXT("CooldownGameplayEffectClass"),
					TEXT("AbilityTriggers"),
					TEXT("CancelAbilitiesWithTag"),
					TEXT("BlockAbilitiesWithTag"),
					TEXT("ActivationOwnedTags"),
					TEXT("ActivationRequiredTags"),
					TEXT("ActivationBlockedTags"),
					TEXT("SourceRequiredTags"),
					TEXT("SourceBlockedTags"),
					TEXT("TargetRequiredTags"),
					TEXT("TargetBlockedTags"),
				};

				for (const TPair<FString, FString>& Pair : GenericProperties)
				{
					if (!ExcludedTopLevelKeys.Contains(GetTopLevelConfigKey(Pair.Key)))
					{
						Result.CDOProperties.Add(Pair);
					}
				}
			}

			// Fallback to generic CDO diff
			if (!bHandled)
			{
				ExtractCDOProperties(CDO, ParentCDO, BPVarNames, Result.CDOProperties);
			}
		}
	}

	return Result;
}

FExportedGraph FBlueprintGraphExtractor::ExtractGraph(UEdGraph* Graph, const FString& GraphType)
{
	FExportedGraph Result;
	Result.GraphName = Graph->GetName();
	Result.GraphType = GraphType;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Skip comment nodes
		if (Cast<UEdGraphNode_Comment>(Node))
		{
			continue;
		}

		Result.Nodes.Add(ExtractNode(Node, Result.GraphName));
	}

	// Filter out stub nodes: unimplemented override events and disconnected function entries
	Result.Nodes.RemoveAll([](const FExportedNode& Node)
	{
		// Unimplemented override events (UE auto-creates these stubs)
		if (Node.NodeClass == TEXT("K2Node_Event"))
		{
			bool bIsOverride = false;
			for (const auto& Prop : Node.Properties)
			{
				if (Prop.Key == TEXT("Override") && Prop.Value == TEXT("true"))
				{
					bIsOverride = true;
					break;
				}
			}
			if (!bIsOverride)
			{
				return false;
			}

			for (const FExportedPin& Pin : Node.Pins)
			{
				if (Pin.Category == TEXT("exec")
					&& Pin.Direction == TEXT("Output")
					&& Pin.LinkedTo.Num() > 0)
				{
					return false;
				}
			}
			return true;
		}

		// Empty function entry nodes (e.g. default UserConstructionScript)
		if (Node.NodeClass == TEXT("K2Node_FunctionEntry"))
		{
			for (const FExportedPin& Pin : Node.Pins)
			{
				if (Pin.Category == TEXT("exec")
					&& Pin.Direction == TEXT("Output")
					&& Pin.LinkedTo.Num() > 0)
				{
					return false;
				}
			}
			return true;
		}

		return false;
	});

	return Result;
}

FExportedNode FBlueprintGraphExtractor::ExtractNode(UEdGraphNode* Node, const FString& GraphName)
{
	FExportedNode Result;
	Result.NodeName = Node->GetName();
	Result.NodeClass = Node->GetClass()->GetName();
	Result.GraphName = GraphName;

	ExtractNodeProperties(Node, Result);

	// Extract subgraph for Composite (collapsed) nodes
	if (UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
	{
		if (CompositeNode->BoundGraph)
		{
			Result.SubGraph = MakeShared<FExportedGraph>(
				ExtractGraph(CompositeNode->BoundGraph, TEXT("Collapsed")));
		}
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Result.Pins.Add(ExtractPin(Pin));
		}
	}

	return Result;
}

void FBlueprintGraphExtractor::ExtractNodeProperties(UEdGraphNode* Node, FExportedNode& OutNode)
{
	// UK2Node_ComponentBoundEvent (must be before UK2Node_Event — it inherits from it)
	if (UK2Node_ComponentBoundEvent* CompEventNode = Cast<UK2Node_ComponentBoundEvent>(Node))
	{
		OutNode.Properties.Emplace(TEXT("ComponentProperty"), CompEventNode->ComponentPropertyName.ToString());
		OutNode.Properties.Emplace(TEXT("DelegateProperty"), CompEventNode->DelegatePropertyName.ToString());
		return;
	}

	// UK2Node_CustomEvent (must be before UK2Node_Event — it inherits from it)
	if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Event"), CustomEventNode->CustomFunctionName.ToString());
		return;
	}

	// UK2Node_Event
	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Event"),
			EventNode->EventReference.GetMemberName().ToString());
		if (EventNode->bOverrideFunction)
		{
			OutNode.Properties.Emplace(TEXT("Override"), TEXT("true"));
		}
		return;
	}

	// UK2Node_CallFunction (also covers UK2Node_CallArrayFunction)
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();

		// If not self context, prefix with parent class
		if (!CallNode->FunctionReference.IsSelfContext())
		{
			UClass* MemberParent = CallNode->FunctionReference.GetMemberParentClass();
			if (MemberParent)
			{
				FString ClassName = MemberParent->GetName();
				FuncName = FString::Printf(TEXT("%s::%s"), *ClassName, *FuncName);
			}
		}

		OutNode.Properties.Emplace(TEXT("Function"), FuncName);
		return;
	}

	// UK2Node_VariableGet / UK2Node_VariableSet (both inherit UK2Node_Variable)
	if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Variable"),
			VarGetNode->VariableReference.GetMemberName().ToString());
		return;
	}

	if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Variable"),
			VarSetNode->VariableReference.GetMemberName().ToString());
		return;
	}

	// UK2Node_DynamicCast
	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		if (CastNode->TargetType)
		{
			FString TargetName = CastNode->TargetType->GetName();
			if (TargetName.EndsWith(TEXT("_C")))
			{
				TargetName.LeftChopInline(2);
			}
			OutNode.Properties.Emplace(TEXT("CastTo"), TargetName);
		}
		return;
	}

	// UK2Node_MacroInstance
	if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		UEdGraph* MacroGraph = MacroNode->GetMacroGraph();
		if (MacroGraph)
		{
			OutNode.Properties.Emplace(TEXT("Macro"), MacroGraph->GetName());
		}
		return;
	}

	// UK2Node_Timeline
	if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Timeline"), TimelineNode->TimelineName.ToString());
		return;
	}

	// UK2Node_SwitchEnum
	if (UK2Node_SwitchEnum* SwitchNode = Cast<UK2Node_SwitchEnum>(Node))
	{
		if (SwitchNode->Enum)
		{
			OutNode.Properties.Emplace(TEXT("Enum"), SwitchNode->Enum->GetName());
		}
		return;
	}

	// UK2Node_Composite
	if (UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
	{
		if (CompositeNode->BoundGraph)
		{
			OutNode.Properties.Emplace(TEXT("Collapsed"), CompositeNode->BoundGraph->GetName());
		}
		return;
	}

	// Other node types: no additional properties needed
}

FExportedPin FBlueprintGraphExtractor::ExtractPin(UEdGraphPin* Pin)
{
	FExportedPin Result;

	// Name: prefer display name, fallback to PinName
	FText DisplayName = Pin->GetDisplayName();
	Result.Name = DisplayName.IsEmpty()
		? Pin->PinName.ToString()
		: DisplayName.ToString();

	// Direction
	Result.Direction = (Pin->Direction == EGPD_Output) ? TEXT("Output") : TEXT("Input");

	// Category
	Result.Category = Pin->PinType.PinCategory.ToString();

	// SubType
	if (Pin->PinType.PinSubCategoryObject.IsValid())
	{
		UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get();
		if (SubObj)
		{
			Result.SubType = SubObj->GetName();
			// Remove _C suffix from blueprint generated classes
			if (Result.SubType.EndsWith(TEXT("_C")))
			{
				Result.SubType.LeftChopInline(2);
			}
		}
	}
	else if (!Pin->PinType.PinSubCategory.IsNone())
	{
		// float, double, etc. via PinSubCategory
		Result.SubType = Pin->PinType.PinSubCategory.ToString();
	}

	// Container type
	switch (Pin->PinType.ContainerType)
	{
	case EPinContainerType::Array:
		Result.ContainerType = TEXT("Array");
		break;
	case EPinContainerType::Set:
		Result.ContainerType = TEXT("Set");
		break;
	case EPinContainerType::Map:
		Result.ContainerType = TEXT("Map");
		break;
	default:
		break;
	}

	// Default value
	if (!Pin->DefaultValue.IsEmpty())
	{
		Result.DefaultValue = Pin->DefaultValue;

		// Strip _C suffix for class/object type pins
		const FName& Cat = Pin->PinType.PinCategory;
		if ((Cat == TEXT("class") || Cat == TEXT("object"))
			&& Result.DefaultValue.EndsWith(TEXT("_C")))
		{
			Result.DefaultValue.LeftChopInline(2);
		}

		// Resolve enum display names (e.g. "NewEnumerator0" -> "Idle")
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			UEnum* EnumPtr = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
			if (EnumPtr)
			{
				int64 EnumValue = EnumPtr->GetValueByNameString(Result.DefaultValue);
				if (EnumValue != INDEX_NONE)
				{
					FText Name = EnumPtr->GetDisplayNameTextByValue(EnumValue);
					if (!Name.IsEmpty())
					{
						Result.DefaultValue = Name.ToString();
					}
				}
			}
		}
	}
	else if (Pin->DefaultObject != nullptr)
	{
		Result.DefaultValue = Pin->DefaultObject->GetName();
		if (Result.DefaultValue.EndsWith(TEXT("_C")))
		{
			Result.DefaultValue.LeftChopInline(2);
		}
	}

	// Hidden
	Result.bIsHidden = Pin->bHidden;

	// Connections
	for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (!LinkedPin || !LinkedPin->GetOwningNode())
		{
			continue;
		}

		FString TargetNodeName = LinkedPin->GetOwningNode()->GetName();
		FText TargetDisplayName = LinkedPin->GetDisplayName();
		FString TargetPinName = TargetDisplayName.IsEmpty()
			? LinkedPin->PinName.ToString()
			: TargetDisplayName.ToString();

		Result.LinkedTo.Add(TPair<FString, FString>(TargetNodeName, TargetPinName));
	}

	return Result;
}

FExportedGraph FBlueprintGraphExtractor::ExtractSelectedNodes(
	const TSet<UEdGraphNode*>& SelectedNodes,
	const UEdGraph* OwningGraph)
{
	FExportedGraph Result;
	Result.GraphName = OwningGraph ? OwningGraph->GetName() : TEXT("Unknown");
	Result.GraphType = TEXT("Selection");

	for (UEdGraphNode* Node : SelectedNodes)
	{
		if (!Node)
		{
			continue;
		}

		// Skip comment nodes
		if (Cast<UEdGraphNode_Comment>(Node))
		{
			continue;
		}

		Result.Nodes.Add(ExtractNode(Node, Result.GraphName));
	}

	return Result;
}

FString FBlueprintGraphExtractor::ResolveVariableType(const FEdGraphPinType& PinType)
{
	if (PinType.PinSubCategoryObject.IsValid())
	{
		UObject* SubObj = PinType.PinSubCategoryObject.Get();
		if (SubObj)
		{
			FString TypeName = SubObj->GetName();
			if (TypeName.EndsWith(TEXT("_C")))
			{
				TypeName.LeftChopInline(2);
			}
			return TypeName;
		}
	}

	if (!PinType.PinSubCategory.IsNone())
	{
		return PinType.PinSubCategory.ToString();
	}

	return PinType.PinCategory.ToString();
}

// ────────────────────────────────────────────────────────────────────────────
// GE-Specific Config Extractor
// ────────────────────────────────────────────────────────────────────────────

static FString ModOpToString(EGameplayModOp::Type Op)
{
	switch (Op)
	{
	case EGameplayModOp::Additive:			return TEXT("+=");
	case EGameplayModOp::Multiplicitive:	return TEXT("*=");
	case EGameplayModOp::Division:			return TEXT("/=");
	case EGameplayModOp::Override:			return TEXT("=");
	default:								return TEXT("?=");
	}
}

static FString MagnitudeToString(const FGameplayEffectModifierMagnitude& Mag)
{
	switch (Mag.GetMagnitudeCalculationType())
	{
	case EGameplayEffectMagnitudeCalculation::ScalableFloat:
	{
		float Value = 0.f;
		Mag.GetStaticMagnitudeIfPossible(1.f, Value);
		return FString::Printf(TEXT("%g (ScalableFloat)"), Value);
	}
	case EGameplayEffectMagnitudeCalculation::AttributeBased:
	{
		const FAttributeBasedFloat* AttributeBased = GetStructFieldPtr<FAttributeBasedFloat>(
			&Mag, FGameplayEffectModifierMagnitude::StaticStruct(), TEXT("AttributeBasedMagnitude"));
		if (!AttributeBased)
		{
			return TEXT("(AttributeBased)");
		}

		FString Result = FString::Printf(TEXT("AttributeBased(%s)"),
			*AttributeBased->BackingAttribute.ToSimpleString());

		if (AttributeBased->Coefficient.GetValueAtLevel(1.f) != 1.f)
		{
			Result += FString::Printf(TEXT(" | Coefficient: %g"), AttributeBased->Coefficient.GetValueAtLevel(1.f));
		}
		if (AttributeBased->PreMultiplyAdditiveValue.GetValueAtLevel(1.f) != 0.f)
		{
			Result += FString::Printf(TEXT(" | PreAdd: %g"), AttributeBased->PreMultiplyAdditiveValue.GetValueAtLevel(1.f));
		}
		if (AttributeBased->PostMultiplyAdditiveValue.GetValueAtLevel(1.f) != 0.f)
		{
			Result += FString::Printf(TEXT(" | PostAdd: %g"), AttributeBased->PostMultiplyAdditiveValue.GetValueAtLevel(1.f));
		}

		return Result;
	}
	case EGameplayEffectMagnitudeCalculation::CustomCalculationClass:
	{
		FString Result = TEXT("CustomCalculation");
		if (const UClass* CalcClass = Mag.GetCustomMagnitudeCalculationClass())
		{
			Result += FString::Printf(TEXT("(%s)"), *StripGeneratedClassSuffix(CalcClass->GetName()));
		}
		return Result;
	}
	case EGameplayEffectMagnitudeCalculation::SetByCaller:
	{
		FName DataName;
		Mag.GetSetByCallerDataNameIfPossible(DataName);
		const FSetByCallerFloat& SBC = Mag.GetSetByCallerFloat();
		if (SBC.DataTag.IsValid())
		{
			return FString::Printf(TEXT("SetByCaller(%s)"), *SBC.DataTag.ToString());
		}
		if (!DataName.IsNone())
		{
			return FString::Printf(TEXT("SetByCaller(%s)"), *DataName.ToString());
		}
		return TEXT("SetByCaller");
	}
	default:
		return TEXT("(Unknown)");
	}
}

static FString ScalableFloatToString(const FScalableFloat& Value)
{
	return FString::Printf(TEXT("%g (ScalableFloat)"), Value.GetValueAtLevel(1.f));
}

static FString AbilityInstancingPolicyToString(EGameplayAbilityInstancingPolicy::Type Policy)
{
#pragma warning(push)
#pragma warning(disable: 4996)
	switch (Policy)
	{
	case EGameplayAbilityInstancingPolicy::NonInstanced:			return TEXT("NonInstanced");
	case EGameplayAbilityInstancingPolicy::InstancedPerActor:		return TEXT("InstancedPerActor");
	case EGameplayAbilityInstancingPolicy::InstancedPerExecution: return TEXT("InstancedPerExecution");
	default:													 return TEXT("Unknown");
	}
#pragma warning(pop)
}

static FString AbilityReplicationPolicyToString(EGameplayAbilityReplicationPolicy::Type Policy)
{
	switch (Policy)
	{
	case EGameplayAbilityReplicationPolicy::ReplicateNo:  return TEXT("ReplicateNo");
	case EGameplayAbilityReplicationPolicy::ReplicateYes: return TEXT("ReplicateYes");
	default:											 return TEXT("Unknown");
	}
}

static FString AbilityNetExecutionPolicyToString(EGameplayAbilityNetExecutionPolicy::Type Policy)
{
	switch (Policy)
	{
	case EGameplayAbilityNetExecutionPolicy::LocalPredicted:  return TEXT("LocalPredicted");
	case EGameplayAbilityNetExecutionPolicy::LocalOnly:	   return TEXT("LocalOnly");
	case EGameplayAbilityNetExecutionPolicy::ServerInitiated: return TEXT("ServerInitiated");
	case EGameplayAbilityNetExecutionPolicy::ServerOnly:	   return TEXT("ServerOnly");
	default:												   return TEXT("Unknown");
	}
}

static FString AbilityNetSecurityPolicyToString(EGameplayAbilityNetSecurityPolicy::Type Policy)
{
	switch (Policy)
	{
	case EGameplayAbilityNetSecurityPolicy::ClientOrServer:		   return TEXT("ClientOrServer");
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyExecution:   return TEXT("ServerOnlyExecution");
	case EGameplayAbilityNetSecurityPolicy::ServerOnlyTermination: return TEXT("ServerOnlyTermination");
	case EGameplayAbilityNetSecurityPolicy::ServerOnly:			   return TEXT("ServerOnly");
	default:													   return TEXT("Unknown");
	}
}

static FString AbilityTriggerSourceToString(EGameplayAbilityTriggerSource::Type Source)
{
	switch (Source)
	{
	case EGameplayAbilityTriggerSource::GameplayEvent: return TEXT("GameplayEvent");
	case EGameplayAbilityTriggerSource::OwnedTagAdded: return TEXT("OwnedTagAdded");
	case EGameplayAbilityTriggerSource::OwnedTagPresent: return TEXT("OwnedTagPresent");
	default: return TEXT("Unknown");
	}
}

static FString DurationPolicyToString(EGameplayEffectDurationType Type)
{
	switch (Type)
	{
	case EGameplayEffectDurationType::Instant:		return TEXT("Instant");
	case EGameplayEffectDurationType::HasDuration:	return TEXT("HasDuration");
	case EGameplayEffectDurationType::Infinite:		return TEXT("Infinite");
	default:										return TEXT("Unknown");
	}
}

static FString StackingTypeToString(EGameplayEffectStackingType Type)
{
	switch (Type)
	{
	case EGameplayEffectStackingType::None:					return TEXT("None");
	case EGameplayEffectStackingType::AggregateBySource:	return TEXT("AggregateBySource");
	case EGameplayEffectStackingType::AggregateByTarget:	return TEXT("AggregateByTarget");
	default:												return TEXT("Unknown");
	}
}

static FString StackingDurationPolicyToString(EGameplayEffectStackingDurationPolicy Policy)
{
	switch (Policy)
	{
	case EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication: return TEXT("RefreshOnSuccessfulApplication");
	case EGameplayEffectStackingDurationPolicy::NeverRefresh: return TEXT("NeverRefresh");
	default: return TEXT("Unknown");
	}
}

static FString StackingPeriodPolicyToString(EGameplayEffectStackingPeriodPolicy Policy)
{
	switch (Policy)
	{
	case EGameplayEffectStackingPeriodPolicy::ResetOnSuccessfulApplication: return TEXT("ResetOnSuccessfulApplication");
	case EGameplayEffectStackingPeriodPolicy::NeverReset: return TEXT("NeverReset");
	default: return TEXT("Unknown");
	}
}

static FString StackingExpirationPolicyToString(EGameplayEffectStackingExpirationPolicy Policy)
{
	switch (Policy)
	{
	case EGameplayEffectStackingExpirationPolicy::ClearEntireStack: return TEXT("ClearEntireStack");
	case EGameplayEffectStackingExpirationPolicy::RemoveSingleStackAndRefreshDuration: return TEXT("RemoveSingleStackAndRefreshDuration");
	case EGameplayEffectStackingExpirationPolicy::RefreshDuration: return TEXT("RefreshDuration");
	default: return TEXT("Unknown");
	}
}

static FString PeriodicInhibitionPolicyToString(EGameplayEffectPeriodInhibitionRemovedPolicy Policy)
{
	switch (Policy)
	{
	case EGameplayEffectPeriodInhibitionRemovedPolicy::NeverReset: return TEXT("NeverReset");
	case EGameplayEffectPeriodInhibitionRemovedPolicy::ResetPeriod: return TEXT("ResetPeriod");
	case EGameplayEffectPeriodInhibitionRemovedPolicy::ExecuteAndResetPeriod: return TEXT("ExecuteAndResetPeriod");
	default: return TEXT("Unknown");
	}
}

static FString TagContainerToString(const FGameplayTagContainer& Tags)
{
	if (Tags.Num() == 0)
	{
		return FString();
	}

	TArray<FString> TagStrings;
	for (const FGameplayTag& Tag : Tags)
	{
		TagStrings.Add(Tag.ToString());
	}
	return FString::Join(TagStrings, TEXT(", "));
}

static FString TagQueryToString(const FGameplayTagQuery& Query)
{
	if (Query.IsEmpty())
	{
		return FString();
	}

	const FString Description = Query.GetDescription();
	return Description.IsEmpty() ? TEXT("(TagQuery)") : Description;
}

static FString TagRequirementsToString(const FGameplayTagRequirements& Requirements)
{
	if (Requirements.IsEmpty())
	{
		return FString();
	}

	TArray<FString> Parts;

	const FString RequireTags = TagContainerToString(Requirements.RequireTags);
	if (!RequireTags.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("Require: %s"), *RequireTags));
	}

	const FString IgnoreTags = TagContainerToString(Requirements.IgnoreTags);
	if (!IgnoreTags.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("Ignore: %s"), *IgnoreTags));
	}

	const FString Query = TagQueryToString(Requirements.TagQuery);
	if (!Query.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("Query: %s"), *Query));
	}

	return FString::Join(Parts, TEXT("; "));
}

static FString GameplayEffectQueryToString(const FGameplayEffectQuery& Query)
{
	if (Query.IsEmpty())
	{
		return FString();
	}

	TArray<FString> Parts;

	const FString OwningQuery = TagQueryToString(Query.OwningTagQuery);
	if (!OwningQuery.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("OwningTags: %s"), *OwningQuery));
	}

	const FString EffectQuery = TagQueryToString(Query.EffectTagQuery);
	if (!EffectQuery.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("EffectTags: %s"), *EffectQuery));
	}

	const FString SourceQuery = TagQueryToString(Query.SourceTagQuery);
	if (!SourceQuery.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("SourceSpecTags: %s"), *SourceQuery));
	}

	const FString SourceAggregateQuery = TagQueryToString(Query.SourceAggregateTagQuery);
	if (!SourceAggregateQuery.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("SourceTags: %s"), *SourceAggregateQuery));
	}

	if (Query.ModifyingAttribute.IsValid())
	{
		Parts.Add(FString::Printf(TEXT("Modifies: %s"), *Query.ModifyingAttribute.GetName()));
	}

	if (Query.EffectSource)
	{
		Parts.Add(FString::Printf(TEXT("EffectSource: %s"), *GetNameSafe(Query.EffectSource)));
	}

	const FString EffectDefinition = ExportClassName(Query.EffectDefinition);
	if (!EffectDefinition.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("EffectDefinition: %s"), *EffectDefinition));
	}

	return FString::Join(Parts, TEXT("; "));
}

static void ExtractGameplayEffectConfig(UObject* CDO, UObject* ParentCDO, TArray<TPair<FString, FString>>& Out)
{
	TArray<TPair<FString, FString>> CurrentConfig;
	ExtractGameplayEffectConfigRaw(CDO, CurrentConfig);

	TArray<TPair<FString, FString>> ParentConfig;
	if (ParentCDO && ParentCDO->IsA<UGameplayEffect>())
	{
		ExtractGameplayEffectConfigRaw(ParentCDO, ParentConfig);
	}

	AppendConfigDiff(CurrentConfig, ParentConfig, Out);
}

static void ExtractGameplayEffectConfigRaw(UObject* CDO, TArray<TPair<FString, FString>>& Out)
{
	UGameplayEffect* GE = Cast<UGameplayEffect>(CDO);
	if (!GE)
	{
		return;
	}

	// Duration
	Out.Emplace(TEXT("Duration"), DurationPolicyToString(GE->DurationPolicy));

	if (GE->DurationPolicy != EGameplayEffectDurationType::Instant)
	{
		Out.Emplace(TEXT("DurationMagnitude"), MagnitudeToString(GE->DurationMagnitude));

		// Period
		float PeriodVal = GE->Period.GetValueAtLevel(1.f);
		if (PeriodVal > 0.f)
		{
			FString PeriodStr = ScalableFloatToString(GE->Period);
			if (GE->bExecutePeriodicEffectOnApplication)
			{
				PeriodStr += TEXT(" (execute on application)");
			}
			Out.Emplace(TEXT("Period"), PeriodStr);
		}

		if (PeriodVal > 0.f || GE->PeriodicInhibitionPolicy != EGameplayEffectPeriodInhibitionRemovedPolicy::NeverReset)
		{
			Out.Emplace(TEXT("PeriodicInhibitionPolicy"), PeriodicInhibitionPolicyToString(GE->PeriodicInhibitionPolicy));
		}
	}

	// Modifiers
	for (int32 i = 0; i < GE->Modifiers.Num(); ++i)
	{
		const FGameplayModifierInfo& Mod = GE->Modifiers[i];
		FString AttrName = Mod.Attribute.GetName();
		FString OpStr = ModOpToString(Mod.ModifierOp);
		FString MagStr = MagnitudeToString(Mod.ModifierMagnitude);
		const FString SourceReq = TagRequirementsToString(Mod.SourceTags);
		const FString TargetReq = TagRequirementsToString(Mod.TargetTags);

		FString Key = FString::Printf(TEXT("Modifiers[%d]"), i);
		FString Value = FString::Printf(TEXT("%s %s %s"), *AttrName, *OpStr, *MagStr);
		if (!SourceReq.IsEmpty())
		{
			Value += FString::Printf(TEXT(" | SourceTags: %s"), *SourceReq);
		}
		if (!TargetReq.IsEmpty())
		{
			Value += FString::Printf(TEXT(" | TargetTags: %s"), *TargetReq);
		}
		Out.Emplace(MoveTemp(Key), MoveTemp(Value));
	}

	// Executions
	for (int32 i = 0; i < GE->Executions.Num(); ++i)
	{
		const FGameplayEffectExecutionDefinition& Exec = GE->Executions[i];
		FString ClassName = Exec.CalculationClass
			? StripGeneratedClassSuffix(Exec.CalculationClass->GetName()) : TEXT("None");
		const FString PassedInTags = TagContainerToString(Exec.PassedInTags);
		FString Value = ClassName;
		if (!PassedInTags.IsEmpty())
		{
			Value += FString::Printf(TEXT(" | PassedInTags: %s"), *PassedInTags);
		}
		if (Exec.ConditionalGameplayEffects.Num() > 0)
		{
			Value += FString::Printf(TEXT(" | ConditionalEffects: %d"), Exec.ConditionalGameplayEffects.Num());
		}
		Out.Emplace(
			FString::Printf(TEXT("Executions[%d]"), i),
			Value);
	}

	// Stacking
	if (GE->StackingType != EGameplayEffectStackingType::None)
	{
		FString StackStr = StackingTypeToString(GE->StackingType);
		if (GE->StackLimitCount > 0)
		{
			StackStr += FString::Printf(TEXT(" (limit: %d)"), GE->StackLimitCount);
		}
		Out.Emplace(TEXT("Stacking"), StackStr);
		Out.Emplace(TEXT("StackDurationRefreshPolicy"), StackingDurationPolicyToString(GE->StackDurationRefreshPolicy));
		Out.Emplace(TEXT("StackPeriodResetPolicy"), StackingPeriodPolicyToString(GE->StackPeriodResetPolicy));
		Out.Emplace(TEXT("StackExpirationPolicy"), StackingExpirationPolicyToString(GE->StackExpirationPolicy));
		if (GE->bFactorInStackCount)
		{
			Out.Emplace(TEXT("FactorInStackCount"), TEXT("True"));
		}
	}

	// Tags — extract from GE Components (UE 5.3+) and legacy containers
	auto AddTags = [&](const FString& Label, const FGameplayTagContainer& Tags)
	{
		FString TagStr = TagContainerToString(Tags);
		if (!TagStr.IsEmpty())
		{
			Out.Emplace(FString::Printf(TEXT("Tags.%s"), *Label), TagStr);
		}
	};

	AddTags(TEXT("AssetTags"), GE->GetAssetTags());
	AddTags(TEXT("GrantedToActor"), GE->GetGrantedTags());
	AddTags(TEXT("BlockedAbilities"), GE->GetBlockedAbilityTags());

	if (const UTargetTagRequirementsGameplayEffectComponent* RequirementsComponent =
		GE->FindComponent<UTargetTagRequirementsGameplayEffectComponent>())
	{
		const FString ApplicationReq = TagRequirementsToString(RequirementsComponent->ApplicationTagRequirements);
		if (!ApplicationReq.IsEmpty())
		{
			Out.Emplace(TEXT("Requirements.Application"), ApplicationReq);
		}

		const FString OngoingReq = TagRequirementsToString(RequirementsComponent->OngoingTagRequirements);
		if (!OngoingReq.IsEmpty())
		{
			Out.Emplace(TEXT("Requirements.Ongoing"), OngoingReq);
		}

		const FString RemovalReq = TagRequirementsToString(RequirementsComponent->RemovalTagRequirements);
		if (!RemovalReq.IsEmpty())
		{
			Out.Emplace(TEXT("Requirements.Removal"), RemovalReq);
		}
	}
	else
	{
#pragma warning(push)
#pragma warning(disable: 4996)
		const FString ApplicationReq = TagRequirementsToString(GE->ApplicationTagRequirements);
		if (!ApplicationReq.IsEmpty())
		{
			Out.Emplace(TEXT("Requirements.Application"), ApplicationReq);
		}

		const FString OngoingReq = TagRequirementsToString(GE->OngoingTagRequirements);
		if (!OngoingReq.IsEmpty())
		{
			Out.Emplace(TEXT("Requirements.Ongoing"), OngoingReq);
		}

		const FString RemovalReq = TagRequirementsToString(GE->RemovalTagRequirements);
		if (!RemovalReq.IsEmpty())
		{
			Out.Emplace(TEXT("Requirements.Removal"), RemovalReq);
		}
#pragma warning(pop)
	}

	if (const UAbilitiesGameplayEffectComponent* AbilitiesComponent =
		GE->FindComponent<UAbilitiesGameplayEffectComponent>())
	{
		if (const TArray<FGameplayAbilitySpecConfig>* GrantConfigs =
			GetObjectArrayFieldPtr<FGameplayAbilitySpecConfig>(AbilitiesComponent, TEXT("GrantAbilityConfigs")))
		{
			for (int32 i = 0; i < GrantConfigs->Num(); ++i)
			{
				const FGameplayAbilitySpecConfig& Config = (*GrantConfigs)[i];
				FString Value = ExportClassName(Config.Ability);
				Value += FString::Printf(TEXT(" | Level: %g"), Config.LevelScalableFloat.GetValueAtLevel(1.f));
				if (Config.InputID != INDEX_NONE)
				{
					Value += FString::Printf(TEXT(" | InputID: %d"), Config.InputID);
				}
				Value += FString::Printf(TEXT(" | RemovalPolicy: %s"),
					*StaticEnum<EGameplayEffectGrantedAbilityRemovePolicy>()->GetNameStringByValue(static_cast<int64>(Config.RemovalPolicy)));

				Out.Emplace(FString::Printf(TEXT("GrantedAbilities[%d]"), i), Value);
			}
		}
	}
	else if (const TArray<FGameplayAbilitySpecDef>* GrantedAbilities =
		GetObjectArrayFieldPtr<FGameplayAbilitySpecDef>(GE, TEXT("GrantedAbilities")))
	{
		for (int32 i = 0; i < GrantedAbilities->Num(); ++i)
		{
			const FGameplayAbilitySpecDef& GrantedAbility = (*GrantedAbilities)[i];
			Out.Emplace(
				FString::Printf(TEXT("GrantedAbilities[%d]"), i),
				StripGeneratedClassSuffix(GetNameSafe(GrantedAbility.Ability)));
		}
	}

	if (const UImmunityGameplayEffectComponent* ImmunityComponent =
		GE->FindComponent<UImmunityGameplayEffectComponent>())
	{
		for (int32 i = 0; i < ImmunityComponent->ImmunityQueries.Num(); ++i)
		{
			const FString Query = GameplayEffectQueryToString(ImmunityComponent->ImmunityQueries[i]);
			if (!Query.IsEmpty())
			{
				Out.Emplace(FString::Printf(TEXT("ImmunityQueries[%d]"), i), Query);
			}
		}
	}
	else
	{
#pragma warning(push)
#pragma warning(disable: 4996)
		const FString ImmunityReq = TagRequirementsToString(GE->GrantedApplicationImmunityTags);
		if (!ImmunityReq.IsEmpty())
		{
			Out.Emplace(TEXT("GrantedApplicationImmunityTags"), ImmunityReq);
		}
		const FString ImmunityQuery = GameplayEffectQueryToString(GE->GrantedApplicationImmunityQuery);
		if (!ImmunityQuery.IsEmpty())
		{
			Out.Emplace(TEXT("GrantedApplicationImmunityQuery"), ImmunityQuery);
		}
#pragma warning(pop)
	}

	if (const URemoveOtherGameplayEffectComponent* RemoveOtherComponent =
		GE->FindComponent<URemoveOtherGameplayEffectComponent>())
	{
		for (int32 i = 0; i < RemoveOtherComponent->RemoveGameplayEffectQueries.Num(); ++i)
		{
			const FString Query = GameplayEffectQueryToString(RemoveOtherComponent->RemoveGameplayEffectQueries[i]);
			if (!Query.IsEmpty())
			{
				Out.Emplace(FString::Printf(TEXT("RemoveGameplayEffectQueries[%d]"), i), Query);
			}
		}
	}
	else
	{
#pragma warning(push)
#pragma warning(disable: 4996)
		const FString RemoveQuery = GameplayEffectQueryToString(GE->RemoveGameplayEffectQuery);
		if (!RemoveQuery.IsEmpty())
		{
			Out.Emplace(TEXT("RemoveGameplayEffectQuery"), RemoveQuery);
		}
#pragma warning(pop)
	}

	// GameplayCues
	for (int32 i = 0; i < GE->GameplayCues.Num(); ++i)
	{
		FString CueTags = TagContainerToString(GE->GameplayCues[i].GameplayCueTags);
		if (!CueTags.IsEmpty())
		{
			Out.Emplace(
				FString::Printf(TEXT("GameplayCues[%d]"), i),
				CueTags);
		}
	}
}

static void ExtractGameplayAbilityConfig(UObject* CDO, UObject* ParentCDO, TArray<TPair<FString, FString>>& Out)
{
	TArray<TPair<FString, FString>> CurrentConfig;
	ExtractGameplayAbilityConfigRaw(CDO, CurrentConfig);

	TArray<TPair<FString, FString>> ParentConfig;
	if (ParentCDO && ParentCDO->IsA<UGameplayAbility>())
	{
		ExtractGameplayAbilityConfigRaw(ParentCDO, ParentConfig);
	}

	AppendConfigDiff(CurrentConfig, ParentConfig, Out);
}

static void ExtractGameplayAbilityConfigRaw(UObject* CDO, TArray<TPair<FString, FString>>& Out)
{
	UGameplayAbility* Ability = Cast<UGameplayAbility>(CDO);
	if (!Ability)
	{
		return;
	}

	const FString AbilityTags = TagContainerToString(Ability->GetAssetTags());
	if (!AbilityTags.IsEmpty())
	{
		Out.Emplace(TEXT("AbilityTags"), AbilityTags);
	}

	Out.Emplace(TEXT("ReplicationPolicy"),
		AbilityReplicationPolicyToString(Ability->GetReplicationPolicy()));
	Out.Emplace(TEXT("InstancingPolicy"),
		AbilityInstancingPolicyToString(Ability->GetInstancingPolicy()));
	Out.Emplace(TEXT("NetExecutionPolicy"),
		AbilityNetExecutionPolicyToString(Ability->GetNetExecutionPolicy()));
	Out.Emplace(TEXT("NetSecurityPolicy"),
		AbilityNetSecurityPolicyToString(Ability->GetNetSecurityPolicy()));

	if (GetObjectBoolField(Ability, TEXT("bServerRespectsRemoteAbilityCancellation")))
	{
		Out.Emplace(TEXT("ServerRespectsRemoteAbilityCancellation"), TEXT("True"));
	}
	if (GetObjectBoolField(Ability, TEXT("bRetriggerInstancedAbility")))
	{
		Out.Emplace(TEXT("RetriggerInstancedAbility"), TEXT("True"));
	}
	if (GetObjectBoolField(Ability, TEXT("bReplicateInputDirectly")))
	{
		Out.Emplace(TEXT("ReplicateInputDirectly"), TEXT("True"));
	}

	const FString CostClass = GetObjectClassFieldName(Ability, TEXT("CostGameplayEffectClass"));
	if (!CostClass.IsEmpty())
	{
		Out.Emplace(TEXT("CostGameplayEffectClass"), CostClass);
	}

	const FString CooldownClass = GetObjectClassFieldName(Ability, TEXT("CooldownGameplayEffectClass"));
	if (!CooldownClass.IsEmpty())
	{
		Out.Emplace(TEXT("CooldownGameplayEffectClass"), CooldownClass);
	}

	auto AddTags = [&](const TCHAR* Label, const TCHAR* FieldName)
	{
		if (const FGameplayTagContainer* Tags = GetObjectStructFieldPtr<FGameplayTagContainer>(Ability, FieldName))
		{
			const FString TagString = TagContainerToString(*Tags);
			if (!TagString.IsEmpty())
			{
				Out.Emplace(Label, TagString);
			}
		}
	};

	AddTags(TEXT("CancelAbilitiesWithTag"), TEXT("CancelAbilitiesWithTag"));
	AddTags(TEXT("BlockAbilitiesWithTag"), TEXT("BlockAbilitiesWithTag"));
	AddTags(TEXT("ActivationOwnedTags"), TEXT("ActivationOwnedTags"));
	AddTags(TEXT("ActivationRequiredTags"), TEXT("ActivationRequiredTags"));
	AddTags(TEXT("ActivationBlockedTags"), TEXT("ActivationBlockedTags"));
	AddTags(TEXT("SourceRequiredTags"), TEXT("SourceRequiredTags"));
	AddTags(TEXT("SourceBlockedTags"), TEXT("SourceBlockedTags"));
	AddTags(TEXT("TargetRequiredTags"), TEXT("TargetRequiredTags"));
	AddTags(TEXT("TargetBlockedTags"), TEXT("TargetBlockedTags"));

	if (const TArray<FAbilityTriggerData>* AbilityTriggers =
		GetObjectArrayFieldPtr<FAbilityTriggerData>(Ability, TEXT("AbilityTriggers")))
	{
		for (int32 i = 0; i < AbilityTriggers->Num(); ++i)
		{
			const FAbilityTriggerData& Trigger = (*AbilityTriggers)[i];
			FString Value = AbilityTriggerSourceToString(Trigger.TriggerSource);
			if (Trigger.TriggerTag.IsValid())
			{
				Value = FString::Printf(TEXT("%s | Tag: %s"), *Value, *Trigger.TriggerTag.ToString());
			}
			Out.Emplace(FString::Printf(TEXT("AbilityTriggers[%d]"), i), Value);
		}
	}
}

// ────────────────────────────────────────────────────────────────────────────
// Config Extractor Registry
// ────────────────────────────────────────────────────────────────────────────

static const TArray<FConfigExtractorEntry>& GetConfigExtractorRegistry()
{
	static TArray<FConfigExtractorEntry> Registry = {
		{ UGameplayEffect::StaticClass(), TEXT("GameplayEffect"), &ExtractGameplayEffectConfig },
		{ UGameplayAbility::StaticClass(), TEXT("GameplayAbility"), &ExtractGameplayAbilityConfig },
	};
	return Registry;
}

// ────────────────────────────────────────────────────────────────────────────
// Generic CDO Property Diff
// ────────────────────────────────────────────────────────────────────────────

FString FBlueprintGraphExtractor::ExportLeafValue(FProperty* Prop, const void* ValuePtr)
{
	FString Value;

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
		int64 IntValue = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
		UEnum* Enum = EnumProp->GetEnum();
		if (Enum)
		{
			FText DisplayName = Enum->GetDisplayNameTextByValue(IntValue);
			if (!DisplayName.IsEmpty())
			{
				return DisplayName.ToString();
			}
		}
	}

	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			uint8 ByteValue = *static_cast<const uint8*>(ValuePtr);
			FText DisplayName = ByteProp->Enum->GetDisplayNameTextByValue(ByteValue);
			if (!DisplayName.IsEmpty())
			{
				return DisplayName.ToString();
			}
		}
	}

	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (Obj)
		{
			FString Name = Obj->GetName();
			if (Name.EndsWith(TEXT("_C")))
			{
				Name.LeftChopInline(2);
			}
			return Name;
		}
		return TEXT("None");
	}

	Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
	return Value;
}

void FBlueprintGraphExtractor::FlattenProperty(
	FProperty* Prop, const void* ValuePtr, const void* DefaultPtr,
	const FString& Prefix,
	TArray<TPair<FString, FString>>& OutProperties)
{
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			FProperty* SubProp = *It;
			if (SubProp->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
			{
				continue;
			}

			const void* SubValue = SubProp->ContainerPtrToValuePtr<void>(ValuePtr);
			const void* SubDefault = DefaultPtr
				? SubProp->ContainerPtrToValuePtr<void>(DefaultPtr) : nullptr;

			if (SubDefault && SubProp->Identical(SubValue, SubDefault))
			{
				continue;
			}

			FString SubPrefix = Prefix + TEXT(".") + SubProp->GetName();
			FlattenProperty(SubProp, SubValue, SubDefault, SubPrefix, OutProperties);
		}
		return;
	}

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
		if (ArrayHelper.Num() == 0)
		{
			return;
		}

		for (int32 i = 0; i < ArrayHelper.Num(); ++i)
		{
			FString ElemPrefix = FString::Printf(TEXT("%s[%d]"), *Prefix, i);
			FlattenProperty(
				ArrayProp->Inner,
				ArrayHelper.GetRawPtr(i), nullptr,
				ElemPrefix, OutProperties);
		}
		return;
	}

	// Leaf property
	FString Value = ExportLeafValue(Prop, ValuePtr);
	if (!Value.IsEmpty())
	{
		OutProperties.Emplace(Prefix, MoveTemp(Value));
	}
}

void FBlueprintGraphExtractor::ExtractCDOProperties(
	UObject* CDO, UObject* ParentCDO,
	const TSet<FName>& BlueprintVarNames,
	TArray<TPair<FString, FString>>& OutProperties)
{
	if (!CDO || !ParentCDO)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(CDO->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
		{
			continue;
		}

		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		if (Prop->HasMetaData(TEXT("DeprecatedProperty")))
		{
			continue;
		}

		if (BlueprintVarNames.Contains(Prop->GetFName()))
		{
			continue;
		}

		const void* CDOValue = Prop->ContainerPtrToValuePtr<void>(CDO);
		const void* ParentValue = Prop->ContainerPtrToValuePtr<void>(ParentCDO);

		if (Prop->Identical(CDOValue, ParentValue))
		{
			continue;
		}

		FlattenProperty(Prop, CDOValue, ParentValue, Prop->GetName(), OutProperties);
	}
}
