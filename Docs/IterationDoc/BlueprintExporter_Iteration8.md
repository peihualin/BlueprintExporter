# BlueprintExporter 插件 — 第八次迭代需求

## 来源

- v7 双层增量检测优化后，用户反馈导出文件体积仍然较大，AI 读取时消耗 token 过多
- 对比旧版导出（`Docs/BlueprintExpoter/TestResult/ExportAll_Backup/`）发现：旧版格式化输出存在大量对齐空格和冗余信息
- 本次迭代聚焦于**输出格式精简优化**，在不丢失核心信息的前提下减少 token 消耗

---

## 目标

实现**紧凑格式化输出**机制。通过以下手段减少导出文件的 token 数：

1. 移除变量声明区的对齐空格（改为单空格分隔）
2. 移除 Graph 列表的对齐空格
3. 移除 Data Flow 独立区块（执行流已包含足够的连接信息）
4. 节点标识从 `K2Node_XXX` 改为语义化名称（如 `CallFunction_GetHealth` → `GetHealth`）
5. 引脚类型隐藏基础类型（`bool`、`int`、`float` 等不显示类型注解）
6. 新增 `_compact.txt` 文件，提供伪代码级别的高层逻辑视图

### 不做的事

- 不删除任何核心逻辑信息（节点、连接、变量、执行流程仍然完整）
- 不修改 `_summary.txt` 的 Graph 列表结构（仅移除对齐空格）
- 不自动启用 `_compact.txt`（需通过 Settings 手动开启）

---

## P0：格式化精简

### 1. 移除变量声明区的对齐空格

**问题**：旧版 `FormatVariables()` 为每个变量名和类型计算最大宽度并填充空格，使输出对齐美观但消耗大量 token。

**方案**：直接使用 `Var.Name` 和 `FullType`，不填充空格。

```cpp
// 修改前（v7）
int32 MaxNameWidth = 4;
int32 MaxTypeWidth = 4;
for (const FExportedVariable& Var : Variables)
{
    MaxNameWidth = FMath::Max(MaxNameWidth, Var.Name.Len());
    FString FullType = Var.Type;
    if (!Var.ContainerType.IsEmpty())
        FullType = FString::Printf(TEXT("%s<%s>"), *Var.ContainerType, *Var.Type);
    MaxTypeWidth = FMath::Max(MaxTypeWidth, FullType.Len());
}

for (const FExportedVariable& Var : Variables)
{
    FString FullType = ...;
    FString NamePadded = Var.Name + FString::ChrN(MaxNameWidth - Var.Name.Len(), TEXT(' '));
    FString TypePadded = FullType + FString::ChrN(MaxTypeWidth - FullType.Len(), TEXT(' '));
    FString Line = FString::Printf(TEXT("  %s : %s"), *NamePadded, *TypePadded);
}

// 修改后（v8）
for (const FExportedVariable& Var : Variables)
{
    FString FullType = ...;
    FString Line = FString::Printf(TEXT("  %s : %s"), *Var.Name, *FullType);
}
```

**影响**：
- `AC_EnemyAI/_summary.txt`：2,943 字节（旧版 5,120 字节，-42%）
- 视觉效果：从表格对齐变为紧凑列表

---

### 2. 移除 Graph 列表的对齐空格

**问题**：`FormatSummary()` 中的 Graph 列表同样使用对齐格式。

**方案**：直接输出 `GraphName`，不填充空格。

```cpp
// 修改前（v7）
int32 MaxNameWidth = 9;
for (const FExportedGraph& G : Blueprint.Graphs)
    MaxNameWidth = FMath::Max(MaxNameWidth, G.GraphName.Len());

for (const FExportedGraph& G : Blueprint.Graphs)
{
    FString NamePad = G.GraphName + FString::ChrN(MaxNameWidth - G.GraphName.Len(), TEXT(' '));
    Lines.Add(FString::Printf(TEXT("  %s  (%s)  %d nodes"), *NamePad, *G.GraphType, G.Nodes.Num()));
}

// 修改后（v8）
for (const FExportedGraph& G : Blueprint.Graphs)
{
    Lines.Add(FString::Printf(TEXT("  %s (%s) %d nodes"),
        *G.GraphName, *G.GraphType, G.Nodes.Num()));
}
```

