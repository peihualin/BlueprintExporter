#include "BlueprintExporterModule.h"
#include "BlueprintGraphExtractor.h"
#include "BlueprintTextFormatter.h"
#include "BlueprintExporterSettings.h"
#include "FlowAssetExtractor.h"

#include "ToolMenus.h"
#include "ToolMenuContext.h"
#include "ContentBrowserMenuContexts.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Framework/Application/SlateApplication.h"
#include "BlueprintEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "HAL/PlatformApplicationMisc.h"
#include "GraphEditorModule.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectHash.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "FlowAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintExporter, Log, All);

#define LOCTEXT_NAMESPACE "BlueprintExporter"

namespace
{
int32 CountExtractedNodes(const FExportedBlueprint& Blueprint)
{
	int32 TotalNodes = 0;
	for (const FExportedGraph& Graph : Blueprint.Graphs)
	{
		TotalNodes += Graph.Nodes.Num();
	}
	return TotalNodes;
}

bool HasConfigOnlyExportContent(const FExportedBlueprint& Blueprint)
{
	return Blueprint.CDOProperties.Num() > 0
		|| !Blueprint.ParentConfigSource.IsEmpty();
}

bool HasStructuralExportContent(const FExportedBlueprint& Blueprint)
{
	return Blueprint.Variables.Num() > 0
		|| Blueprint.ImplementedInterfaces.Num() > 0
		|| Blueprint.Components.Num() > 0;
}

bool MatchesParentClassList(UClass* ParentClass, const TArray<FSoftClassPath>& CandidatePaths)
{
	if (!ParentClass)
	{
		return false;
	}

	for (const FSoftClassPath& CandidatePath : CandidatePaths)
	{
		UClass* CandidateClass = CandidatePath.ResolveClass();
		if (CandidateClass && ParentClass->IsChildOf(CandidateClass))
		{
			return true;
		}
	}

	return false;
}

bool CachedSummaryLikelyHasLogic(const FString& SummaryPath)
{
	FString SummaryText;
	if (!FFileHelper::LoadFileToString(SummaryText, *SummaryPath))
	{
		return false;
	}

	TArray<FString> Lines;
	SummaryText.ParseIntoArrayLines(Lines, true);
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("--- ")))
		{
			return true;
		}

		if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]:")))
		{
			return true;
		}

		if (Line.StartsWith(TEXT("=== "))
			&& !Line.StartsWith(TEXT("=== Blueprint:"))
			&& Line != TEXT("=== Variables ===")
			&& Line != TEXT("=== Configuration ===")
			&& Line.Contains(TEXT("(")))
		{
			return true;
		}
	}

	return false;
}

void DeleteCachedExportDirectory(const FString& BlueprintName)
{
	const FString ExportDir = FPaths::Combine(
		FPaths::ProjectDir(), TEXT("BlueprintExports"), BlueprintName);
	IFileManager::Get().DeleteDirectory(*ExportDir, /*RequireExists=*/false, /*Tree=*/true);
}
}

void FBlueprintExporterModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintExporterModule::RegisterMenus));

	ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddRaw(
		this, &FBlueprintExporterModule::OnModulesChanged);

	if (FModuleManager::Get().IsModuleLoaded(TEXT("GraphEditor")))
	{
		RegisterGraphEditorExtender();
	}

	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(
		this, &FBlueprintExporterModule::OnPackageSaved);
	PreExitHandle = FEditorDelegates::OnShutdownPostPackagesSaved.AddRaw(
		this, &FBlueprintExporterModule::OnEditorPreExit);
}

