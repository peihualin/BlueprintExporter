# BlueprintExporter

UE5 Editor-only 插件，将 Blueprint 和 [FlowGraph](https://github.com/MothCocoon/FlowGraph) FlowAsset 的节点图导出为 AI 可读的纯文本格式。

## 功能

|功能|触发方式|输出目录|
|-|-|-|
|蓝图按需导出|Content Browser 右键蓝图 → Export Blueprint Logic|弹出保存对话框|
|蓝图批量导出|Content Browser 右键蓝图 → Export All Blueprints to Cache|`BlueprintExports/`|
|蓝图自动导出|Ctrl+S 保存时触发|`BlueprintExports/`|
|蓝图选中节点导出|蓝图编辑器内右键节点 → Copy / Export Selected Nodes|剪贴板 / 文件|
|FlowAsset 按需导出|Content Browser 右键 FlowAsset → Export FlowAsset Logic|弹出保存对话框|
|FlowAsset 批量导出|Content Browser 右键 FlowAsset → Export All FlowAssets to Cache|`FlowAssetExports/`|
|FlowAsset 自动导出|Ctrl+S 保存时触发|`FlowAssetExports/`|
|编辑器关闭时全量导出|关闭编辑器前自动触发|两个目录|

## 输出目录结构

```
{ProjectDir}/
├── BlueprintExports/
│   ├── AGENTS.md                    ← AI 引导文件（自动生成）
│   ├── BP_SomeActor/
│   │   ├── _summary.txt             ← 先读这个：紧凑执行流 + 变量 + 组件 + 配置
│   │   ├── EventGraph.txt           ← 完整节点/引脚/连接
│   │   └── SomeFunction.txt
│   └── GE_Damage/
│       └── _summary.txt             ← GAS 资产通常只有配置
│
└── FlowAssetExports/
    ├── AGENTS.md
    └── FG_AiC4/
        ├── _summary.txt             ← 紧凑执行流树
        └── Flow_Graph.txt           ← 完整节点详情
```

**阅读顺序**：先读 `_summary.txt`，只在需要节点级细节时才读各图文件。

## Settings 配置

位置：`Editor Preferences → Plugins → Blueprint Exporter`

### Auto Export

|属性|默认值|说明|
|-|-|-|
|`bAutoExportOnSave`|`false`|保存蓝图时自动导出|
|`bExportOnEditorClose`|`false`|关闭编辑器前全量导出（含蓝图和 FlowAsset）|

### FlowAsset Export

|属性|默认值|说明|
|-|-|-|
|`bExportFlowAssets`|`true`|FlowAsset 导出总开关|
|`bAutoExportFlowAssetOnSave`|`false`|保存 FlowAsset 时自动导出|

### Export Filter — Blueprint

|属性|默认值|说明|
|-|-|-|
|`bExportNormalBlueprint`|`true`|导出普通蓝图|
|`bExportFunctionLibrary`|`true`|导出函数库蓝图|
|`bExportMacroLibrary`|`false`|导出宏库蓝图|
|`bExportInterface`|`false`|导出接口蓝图|
|`bExportLevelScript`|`false`|导出关卡脚本蓝图|
|`ParentClassFilter`|空|白名单：只导出继承自列表中类的蓝图，留空不限制|
|`ExcludedParentClasses`|空|黑名单：排除继承自列表中类的蓝图，优先级高于白名单|
|`ConfigOnlyParentClassWhitelist`|GAS 相关类|无图逻辑但有 CDO 差异时，仅允许这些父类导出|
|`MinNodeCount`|`0`|有图逻辑的蓝图节点数低于此值时跳过|

## 输出格式

### Blueprint `_summary.txt`

```
=== Blueprint: BP_DamageVolume (Parent: Actor) ===
Path: /Game/Blueprints/BP_DamageVolume
Interfaces: BPI_Damageable

=== Components ===
  DefaultSceneRoot : SceneComponent
  DamageVolume : BoxComponent

=== Variables ===
  Damage : double = 25.000000

--- EventGraph ---
[ReceiveBeginPlay]:
KismetSystemLibrary::K2_SetTimerDelegate
```

### FlowAsset `_summary.txt`

```
=== FlowAsset: FG_AiC4 (ExpectedOwner: FlowComponent) ===
Path: /Game/Blueprints/FlowGraph/StreetFight/FG_AiC4

--- Flow Graph ---
[Start]:
Sequence:
├ [0]:
  On Trigger Event [Trigger.AiC4A1]:
  └ [Enter]:
    Enable Trigger Box [Trigger.AiC4A2]:
    └ [End]:
      Notify Actor [C4.A1]:
      └ [Out]: Notify Actor [SpawnPoint.C4AiA1]
└ [2]:
  On Trigger Event [Trigger.AIC4B325]:
  └ [Enter]:
    Notify Actor [C4.B325]:
    └ [Out]: Notify Actor [SpawnPoint.C4B325]
```

### 详细图文件格式

```
[SemanticTitle] (ShortId)
  Property: Value
  → OutputPin (Type) -> TargetNode.TargetPin
  ← InputPin (Type) = DefaultValue

=== Execution Flow ===
  SourceNode [PinLabel] --> TargetNode
```

## 技术架构

### 模块结构

```
BlueprintExporter/
├── BlueprintExporter.uplugin
└── Source/BlueprintExporter/
    ├── BlueprintExporter.Build.cs
    ├── Public/
    │   ├── BlueprintExporterModule.h       ← 模块入口，菜单注册，事件绑定
    │   ├── BlueprintExporterSettings.h     ← UDeveloperSettings 配置
    │   ├── BlueprintExporterTypes.h        ← 中间数据结构
    │   ├── BlueprintGraphExtractor.h       ← UBlueprint → FExportedBlueprint
    │   ├── BlueprintTextFormatter.h        ← FExportedBlueprint → 文本
    │   └── FlowAssetExtractor.h            ← UFlowAsset → FExportedBlueprint
    └── Private/
        ├── BlueprintExporterModule.cpp
        ├── BlueprintExporterSettings.cpp
        ├── BlueprintGraphExtractor.cpp
        ├── BlueprintTextFormatter.cpp
        └── FlowAssetExtractor.cpp
```

### 数据流

```
UBlueprint ──► FBlueprintGraphExtractor::Extract() ──► FExportedBlueprint ──┐
                                                                            ├─► FBlueprintTextFormatter ──► 文本文件
UFlowAsset ──► FFlowAssetExtractor::Extract() ─────► FExportedBlueprint ──┘
```

Blueprint 和 FlowAsset 共享同一套中间数据结构（`FExportedBlueprint` / `FExportedGraph` / `FExportedNode` / `FExportedPin`）和格式化器（`FBlueprintTextFormatter`），但提取逻辑完全独立。

### 模块依赖

```
Public:  Core, CoreUObject, Engine
Private: UnrealEd, BlueprintGraph, KismetCompiler, Kismet, GraphEditor,
         ToolMenus, ContentBrowser, AssetTools, Slate, SlateCore, InputCore,
         DesktopPlatform, ApplicationCore, AssetRegistry, DeveloperSettings,
         GameplayAbilities, GameplayTags, Flow
```

### 增量检测

批量导出和自动导出使用双层增量检测：

1. **时间戳比对**：`_summary.txt` 修改时间 >= `.uasset` 修改时间则跳过，不加载资产
2. **内容比对**：`WriteFileIfChanged()` 只在内容变化时写入，保持时间戳语义准确

全量导出完成后 `CleanupStaleExports()` 自动清理已删除资产的残留导出目录。

## FlowAsset 导出细节

### 节点消歧

FlowGraph 中经常出现多个同类型节点（如多个 `Notify Actor`）。当同一图内存在重复 DisplayName 时，自动从 IdentityTags 等属性提取简短标识附加到标题：

- `On Trigger Event [Trigger.AiC4A1]`
- `Notify Actor [C4.A1]`
- `Notify Actor [SpawnPoint.C4AiA1]`

此消歧机制仅对 FlowGraph 图类型启用，不影响蓝图导出。

### 节点属性提取

通过 UObject 反射提取 `UFlowNode` 子类的 UPROPERTY：
- 只导出 `EditAnywhere` / `EditDefaultsOnly` / `BlueprintVisible` 属性
- 跳过基类属性（InputPins, OutputPins, Connections, NodeGuid 等）和 Transient 属性
- 差异导出：只输出与类默认值不同的值
- GameplayTag/Container 直接输出标签字符串，枚举输出显示名

### FlowNode 类型映射

|节点类|输出标记|
|-|-|
|`FlowNode_Start`|`START`|
|`FlowNode_Finish`|`FINISH`|
|`FlowNode_Branch`|`BRANCH`|
|`FlowNode_ExecutionSequence`|`SEQUENCE`|
|`FlowNode_SubGraph`|`SUB_GRAPH`|
|`FlowNode_Timer`|`TIMER`|
|`FlowNode_CustomInput / CustomOutput`|`CUSTOM_INPUT / CUSTOM_OUTPUT`|
|`FlowNode_Reroute`|`REROUTE`（自动穿透，不输出）|
|`RFlowNode_*`|去掉前缀|

### 与蓝图导出的隔离

- 独立输出目录：`FlowAssetExports/` vs `BlueprintExports/`
- 独立 `AGENTS.md`、独立 Settings 开关
- 消歧机制仅对 `FlowGraph` 图类型启用
- 蓝图导出路径的行为完全不受 FlowAsset 代码影响

## 注意事项

- **`_C` 后缀**：蓝图生成类名的 `_C` 后缀在 ParentClass、Pin SubType、Cast 目标、DefaultValue 中统一去除
- **枚举显示名**：枚举值通过 `GetDisplayNameTextByValue()` 转为可读名
- **Reroute 穿透**：Blueprint `K2Node_Knot` 和 FlowGraph `FlowNode_Reroute` 均自动穿透，带循环检测
- **导出目录**：建议加入 `.gitignore` / `.p4ignore`，但不要加入 `.cursorignore`（需被 AI 索引）
- **编译错误蓝图**：仍可遍历导出，部分连接可能不完整，提取器使用空指针守卫保护

## License

MIT
