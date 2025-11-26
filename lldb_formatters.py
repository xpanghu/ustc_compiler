import lldb

def parseString(val : lldb.SBValue):
    summary = val.GetSummary() or val.GetValue()
    if summary:
        # 去掉两端引号，只留裸文本（可选）
        if summary.startswith('"') and summary.endswith('"'):
            summary = summary[1:-1]
        return summary
    return ""

def TypePrinterSummary(valobj: lldb.SBValue, internal_dict):
    # 调用 print 函数进行显示
    name: lldb.SBValue = valobj.EvaluateExpression("print()");
    return parseString(name)

def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    types = ["Type", "IntegerType", "FunctionType", "ArrayType", "PointerType", "FloatType"]
    for i in types:
        debugger.HandleCommand(
            f"type summary add -F lldb_formatters.TypePrinterSummary {i} -w my"
        )
    debugger.HandleCommand("type category enable my")