void FBlueprintExporterModule::ShutdownModule()
{
	UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	FEditorDelegates::OnShutdownPostPackagesSaved.Remove(PreExitHandle);

	FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);

	if (GraphEditorMenuExtenderIndex != INDEX_NONE)
	{
		if (FGraphEditorModule* GraphEditorModule =
			FModuleManager::GetModulePtr<FGraphEditorModule>(TEXT("GraphEditor")))
		{
			TArray<FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode>& Extenders =
				GraphEditorModule->GetAllGraphEditorContextMenuExtender();
			if (Extenders.IsValidIndex(GraphEditorMenuExtenderIndex))
			{
				Extenders.RemoveAt(GraphEditorMenuExtenderIndex);
			}
		}
		GraphEditorMenuExtenderIndex = INDEX_NONE;
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FBlueprintExporterModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.Blueprint");
	FToolMenuSection& Section = Menu->AddSection("BlueprintExporter",
		LOCTEXT("BlueprintExporterSection", "Blueprint Exporter"));

	Section.AddMenuEntry(
		"ExportBlueprintLogic",
		LOCTEXT("ExportLabel", "Export Blueprint Logic"),
		LOCTEXT("ExportTooltip", "Export all graphs to AI-readable plain text"),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateLambda([this](const FToolMenuContext& Context)
		{
			const UContentBrowserAssetContextMenuContext* CBContext =
				Context.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!CBContext)
			{
				return;
			}

			TArray<FAssetData> SelectedAssets;
			for (const FAssetData& AssetData : CBContext->SelectedAssets)
			{
				SelectedAssets.Add(AssetData);
			}

			if (SelectedAssets.Num() > 0)
			{
				ExportSelectedBlueprints(SelectedAssets);
			}
		})
	);

	Section.AddMenuEntry(
		"ExportAllBlueprintsToCache",
		LOCTEXT("ExportAllLabel", "Export All Blueprints to Cache"),
		LOCTEXT("ExportAllTooltip", "Scan all Blueprints and export to ProjectDir/BlueprintExports/"),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateLambda([this](const FToolMenuContext&)
		{
			ExportAllBlueprints();
		})
	);

	// FlowAsset Content Browser context menu
	UToolMenu* FlowMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.FlowAsset");
	FToolMenuSection& FlowSection = FlowMenu->AddSection("FlowAssetExporter",
		LOCTEXT("FlowAssetExporterSection", "FlowAsset Exporter"));

	FlowSection.AddMenuEntry(
		"ExportFlowAssetLogic",
		LOCTEXT("ExportFlowLabel", "Export FlowAsset Logic"),
		LOCTEXT("ExportFlowTooltip", "Export flow graph to AI-readable plain text"),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateLambda([this](const FToolMenuContext& Context)
		{
			const UContentBrowserAssetContextMenuContext* CBContext =
				Context.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!CBContext)
			{
				return;
			}

			for (const FAssetData& AssetData : CBContext->SelectedAssets)
			{
				UFlowAsset* FlowAsset = Cast<UFlowAsset>(AssetData.GetAsset());
				if (!FlowAsset)
				{
					continue;
				}

				FFlowAssetExtractor Extractor;
				FExportedBlueprint ExportedFA = Extractor.Extract(FlowAsset);

				FBlueprintTextFormatter Formatter;
				FString OutputText = Formatter.Format(ExportedFA);

				IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
				if (!DesktopPlatform)
				{
					continue;
				}

				const FString DefaultFileName = FString::Printf(TEXT("%s_exported.txt"), *FlowAsset->GetName());
				TArray<FString> OutFiles;
				const bool bOpened = DesktopPlatform->SaveFileDialog(
					FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
					TEXT("Export FlowAsset Logic"),
					FPaths::ProjectDir(),
					DefaultFileName,
					TEXT("Text Files (*.txt)|*.txt"),
					0,
					OutFiles);

				if (bOpened && OutFiles.Num() > 0)
				{
					FFileHelper::SaveStringToFile(OutputText, *OutFiles[0],
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				}
			}
		})
	);

	FlowSection.AddMenuEntry(
		"ExportAllFlowAssetsToCache",
		LOCTEXT("ExportAllFlowLabel", "Export All FlowAssets to Cache"),
		LOCTEXT("ExportAllFlowTooltip", "Scan all FlowAssets and export to ProjectDir/FlowAssetExports/"),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateLambda([this](const FToolMenuContext&)
		{
			ExportAllFlowAssets();
		})
	);
}

void FBlueprintExporterModule::RegisterGraphEditorExtender()
{
	if (GraphEditorMenuExtenderIndex != INDEX_NONE)
	{
		return; // Already registered
	}

	FGraphEditorModule& GraphEditorModule =
		FModuleManager::GetModuleChecked<FGraphEditorModule>(TEXT("GraphEditor"));
	GraphEditorMenuExtenderIndex =
		GraphEditorModule.GetAllGraphEditorContextMenuExtender().Add(
			FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode::CreateLambda(
				[this](const TSharedRef<FUICommandList>, const UEdGraph* Graph,
					   const UEdGraphNode* /*Node*/, const UEdGraphPin* /*Pin*/,
					   bool /*bIsReadOnly*/) -> TSharedRef<FExtender>
				{
					TSharedRef<FExtender> Extender = MakeShared<FExtender>();
					if (!Graph)
					{
						return Extender;
					}
					UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
					if (!BP)
					{
						return Extender;
					}

					bool bHasSelection = false;
					UAssetEditorSubsystem* Sub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
					if (Sub)
					{
						IAssetEditorInstance* Inst = Sub->FindEditorForAsset(BP, false);
						if (Inst && Inst->GetEditorName() == FName("BlueprintEditor"))
						{
							bHasSelection = static_cast<FBlueprintEditor*>(Inst)->GetSelectedNodes().Num() > 0;
						}
					}

					Extender->AddMenuExtension(
						"EdGraphSchemaNodeActions", EExtensionHook::After, nullptr,
						FMenuExtensionDelegate::CreateLambda(
							[this, Graph, bHasSelection](FMenuBuilder& MenuBuilder)
							{
								MenuBuilder.BeginSection("BlueprintExporterNodes",
									LOCTEXT("SelectedNodesSection", "Blueprint Exporter"));
								MenuBuilder.AddMenuEntry(
									LOCTEXT("CopySelectedLabel", "Copy Selected Nodes as Text"),
									bHasSelection
										? LOCTEXT("CopySelectedTooltip", "Copy selected nodes as AI-readable plain text to clipboard")
										: LOCTEXT("CopySelectedTooltipDisabled", "No nodes selected"),
									FSlateIcon(),
									FUIAction(
										FExecuteAction::CreateLambda([this, Graph]() { CopySelectedNodesToClipboard(Graph); }),
										FCanExecuteAction::CreateLambda([bHasSelection]() { return bHasSelection; })
									)
								);
								MenuBuilder.AddMenuEntry(
									LOCTEXT("ExportSelectedLabel", "Export Selected Nodes to File..."),
									bHasSelection
										? LOCTEXT("ExportSelectedTooltip", "Export selected nodes as AI-readable plain text to a file")
										: LOCTEXT("ExportSelectedTooltipDisabled", "No nodes selected"),
									FSlateIcon(),
									FUIAction(
										FExecuteAction::CreateLambda([this, Graph]() { ExportSelectedNodesToFile(Graph); }),
										FCanExecuteAction::CreateLambda([bHasSelection]() { return bHasSelection; })
									)
								);
								MenuBuilder.EndSection();
							}
						)
					);
					return Extender;
				}
			)
		);
}