---

### 3. 移除 Data Flow 独立区块

**问题**：`FormatGraph()` 在输出 `Execution Flow` 后会额外输出 `Data Flow` 区块，包含数据引脚连接信息。但执行流中已经包含了节点间的连接关系，Data Flow 存在信息冗余。

**方案**：删除 `FormatGraph()` 中对 `FormatDataFlow()` 的调用。

```cpp
// 修改前（v7）
FString FBlueprintTextFormatter::FormatGraph(const FExportedGraph& Graph)
{
    // ... Exec Flow ...
    Lines.Add(TEXT("=== Execution Flow ==="));
    Lines.Add(ExecFlow);
    Lines.Add(TEXT(""));

    // Data Flow -- 删除此区块
    FString DataFlow = FormatDataFlow(SortedNodes, NodeMap);
    if (!DataFlow.IsEmpty())
    {
        Lines.Add(TEXT("=== Data Flow ==="));
        Lines.Add(DataFlow);
        Lines.Add(TEXT(""));
    }
}

// 修改后（v8）
FString FBlueprintTextFormatter::FormatGraph(const FExportedGraph& Graph)
{
    // ... Exec Flow ...
    Lines.Add(TEXT("=== Execution Flow ==="));
    Lines.Add(ExecFlow);
    Lines.Add(TEXT(""));
    // Data Flow 已移除
}
```

**注意**：`FormatSelectedNodes()` 中同样删除 Data Flow 输出。

**影响**：
- 每个 Graph 文件减少约 15-25% 的行数
- 丢失的信息：数据引脚的具体连接目标（但执行流中的 `Then 0 → Node` 仍保留）

---

### 4. 节点标识语义化

**问题**：旧版使用 `K2Node_CallFunction_123` 作为节点标识，AI 需要额外上下文才能理解节点功能。

**方案**：新增 `GetSemanticTitle()` 和 `GetShortNodeId()` 方法，将节点标识改为语义化名称。

