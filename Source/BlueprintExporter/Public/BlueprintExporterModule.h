#pragma once

#include "Modules/ModuleManager.h"

class UBlueprintExporterSettings;
class UFlowAsset;
struct FExportedBlueprint;

class FBlueprintExporterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void RegisterGraphEditorExtender();
	void OnModulesChanged(FName ModuleName, EModuleChangeReason Reason);
	void ExportSelectedBlueprints(const TArray<FAssetData>& SelectedAssets);
	void CopySelectedNodesToClipboard(const UEdGraph* Graph);
	void ExportSelectedNodesToFile(const UEdGraph* Graph);

	// --- Iteration 6 ---
	void OnPackageSaved(const FString& PackageFilename, UPackage* Package,
	                    FObjectPostSaveContext SaveContext);
	void OnEditorPreExit();
	bool ExportBlueprintToCache(UBlueprint* Blueprint, FExportedBlueprint* PreExtracted = nullptr);
	void ExportAllBlueprints();
	void GenerateAgentsMd();
	void CleanupStaleExports(const TSet<FString>& CurrentBPNames);
	bool ShouldExport(UBlueprint* Blueprint, const UBlueprintExporterSettings* Settings,
	                  FExportedBlueprint* OutExtracted = nullptr) const;
	static FString SanitizeFileName(const FString& Name);
	static bool WriteFileIfChanged(const FString& FilePath, const FString& Content);
	static FDateTime GetAssetFileTimestamp(const FAssetData& AssetData);

	// --- FlowAsset Export ---
	bool ExportFlowAssetToCache(UFlowAsset* FlowAsset);
	void ExportAllFlowAssets();
	void GenerateFlowAgentsMd();
	void CleanupStaleFlowExports(const TSet<FString>& CurrentNames);

	int32 GraphEditorMenuExtenderIndex = INDEX_NONE;
	FDelegateHandle ModulesChangedHandle;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle PreExitHandle;
};