void FBlueprintExporterModule::OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
{
	if (ModuleName == TEXT("GraphEditor") && Reason == EModuleChangeReason::ModuleLoaded)
	{
		RegisterGraphEditorExtender();
	}
}

void FBlueprintExporterModule::ExportSelectedBlueprints(const TArray<FAssetData>& SelectedAssets)
{
	for (const FAssetData& AssetData : SelectedAssets)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
		if (!Blueprint)
		{
			continue;
		}

		// Extract
		FBlueprintGraphExtractor Extractor;
		FExportedBlueprint ExportedBP = Extractor.Extract(Blueprint);

		// Format
		FBlueprintTextFormatter Formatter;
		FString OutputText = Formatter.Format(ExportedBP);

		// Save file dialog
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform)
		{
			continue;
		}

		const FString DefaultFileName = FString::Printf(TEXT("%s_exported.txt"), *Blueprint->GetName());
		const FString DefaultPath = FPaths::ProjectDir();

		TArray<FString> OutFiles;
		const bool bOpened = DesktopPlatform->SaveFileDialog(
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
			TEXT("Export Blueprint Logic"),
			DefaultPath,
			DefaultFileName,
			TEXT("Text Files (*.txt)|*.txt"),
			0,
			OutFiles);

		if (bOpened && OutFiles.Num() > 0)
		{
			FFileHelper::SaveStringToFile(OutputText, *OutFiles[0],
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}
}

static bool GetSelectedNodesForGraph(
	const UEdGraph* Graph,
	TSet<UEdGraphNode*>& OutNodes,
	FString& OutBlueprintName)
{
	UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!BP)
	{
		return false;
	}

	OutBlueprintName = BP->GetName();

	UAssetEditorSubsystem* AssetEditorSubsystem =
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return false;
	}

	IAssetEditorInstance* EditorInstance =
		AssetEditorSubsystem->FindEditorForAsset(BP, false);
	if (!EditorInstance || EditorInstance->GetEditorName() != FName("BlueprintEditor"))
	{
		return false;
	}

	FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(EditorInstance);
	const FGraphPanelSelectionSet& RawSelection = BPEditor->GetSelectedNodes();
	for (UObject* Obj : RawSelection)
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Obj))
		{
			OutNodes.Add(Node);
		}
	}

	return OutNodes.Num() > 0;
}

void FBlueprintExporterModule::CopySelectedNodesToClipboard(const UEdGraph* Graph)
{
	TSet<UEdGraphNode*> SelectedNodes;
	FString BlueprintName;
	if (!GetSelectedNodesForGraph(Graph, SelectedNodes, BlueprintName))
	{
		return;
	}

	FBlueprintGraphExtractor Extractor;
	FExportedGraph ExportedGraph = Extractor.ExtractSelectedNodes(SelectedNodes, Graph);

	FBlueprintTextFormatter Formatter;
	FString OutputText = Formatter.FormatSelectedNodes(ExportedGraph, BlueprintName);

	FPlatformApplicationMisc::ClipboardCopy(*OutputText);
}