```cpp
// 新增方法
FString FBlueprintTextFormatter::GetShortNodeId(const FString& NodeName)
{
    FString Id = NodeName;
    Id.RemoveFromStart(TEXT("K2Node_"));

    // 压缩常见前缀
    static const TArray<TPair<FString, FString>> Replacements = {
        { TEXT("CallFunction"), TEXT("CallFunc") },
        { TEXT("CallArrayFunction"), TEXT("CallArrFunc") },
        { TEXT("VariableGet"), TEXT("VarGet") },
        { TEXT("VariableSet"), TEXT("VarSet") },
        { TEXT("IfThenElse"), TEXT("ITE") },
        { TEXT("FunctionEntry"), TEXT("FuncEntry") },
        { TEXT("FunctionResult"), TEXT("FuncResult") },
        { TEXT("SwitchEnum"), TEXT("Switch") },
        { TEXT("SwitchInteger"), TEXT("SwitchInt") },
        { TEXT("SwitchString"), TEXT("SwitchStr") },
        { TEXT("DynamicCast"), TEXT("Cast") },
        { TEXT("CustomEvent"), TEXT("CustEvent") },
        { TEXT("GetSubsystem"), TEXT("GetSub") },
        { TEXT("MacroInstance"), TEXT("Macro") },
        { TEXT("CommutativeAssociativeBinaryOperator"), TEXT("Op") },
        { TEXT("PromotableOperator"), TEXT("Op") },
        { TEXT("ComponentBoundEvent"), TEXT("CompEvent") },
        { TEXT("ExecutionSequence"), TEXT("Seq") },
        { TEXT("EnumEquality"), TEXT("EnumEq") },
    };

    for (const auto& Rep : Replacements)
    {
        if (Id.StartsWith(Rep.Key))
        {
            Id = Rep.Value + Id.Mid(Rep.Key.Len());
            break;
        }
    }

    return Id;
}

FString FBlueprintTextFormatter::GetSemanticTitle(const FExportedNode& Node) const
{
    const FString& NodeClass = Node.NodeClass;

    // 提取主要属性值（第一个非 Override、非 SelfContext 的属性）
    FString PrimaryValue;
    for (const auto& Prop : Node.Properties)
    {
        if (Prop.Key == TEXT("SelfContext") || Prop.Key == TEXT("Override"))
            continue;
        PrimaryValue = Prop.Value;
        break;
    }

    if (PrimaryValue.IsEmpty())
        return GetReadableType(NodeClass);

    // 根据节点类生成语义化标题
    if (NodeClass == TEXT("K2Node_Event") || NodeClass == TEXT("K2Node_CustomEvent"))
        return PrimaryValue;
    if (NodeClass == TEXT("K2Node_CallFunction") || NodeClass == TEXT("K2Node_CallArrayFunction"))
        return PrimaryValue;
    if (NodeClass == TEXT("K2Node_VariableGet"))
        return FString::Printf(TEXT("Get: %s"), *PrimaryValue);
    if (NodeClass == TEXT("K2Node_VariableSet"))
        return FString::Printf(TEXT("Set: %s"), *PrimaryValue);
    if (NodeClass == TEXT("K2Node_DynamicCast"))
        return FString::Printf(TEXT("Cast: %s"), *PrimaryValue);
    if (NodeClass == TEXT("K2Node_SwitchEnum") || NodeClass == TEXT("K2Node_SwitchInteger")
        || NodeClass == TEXT("K2Node_SwitchString"))
        return FString::Printf(TEXT("Switch: %s"), *PrimaryValue);
    if (NodeClass == TEXT("K2Node_MacroInstance"))
        return FString::Printf(TEXT("Macro: %s"), *PrimaryValue);
    if (NodeClass == TEXT("K2Node_Timeline"))
        return FString::Printf(TEXT("Timeline: %s"), *PrimaryValue);
    if (NodeClass == TEXT("K2Node_Composite"))
        return FString::Printf(TEXT("Collapsed: %s"), *PrimaryValue);
    if (NodeClass == TEXT("K2Node_ComponentBoundEvent"))
    {
        FString DelegateName;
        for (const auto& Prop : Node.Properties)
        {
            if (Prop.Key == TEXT("DelegateProperty"))
            {
                DelegateName = Prop.Value;
                break;
            }
        }
        if (!DelegateName.IsEmpty())
            return FString::Printf(TEXT("On %s.%s"), *PrimaryValue, *DelegateName);
        return PrimaryValue;
    }
    if (NodeClass == TEXT("K2Node_IfThenElse"))
        return TEXT("Branch");

    return GetReadableType(NodeClass);
}
```

**使用方式**：

```cpp
// FormatNode 修改
FString FBlueprintTextFormatter::FormatNode(const FExportedNode& Node)
{
    TArray<FString> Lines;

    // 紧凑节点头：[SemanticTitle] (ShortId)
    FString SemanticTitle = GetSemanticTitle(Node);
    FString ShortId = GetShortNodeId(Node.NodeName);
    Lines.Add(FString::Printf(TEXT("[%s] (%s)"), *SemanticTitle, *ShortId));

    // 节点属性 -- 跳过已被 GetSemanticTitle 消耗的键
    static const TSet<FString> ConsumedKeys = {
        TEXT("Event"), TEXT("Function"), TEXT("Variable"), TEXT("CastTo"),
        TEXT("Macro"), TEXT("Timeline"), TEXT("Enum"), TEXT("Collapsed"),
        TEXT("ComponentProperty"), TEXT("DelegateProperty"),
    };

    bool bHasOverride = false;
    for (const auto& Prop : Node.Properties)
    {
        if (Prop.Key == TEXT("SelfContext")) continue;
        if (Prop.Key == TEXT("Override") && Prop.Value == TEXT("true"))
        {
            bHasOverride = true;
            continue;
        }
        if (ConsumedKeys.Contains(Prop.Key)) continue;
        Lines.Add(FString::Printf(TEXT("  %s: %s"), *Prop.Key, *Prop.Value));
    }
    if (bHasOverride)
        Lines.Add(TEXT("  (Override)"));

    // Meaningful pins...
}
```