void FBlueprintExporterModule::ExportSelectedNodesToFile(const UEdGraph* Graph)
{
	TSet<UEdGraphNode*> SelectedNodes;
	FString BlueprintName;
	if (!GetSelectedNodesForGraph(Graph, SelectedNodes, BlueprintName))
	{
		return;
	}

	FBlueprintGraphExtractor Extractor;
	FExportedGraph ExportedGraph = Extractor.ExtractSelectedNodes(SelectedNodes, Graph);

	FBlueprintTextFormatter Formatter;
	FString OutputText = Formatter.FormatSelectedNodes(ExportedGraph, BlueprintName);

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return;
	}

	const FString DefaultFileName = FString::Printf(TEXT("%s_selection.txt"), *BlueprintName);
	const FString DefaultPath = FPaths::ProjectDir();

	TArray<FString> OutFiles;
	const bool bOpened = DesktopPlatform->SaveFileDialog(
		FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
		TEXT("Export Selected Nodes"),
		DefaultPath,
		DefaultFileName,
		TEXT("Text Files (*.txt)|*.txt"),
		0,
		OutFiles);

	if (bOpened && OutFiles.Num() > 0)
	{
		FFileHelper::SaveStringToFile(OutputText, *OutFiles[0],
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

// --- Iteration 6 ---

FString FBlueprintExporterModule::SanitizeFileName(const FString& Name)
{
	FString Result;
	Result.Reserve(Name.Len());
	for (TCHAR Ch : Name)
	{
		if (FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-'))
		{
			Result.AppendChar(Ch);
		}
		else
		{
			Result.AppendChar(TEXT('_'));
		}
	}
	return Result;
}

bool FBlueprintExporterModule::WriteFileIfChanged(const FString& FilePath, const FString& Content)
{
	FString ExistingContent;
	if (FFileHelper::LoadFileToString(ExistingContent, *FilePath))
	{
		if (ExistingContent == Content)
		{
			return false;  // 内容完全相同，跳过写入
		}
	}

	FFileHelper::SaveStringToFile(Content, *FilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return true;
}

FDateTime FBlueprintExporterModule::GetAssetFileTimestamp(const FAssetData& AssetData)
{
	FString PackagePath;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			AssetData.PackageName.ToString(), PackagePath,
			FPackageName::GetAssetPackageExtension()))
	{
		return FDateTime::MinValue();
	}

	return IFileManager::Get().GetTimeStamp(*PackagePath);
}

bool FBlueprintExporterModule::ShouldExport(UBlueprint* Blueprint,
	const UBlueprintExporterSettings* Settings,
	FExportedBlueprint* OutExtracted) const
{
	if (!Blueprint || !Settings)
	{
		return false;
	}

	// Blueprint type filter
	switch (Blueprint->BlueprintType)
	{
	case BPTYPE_Normal:
		if (!Settings->bExportNormalBlueprint) return false;
		break;
	case BPTYPE_FunctionLibrary:
		if (!Settings->bExportFunctionLibrary) return false;
		break;
	case BPTYPE_MacroLibrary:
		if (!Settings->bExportMacroLibrary) return false;
		break;
	case BPTYPE_Interface:
		if (!Settings->bExportInterface) return false;
		break;
	case BPTYPE_LevelScript:
		if (!Settings->bExportLevelScript) return false;
		break;
	default:
		break;
	}

	// Blacklist: excluded parent classes
	UClass* ParentClass = Blueprint->ParentClass;
	if (MatchesParentClassList(ParentClass, Settings->ExcludedParentClasses))
	{
		return false;
	}

	// Whitelist: parent class filter (empty = no restriction)
	if (Settings->ParentClassFilter.Num() > 0
		&& !MatchesParentClassList(ParentClass, Settings->ParentClassFilter))
	{
		return false;
	}

	FBlueprintGraphExtractor Extractor;
	FExportedBlueprint ExportedBP = Extractor.Extract(Blueprint);
	const int32 ExtractedNodeCount = CountExtractedNodes(ExportedBP);
	const bool bHasConfigOnlyContent = HasConfigOnlyExportContent(ExportedBP);
	const bool bHasStructuralContent = HasStructuralExportContent(ExportedBP);

	if (ExtractedNodeCount == 0 && !bHasConfigOnlyContent && !bHasStructuralContent)
	{
		return false;
	}

	bool bShouldExport;
	if (ExtractedNodeCount > 0)
	{
		bShouldExport = ExtractedNodeCount >= Settings->MinNodeCount;
	}
	else if (bHasStructuralContent)
	{
		// Blueprints that only carry structural context should still export even without graph nodes.
		bShouldExport = true;
	}
	else
	{
		// Pure config-only exports are opt-in to avoid flooding the cache with data-only assets.
		bShouldExport = MatchesParentClassList(ParentClass, Settings->ConfigOnlyParentClassWhitelist);
	}

	if (bShouldExport && OutExtracted)
	{
		*OutExtracted = MoveTemp(ExportedBP);
	}
	return bShouldExport;
}

void FBlueprintExporterModule::OnPackageSaved(const FString& /*PackageFilename*/,
	UPackage* Package, FObjectPostSaveContext /*SaveContext*/)
{
	const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
	if (!Settings || !Package)
	{
		return;
	}

	ForEachObjectWithPackage(Package, [this, Settings](UObject* Obj) -> bool
	{
		// Blueprint auto-export
		if (Settings->bAutoExportOnSave)
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				FExportedBlueprint ExtractedBP;
				if (ShouldExport(BP, Settings, &ExtractedBP))
				{
					ExportBlueprintToCache(BP, &ExtractedBP);
					GenerateAgentsMd();
				}
				else
				{
					DeleteCachedExportDirectory(BP->GetName());
				}
				return true;
			}
		}

		// FlowAsset auto-export
		if (Settings->bAutoExportFlowAssetOnSave && Settings->bExportFlowAssets)
		{
			if (UFlowAsset* FlowAsset = Cast<UFlowAsset>(Obj))
			{
				ExportFlowAssetToCache(FlowAsset);
				GenerateFlowAgentsMd();
				return true;
			}
		}

		return true;
	});
}

void FBlueprintExporterModule::OnEditorPreExit()
{
	const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
	if (!Settings || !Settings->bExportOnEditorClose)
	{
		return;
	}

	ExportAllBlueprints();

	if (Settings->bExportFlowAssets)
	{
		ExportAllFlowAssets();
	}
}

bool FBlueprintExporterModule::ExportBlueprintToCache(UBlueprint* Blueprint, FExportedBlueprint* PreExtracted)
{
	if (!Blueprint)
	{
		return false;
	}

	FExportedBlueprint ExportedBP;
	if (PreExtracted)
	{
		ExportedBP = MoveTemp(*PreExtracted);
	}
	else
	{
		FBlueprintGraphExtractor Extractor;
		ExportedBP = Extractor.Extract(Blueprint);
	}

	FBlueprintTextFormatter Formatter;

	const FString BPName = Blueprint->GetName();
	const FString OutputDir = FPaths::Combine(
		FPaths::ProjectDir(), TEXT("BlueprintExports"), BPName);

	IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/true);

	int32 FilesWritten = 0;

	// Write individual graph files
	for (const FExportedGraph& Graph : ExportedBP.Graphs)
	{
		FString GraphText = Formatter.FormatGraphOnly(Graph);
		if (GraphText.IsEmpty())
		{
			continue;
		}

		const FString FileName = SanitizeFileName(Graph.GraphName) + TEXT(".txt");
		const FString FilePath = FPaths::Combine(OutputDir, FileName);
		if (WriteFileIfChanged(FilePath, GraphText))
		{
			FilesWritten++;
		}
	}

	// Write summary file (includes compact execution flow)
	const FString SummaryText = Formatter.FormatSummary(ExportedBP);
	const FString SummaryPath = FPaths::Combine(OutputDir, TEXT("_summary.txt"));
	if (WriteFileIfChanged(SummaryPath, SummaryText))
	{
		FilesWritten++;
	}

	if (FilesWritten > 0)
	{
		UE_LOG(LogBlueprintExporter, Log,
			TEXT("Exported %s: %d file(s) updated"), *Blueprint->GetName(), FilesWritten);
	}
	else
	{
		UE_LOG(LogBlueprintExporter, Verbose,
			TEXT("Exported %s: no changes detected, all files up-to-date"), *Blueprint->GetName());
	}

	return FilesWritten > 0;
}

void FBlueprintExporterModule::ExportAllBlueprints()
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	AssetRegistry.SearchAllAssets(/*bSynchronousSearch=*/true);

	TArray<FAssetData> BPAssets;
	AssetRegistry.GetAssetsByClass(
		UBlueprint::StaticClass()->GetClassPathName(), BPAssets, /*bSearchSubClasses=*/true);

	const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
	const FString ExportDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("BlueprintExports"));

	TSet<FString> CurrentBPNames;
	int32 ExportedCount = 0;
	int32 SkippedCount  = 0;
	int32 FilteredCount = 0;

	for (const FAssetData& AssetData : BPAssets)
	{
		const FString BPName       = AssetData.AssetName.ToString();
		const FString SanitizedName = SanitizeFileName(BPName);

		// ── 第一层：时间戳比对，跳过未修改的蓝图 ──
		const FString SummaryPath      = ExportDir / SanitizedName / TEXT("_summary.txt");
		const FDateTime ExportTimestamp = IFileManager::Get().GetTimeStamp(*SummaryPath);

		if (ExportTimestamp > FDateTime::MinValue() && CachedSummaryLikelyHasLogic(SummaryPath))
		{
			const FDateTime UassetTimestamp = GetAssetFileTimestamp(AssetData);
			if (UassetTimestamp > FDateTime::MinValue() && ExportTimestamp >= UassetTimestamp)
			{
				// .uasset 未发生变更，跳过加载
				SkippedCount++;
				CurrentBPNames.Add(SanitizedName);
				continue;
			}
		}

		// ── 需要加载并检查 ──
		UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
		if (!BP)
		{
			continue;
		}

		FExportedBlueprint ExtractedBP;
		if (!ShouldExport(BP, Settings, &ExtractedBP))
		{
			FilteredCount++;
			continue;
		}

		// ── 第二层：内容比对在 ExportBlueprintToCache 内部执行 ──
		const bool bActuallyWrote = ExportBlueprintToCache(BP, &ExtractedBP);
		ExportedCount++;
		CurrentBPNames.Add(SanitizedName);

		if (!bActuallyWrote)
		{
			UE_LOG(LogBlueprintExporter, Verbose,
				TEXT("  %s: loaded but content unchanged (timestamp drift)"), *BPName);
		}
	}

	CleanupStaleExports(CurrentBPNames);
	GenerateAgentsMd();

	UE_LOG(LogBlueprintExporter, Log,
		TEXT("ExportAll complete: %d exported, %d skipped (unchanged), %d filtered out, %d total assets"),
		ExportedCount, SkippedCount, FilteredCount, BPAssets.Num());

	// Also export FlowAssets if enabled
	if (Settings && Settings->bExportFlowAssets)
	{
		ExportAllFlowAssets();
	}
}