**影响**：
- `K2Node_CallFunction_123` → `[GetHealth] (CallFunc)`
- `K2Node_IfThenElse_456` → `[Branch] (ITE)`
- `K2Node_VariableGet_789` → `[Get: CurrentAIState] (VarGet)`
- AI 无需查看节点详情即可理解节点功能

---

### 5. 引脚类型隐藏基础类型

**问题**：基础类型（`bool`、`int`、`float` 等）的引脚每次都显示类型注解，造成冗余。

**方案**：对基础类型不显示类型注解。

```cpp
FString FBlueprintTextFormatter::FormatPin(const FExportedPin& Pin)
{
    FString TypeStr = ...;

    // 抑制基础类型的类型注解
    static const TSet<FString> PrimitiveTypes = {
        TEXT("bool"), TEXT("int"), TEXT("int64"), TEXT("float"), TEXT("double"),
        TEXT("byte"), TEXT("real"), TEXT("string"), TEXT("text"), TEXT("name"),
    };
    bool bShowType = !PrimitiveTypes.Contains(TypeStr);

    // Default value...
    FString Default = ...;

    // Connection...
    FString Connection = ...;

    if (bShowType)
    {
        return FString::Printf(TEXT("%s %s (%s)%s%s"),
            Arrow, *Pin.Name, *TypeStr, *Default, *Connection);
    }
    else
    {
        return FString::Printf(TEXT("%s %s%s%s"),
            Arrow, *Pin.Name, *Default, *Connection);
    }
}
```

**影响**：
- `-> bIsAlive (bool)` → `-> bIsAlive`
- `-> Damage (float)` → `-> Damage`
- 复杂类型（`Actor`、`Vector`、`Array<FString>`）仍显示类型

---

## P1：Compact 模式（伪代码视图）

### 6. 新增 `_compact.txt` 文件

**问题**：`_summary.txt` 只包含变量表和 Graph 列表，无法了解逻辑流程；单个 Graph 文件内容太长，AI 需要快速了解蓝图的高层逻辑结构。

**方案**：新增 `FormatCompactBlueprint()` 和 `FormatCompactGraph()` 方法，生成伪代码风格的紧凑输出。

```cpp
// 新增方法声明 (BlueprintTextFormatter.h)
UCLASS()
class FBlueprintTextFormatter
{
    // ... 现有方法 ...

    /** 紧凑模式：生成伪代码风格的蓝图摘要 */
    FString FormatCompactBlueprint(const FExportedBlueprint& Blueprint);
    FString FormatCompactGraph(const FExportedGraph& Graph);

private:
    bool HasExecPins(const FExportedNode& Node) const;
    FString ExtractFunctionSignature(const FExportedGraph& Graph);
    FString GetShortNodeId(const FString& NodeName);
    FString GetSemanticTitle(const FExportedNode& Node) const;
};
```

**Compact 输出示例**：