// Embedded content for BlueprintExports/AGENTS.md
static const TCHAR* GAgentsMdContent = TEXT(
	"## Blueprint Exports\n"
	"\n"
	"This directory contains AI-readable Blueprint exports.\n"
	"\n"
	"## Reading Order\n"
	"\n"
	"1. Locate the relevant Blueprint export folder by blueprint name.\n"
	"2. Read `<BlueprintName>/_summary.txt` first.\n"
	"3. Read `<GraphName>.txt` only when `_summary.txt` is not enough.\n"
	"\n"
	"## What `_summary.txt` Contains\n"
	"\n"
	"`_summary.txt` is the primary entry point. It merges four kinds of information:\n"
	"- Blueprint header: `=== Blueprint: Name (Parent: ParentClass) ===`\n"
	"- Asset path: `Path: /Game/...` (source asset location in content browser)\n"
	"- Interfaces: `Interfaces: Name1, Name2` (implemented blueprint interfaces, if any)\n"
	"- Component hierarchy: `=== Components ===` (blueprint-added components with attachment tree)\n"
	"- Variables: `=== Variables ===`\n"
	"- Configuration: `=== Configuration ===`\n"
	"- Compact graph flow: sections like `--- EventGraph ---` or `=== FuncName(...) ===`\n"
	"\n"
	"### Variables\n"
	"- Format: `Name : Type`, optionally `= Default`, optionally `[Flags]`.\n"
	"- Trivial defaults are omitted; visible defaults usually mean the value is intentionally set.\n"
	"\n"
	"### Configuration\n"
	"- This is exported config/state derived from the Blueprint CDO.\n"
	"- Generic blueprints use reflected CDO diff output.\n"
	"- `GameplayEffect` and `GameplayAbility` use specialized config export.\n"
	"- For specialized GAS assets, values equal to the parent default are omitted.\n"
	"- `ParentConfig: SomeParentBP` means inherited values are intentionally not repeated; inspect the parent `_summary.txt` when needed.\n"
	"\n"
	"### Components\n"
	"- `=== Components ===` shows the component hierarchy added by this blueprint.\n"
	"- Format: `Name : ClassName` optionally followed by `(Detail)` for mesh/asset names.\n"
	"- Indentation reflects parent-child attachment in the scene hierarchy.\n"
	"- Only blueprint-added components are shown; inherited components from native C++ parent classes are not listed.\n"
	"\n"
	"### Compact graph flow\n"
	"- `--- EventGraph ---` starts a compact event graph section.\n"
	"- `=== FuncName(...) ===` means a function graph summary.\n"
	"- `[K2_ActivateAbility]:` or `[ReceiveBeginPlay]:` marks an execution entry point.\n"
	"- Plain lines like `Set: EndSprint`, `Cast: GDCharacterMovementComponent`, `K2_AddGameplayCue` are semantic node summaries in execution order.\n"
	"- `BRANCH:` means a conditional branch node.\n"
	"- Tree markers `├` and `└` indicate execution subpaths.\n"
	"- Labels like `[True]`, `[False]`, `[Then 0]`, `[On Finish]`, `[On Sync]` are output exec pins.\n"
	"- `[continues...]` means the exporter detected a previously visited continuation and intentionally stopped expanding to avoid duplication/cycles.\n"
	"- `LatentAbilityCall:` means a latent/async ability task or latent node; read its child branches to understand callbacks.\n"
	"\n"
	"## What Graph Files Contain\n"
	"\n"
	"`<GraphName>.txt` is the detailed node-level export.\n"
	"\n"
	"### Graph header\n"
	"- `--- Graph: GraphName ---` means an EventGraph.\n"
	"- `--- Graph: GraphName (Function) ---` / `(Macro)` / `(Interface)` means a non-event graph.\n"
	"\n"
	"### Node block\n"
	"- Node header format: `[SemanticTitle] (ShortId)`.\n"
	"- `SemanticTitle` is human-readable and should be treated as the main identity.\n"
	"- `ShortId` is only a lightweight locator, not the primary semantic meaning.\n"
	"\n"
	"### Property lines\n"
	"- Format: `  Property: Value`.\n"
	"- These are node metadata fields that are not already encoded into the semantic title.\n"
	"\n"
	"### Pin lines\n"
	"- Output pins use `→`; input pins use `←`.\n"
	"- Format is roughly: `Arrow PinName (Type) [= Default] [-> TargetNode.TargetPin]`.\n"
	"- If `= Default` is shown, that pin has an explicit literal/default value.\n"
	"- If `-> TargetNode.TargetPin` is shown, the pin is connected to another semantic node/pin.\n"
	"- Unconnected noisy input pins are usually filtered out, so shown pins are generally meaningful.\n"
	"\n"
	"### Execution Flow section\n"
	"- `=== Execution Flow ===` is a flattened edge list.\n"
	"- `Source --> Target` means direct exec flow.\n"
	"- `Source [Label] --> Target` means flow through a labeled exec pin.\n"
	"- Use this section when you need precise node-to-node ordering beyond the compact tree in `_summary.txt`.\n"
	"\n"
	"## Naming Conventions\n"
	"\n"
	"| Prefix | Type | Reading Strategy |\n"
	"|--------|------|------------------|\n"
	"| `GA_` | GameplayAbility | Focus on Configuration + activation flow |\n"
	"| `GE_` | GameplayEffect | `_summary.txt` Configuration is usually sufficient |\n"
	"| `GC_` | GameplayCue | `_summary.txt` first; graph files only if non-trivial |\n"
	"| `BP_` | General Actor/Object | Components + Variables + graph logic |\n"
	"| `BPV_` | Visual effect blueprint | Components + EventGraph |\n"
	"\n"
	"## Practical Guidance\n"
	"\n"
	"- GAS blueprints use **diff export**: child blueprints only show values that differ from the parent. If a child looks too sparse, check `ParentConfig` and read the parent's `_summary.txt`.\n"
	"- Prefer semantic titles (e.g. `KismetMathLibrary::RandomInteger`) over raw node IDs when discussing logic.\n"
	"- `[continues...]` means the exporter stopped expanding a previously visited branch to avoid cycles.\n"
	"- Component hierarchy only includes blueprint-added components; inherited C++ components are implied by the parent class.\n"
);

void FBlueprintExporterModule::GenerateAgentsMd()
{
	const FString BaseDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("BlueprintExports"));
	const FString AgentsPath = FPaths::Combine(BaseDir, TEXT("AGENTS.md"));

	IFileManager::Get().Delete(*FPaths::Combine(BaseDir, TEXT("_index.txt")), /*RequireExists=*/false);
	IFileManager::Get().Delete(*FPaths::Combine(BaseDir, TEXT("README.md")), /*RequireExists=*/false);
	WriteFileIfChanged(AgentsPath, GAgentsMdContent);
}

void FBlueprintExporterModule::CleanupStaleExports(const TSet<FString>& CurrentBPNames)
{
	const FString BaseDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("BlueprintExports"));

	TArray<FString> SubDirs;
	IFileManager::Get().FindFiles(SubDirs, *(BaseDir / TEXT("*")), /*bFiles=*/false, /*bDirectories=*/true);

	for (const FString& SubDir : SubDirs)
	{
		if (SubDir.StartsWith(TEXT("_")))
		{
			continue;
		}

		if (!CurrentBPNames.Contains(SubDir))
		{
			const FString FullPath = FPaths::Combine(BaseDir, SubDir);
			IFileManager::Get().DeleteDirectory(*FullPath, /*RequireExists=*/false, /*Tree=*/true);
		}
	}
}

// ────────────────────────────────────────────────────────────────────────────
// FlowAsset Export
// ────────────────────────────────────────────────────────────────────────────

bool FBlueprintExporterModule::ExportFlowAssetToCache(UFlowAsset* FlowAsset)
{
	if (!FlowAsset)
	{
		return false;
	}

	FFlowAssetExtractor Extractor;
	FExportedBlueprint ExportedFA = Extractor.Extract(FlowAsset);

	FBlueprintTextFormatter Formatter;

	const FString FAName = FlowAsset->GetName();
	const FString OutputDir = FPaths::Combine(
		FPaths::ProjectDir(), TEXT("FlowAssetExports"), FAName);

	IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/true);

	int32 FilesWritten = 0;

	// Write individual graph files
	for (const FExportedGraph& Graph : ExportedFA.Graphs)
	{
		FString GraphText = Formatter.FormatGraphOnly(Graph);
		if (GraphText.IsEmpty())
		{
			continue;
		}

		const FString FileName = SanitizeFileName(Graph.GraphName) + TEXT(".txt");
		const FString FilePath = FPaths::Combine(OutputDir, FileName);
		if (WriteFileIfChanged(FilePath, GraphText))
		{
			FilesWritten++;
		}
	}

	// Write summary file
	const FString SummaryText = Formatter.FormatSummary(ExportedFA);
	const FString SummaryPath = FPaths::Combine(OutputDir, TEXT("_summary.txt"));
	if (WriteFileIfChanged(SummaryPath, SummaryText))
	{
		FilesWritten++;
	}

	if (FilesWritten > 0)
	{
		UE_LOG(LogBlueprintExporter, Log,
			TEXT("Exported FlowAsset %s: %d file(s) updated"), *FAName, FilesWritten);
	}
	else
	{
		UE_LOG(LogBlueprintExporter, Verbose,
			TEXT("Exported FlowAsset %s: no changes detected"), *FAName);
	}

	return FilesWritten > 0;
}

void FBlueprintExporterModule::ExportAllFlowAssets()
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	AssetRegistry.SearchAllAssets(/*bSynchronousSearch=*/true);

	TArray<FAssetData> FlowAssets;
	AssetRegistry.GetAssetsByClass(
		UFlowAsset::StaticClass()->GetClassPathName(), FlowAssets, /*bSearchSubClasses=*/true);

	const FString ExportDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("FlowAssetExports"));

	TSet<FString> CurrentNames;
	int32 ExportedCount = 0;
	int32 SkippedCount = 0;

	for (const FAssetData& AssetData : FlowAssets)
	{
		const FString FAName = AssetData.AssetName.ToString();
		const FString SanitizedName = SanitizeFileName(FAName);

		// Timestamp check: skip unchanged assets
		const FString SummaryPath = ExportDir / SanitizedName / TEXT("_summary.txt");
		const FDateTime ExportTimestamp = IFileManager::Get().GetTimeStamp(*SummaryPath);

		if (ExportTimestamp > FDateTime::MinValue())
		{
			const FDateTime UassetTimestamp = GetAssetFileTimestamp(AssetData);
			if (UassetTimestamp > FDateTime::MinValue() && ExportTimestamp >= UassetTimestamp)
			{
				SkippedCount++;
				CurrentNames.Add(SanitizedName);
				continue;
			}
		}

		UFlowAsset* FA = Cast<UFlowAsset>(AssetData.GetAsset());
		if (!FA)
		{
			continue;
		}

		// Check if it has any nodes worth exporting
		if (FA->GetNodes().Num() == 0)
		{
			continue;
		}

		ExportFlowAssetToCache(FA);
		ExportedCount++;
		CurrentNames.Add(SanitizedName);
	}

	CleanupStaleFlowExports(CurrentNames);
	GenerateFlowAgentsMd();

	UE_LOG(LogBlueprintExporter, Log,
		TEXT("FlowAsset ExportAll complete: %d exported, %d skipped (unchanged), %d total assets"),
		ExportedCount, SkippedCount, FlowAssets.Num());
}