```
=== AC_EnemyAI (Parent: CA_EnemyAIComponentBase) ===

--- EventGraph ---
[Add Target Event]:
Collapsed: Target On Sight
[Attack Event]:
Collapsed: Attack Target
[BeingLocked]:
SetBeingLocked
[Death Event]:
Collapsed: Stop Movement
[Defend Event]:
Collapsed: Strafe Around Target
[Investigate Event]:
Collapsed: Investigate Area
[Match Player Inputs]:
Collapsed: Update Inputs
[On Sight Event]:
Collapsed: AI Pilot
[OnAttackSlotGranted_Handler]:
CallParentFunction
[OnAttackSlotRevoked_Handler]:
CallParentFunction
[OnEnterAttackState]:
CallParentFunction
Collapsed: Attack State Info
[OnEnterDefendState]:
CallParentFunction
Collapsed: Defend State Info
[OnReadyToAttack]:
CallParentFunction
Attack Event
[OnReadyToDefend]:
CallParentFunction
StartStrafeTimer
Defend Event
[Play Montage Event]:
Collapsed: Play Montage
[Play Special Montage]:
Collapsed: Play Special Attack Montage
[Receive Threat Alert]:
Collapsed: Set Action To Do
[ReceiveBeginPlay]:
Collapsed: Begin Play Event
[Relax Event]:
Collapsed: Patrol Or Idle

=== TargetCondition(Target:Actor) -> Is Alive:bool, Is Enemy:bool ===
Macro: IsValid:
├ [Is Valid]:
  Macro: IsValid:
  ├ [Is Valid]: return(Is Alive=Get: IsAlive, Is Enemy=Actor::ActorHasTag)
  └ [Is Not Valid]: return(Is Alive, Is Enemy)
  └ [Is Not Valid]: [continues...]

=== StartStrafe() ===
BRANCH:
└ [True]:
  ExecutionSequence:
  ├ [Then 0]:
    BRANCH:
    └ [True]:
      BRANCH:
      └ [True]:
        StartStrafeTimer
        ComputeStrafeDestination
        Set: MoveDestination
        Set: L_StrafeRes
        BRANCH:
        ├ [True]: MoveAI
        └ [False]: StopStrafe
  └ [Then 1]: Match Player Inputs
```

**关键设计**：

1. **事件驱动结构**：按事件节点（Event/FunctionEntry/CustomEvent）组织输出
2. **DFS 遍历**：从每个入口点开始深度优先搜索，展示执行流程
3. **折叠节点**：Collapsed 节点只显示名称，不展开内部细节
4. **ASCII 流程图**：使用 `├`、`└`、`→` 等字符展示分支和连接

**文件**：
- `BlueprintTextFormatter.cpp`（新增 `FormatCompactBlueprint()`、`FormatCompactGraph()`、`HasExecPins()`、`ExtractFunctionSignature()` 等方法）
- `BlueprintExporterModule.cpp`（在 `ExportBlueprintToCache()` 末尾调用 `FormatCompactBlueprint()` 并写入 `_compact.txt`）

---

### 7. 可配置启用 Compact 文件

**方案**：在 `UBlueprintExporterSettings` 中新增开关。

```cpp
// BlueprintExporterSettings.h
UPROPERTY(Config, EditAnywhere, Category="Output Format", meta=(DisplayName="Generate Compact File"))
bool bGenerateCompactFile = true;
```

**修改 `ExportBlueprintToCache()`**：

```cpp
// Write compact file (if enabled)
const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
if (Settings && Settings->bGenerateCompactFile)
{
    const FString CompactText = Formatter.FormatCompactBlueprint(ExportedBP);
    const FString CompactPath = FPaths::Combine(OutputDir, TEXT("_compact.txt"));
    if (WriteFileIfChanged(CompactPath, CompactText))
    {
        FilesWritten++;
    }
}
```

---

## 修改范围

| 文件 | 操作 | 说明 |
|------|------|------|
| `Private/BlueprintTextFormatter.cpp` | 修改 | 新增紧凑格式化方法、语义化节点标识、移除对齐空格、移除 Data Flow |
| `Public/BlueprintTextFormatter.h` | 修改 | 新增方法声明（`FormatCompactBlueprint`、`FormatCompactGraph`、`GetSemanticTitle`、`GetShortNodeId` 等） |
| `Private/BlueprintExporterModule.cpp` | 修改 | 在 `ExportBlueprintToCache()` 中调用 Compact 格式化并写入文件 |
| `Public/BlueprintExporterSettings.h` | 修改 | 新增 `bGenerateCompactFile` 配置项 |