static const TCHAR* GFlowAgentsMdContent = TEXT(
	"## FlowAsset Exports\n"
	"\n"
	"This directory contains AI-readable FlowAsset exports.\n"
	"\n"
	"## Reading Order\n"
	"\n"
	"1. Locate the relevant FlowAsset export folder by asset name.\n"
	"2. Read `<AssetName>/_summary.txt` first.\n"
	"3. Read `Flow_Graph.txt` only when `_summary.txt` is not enough.\n"
	"\n"
	"## What `_summary.txt` Contains\n"
	"\n"
	"`_summary.txt` is the primary entry point. It contains:\n"
	"- FlowAsset header: `=== FlowAsset: Name (ExpectedOwner: OwnerClass) ===`\n"
	"- Asset path: `Path: /Game/...` (source asset location in content browser)\n"
	"- Configuration: `=== Configuration ===` (asset-level settings like WorldBound)\n"
	"- Compact execution flow: `--- Flow Graph ---` section with tree-style execution overview\n"
	"\n"
	"### Compact execution flow\n"
	"- `[Start]:` marks the main entry point of the flow graph.\n"
	"- Node names like `Spawn AI By Time`, `Branch`, `Enable Trigger Box` are semantic node summaries.\n"
	"- Node properties are shown in parentheses: `Spawn AI By Time (TickInterval=0.5)`\n"
	"- `BRANCH:` or `Branch:` means a conditional branch node.\n"
	"- Tree markers `\\u251C` and `\\u2514` indicate execution subpaths.\n"
	"- Labels like `[True]`, `[False]`, `[Then 0]` are output pin names.\n"
	"- `[continues...]` means the exporter detected a cycle and stopped.\n"
	"- `SubGraph: AssetName` means a subgraph node referencing another FlowAsset.\n"
	"\n"
	"## What Graph Files Contain\n"
	"\n"
	"`Flow_Graph.txt` is the detailed node-level export.\n"
	"\n"
	"### Node block\n"
	"- Node header format: `[SemanticTitle] (ShortId)`.\n"
	"- `SemanticTitle` is the node's display name (e.g. `Start`, `Branch`, `Spawn AI By Time`).\n"
	"- `ShortId` is a truncated GUID for locating the node.\n"
	"\n"
	"### Property lines\n"
	"- Format: `  Property: Value`.\n"
	"- These are the node's configured UPROPERTY values that differ from defaults.\n"
	"\n"
	"### Pin lines\n"
	"- Output pins use `\\u2192`; input pins use `\\u2190`.\n"
	"- Format: `Arrow PinName (Type) [-> TargetNode.TargetPin]`.\n"
	"- Exec pins are the primary execution flow connections.\n"
	"- Data pins carry typed values between nodes.\n"
	"\n"
	"### Execution Flow section\n"
	"- `=== Execution Flow ===` is a flattened edge list.\n"
	"- `Source --> Target` means direct exec flow.\n"
	"- `Source [Label] --> Target` means flow through a labeled exec pin.\n"
	"\n"
	"## FlowNode Types\n"
	"\n"
	"| Node Class | Purpose |\n"
	"|------------|--------|\n"
	"| `FlowNode_Start` | Graph entry point |\n"
	"| `FlowNode_Finish` | Graph termination |\n"
	"| `FlowNode_Branch` | Conditional routing (uses AddOn predicates) |\n"
	"| `FlowNode_ExecutionSequence` | Execute outputs sequentially |\n"
	"| `FlowNode_SubGraph` | Spawn and execute a child FlowAsset |\n"
	"| `FlowNode_Timer` | Delay execution |\n"
	"| `FlowNode_CustomInput/Output` | Custom entry/exit points for SubGraph |\n"
	"| `FlowNode_ComponentObserver` | Observe actor components |\n"
	"| `RFlowNode_*` | Project-specific custom nodes |\n"
	"\n"
	"## Practical Guidance\n"
	"\n"
	"- Prefer semantic titles (e.g. `Spawn AI By Time`) over node GUIDs when discussing logic.\n"
	"- `SubGraph: AssetName` references another FlowAsset; read that asset's `_summary.txt` for details.\n"
	"- Node properties show only values that differ from class defaults.\n"
	"- `[continues...]` means the exporter stopped expanding to avoid cycles.\n"
);

void FBlueprintExporterModule::GenerateFlowAgentsMd()
{
	const FString BaseDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("FlowAssetExports"));
	const FString AgentsPath = FPaths::Combine(BaseDir, TEXT("AGENTS.md"));

	IFileManager::Get().MakeDirectory(*BaseDir, /*Tree=*/true);
	WriteFileIfChanged(AgentsPath, GFlowAgentsMdContent);
}

void FBlueprintExporterModule::CleanupStaleFlowExports(const TSet<FString>& CurrentNames)
{
	const FString BaseDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("FlowAssetExports"));

	TArray<FString> SubDirs;
	IFileManager::Get().FindFiles(SubDirs, *(BaseDir / TEXT("*")), /*bFiles=*/false, /*bDirectories=*/true);

	for (const FString& SubDir : SubDirs)
	{
		if (SubDir.StartsWith(TEXT("_")))
		{
			continue;
		}

		if (!CurrentNames.Contains(SubDir))
		{
			const FString FullPath = FPaths::Combine(BaseDir, SubDir);
			IFileManager::Get().DeleteDirectory(*FullPath, /*RequireExists=*/false, /*Tree=*/true);
		}
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintExporterModule, BlueprintExporter)