---

## 边界情况

1. **空 Graph**：`FormatCompactGraph()` 返回空字符串，不生成输出
2. **折叠节点**：只显示 `Collapsed: XXX`，不展开内部细节（与旧版一致）
3. **宏和接口**：`FormatCompactGraph()` 使用 `ExtractFunctionSignature()` 生成函数签名
4. **事件图**：按事件节点字母顺序排列，FunctionEntry 优先

---

## 验证方法

### 性能验证

1. **全量导出体积对比**
   ```
   旧版：11,910,342 字节
   新版： 9,460,332 字节
   节省：-20.6%
   ```

2. **核心蓝图对比**
   ```
   AC_EnemyAI:
     旧版：284,379 字节
     新版：156,956 字节 (-45%)

   AC_HitReaction:
     旧版：221,790 字节
     新版：169,780 字节 (-23%)

   ABP_CA_Hero:
     旧版：250,458 字节
     新版：185,474 字节 (-25%)
   ```

3. **EventGraph 词数对比**
   ```
   AC_EnemyAI/EventGraph:
     旧版：9,185 词
     新版：7,541 词 (-18%)
   ```

### 信息完整性验证

1. **Graph 列表一致性**
   - 新旧版 `_summary.txt` 中的 Graph 列表应完全一致（仅格式不同）
   - 每个 Graph 文件应存在且内容完整

2. **执行流一致性**
   - 新旧版 `EventGraph.txt` 中的执行流应包含相同的节点连接关系
   - Compact 模式应正确展示高层逻辑结构

3. **Compact 文件可读性**
   - AI 应能通过 `_compact.txt` 快速理解蓝图的核心逻辑
   - 折叠节点应正确显示为 `Collapsed: XXX`

### 端到端验证（模拟 ClaudeCode 工作流）

1. 在编辑器中保存蓝图后关闭编辑器
2. 读取 `Saved/BlueprintExports/_index.txt` 查看全局索引
3. 读取 `Saved/BlueprintExports/AC_EnemyAI/_summary.txt` 查看蓝图结构
4. 读取 `Saved/BlueprintExports/AC_EnemyAI/_compact.txt` 查看高层逻辑
5. 读取 `Saved/BlueprintExports/AC_EnemyAI/EventGraph.txt` 查看具体实现
6. 确认每一层的信息足够用于 AI 理解，且 token 消耗显著降低

---

## 实施优先级总结

| 优先级 | 需求 | 工作量估计 | 实际节省 |
|--------|------|------------|----------|
| P0 | #1 移除变量对齐空格 | 小 | -2% |
| P0 | #2 移除 Graph 列表对齐 | 小 | -1% |
| P0 | #3 移除 Data Flow 区块 | 小 | -10% |
| P0 | #4 节点标识语义化 | 中 | -5% |
| P0 | #5 引脚隐藏基础类型 | 小 | -3% |
| P1 | #6 Compact 模式 | 中 | 新增功能 |
| P1 | #7 可配置启用 Compact | 小 | - |

**合计节省**：约 20-28%（取决于蓝图的复杂程度）

---

## 与 v7 的兼容性

v8 的改动完全向后兼容：
- 导出目录结构不变（`Saved/BlueprintExports/{BPName}/`）
- `_summary.txt` 格式仅移除对齐空格，内容不变
- 单个 Graph 文件的执行流格式保持一致（仅节点标识更语义化）
- `_compact.txt` 是新增文件，不影响现有读取逻辑

**注意**：如果用户依赖旧版格式化输出的对齐格式（如解析脚本），需要更新脚本以适应 v8 的紧凑格式。
