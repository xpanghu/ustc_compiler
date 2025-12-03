import lldb

def parseString(val : lldb.SBValue):
    summary = val.GetSummary() or val.GetValue()
    if summary:
        if summary.startswith('"') and summary.endswith('"'):
            summary = summary[1:-1]
        return summary
    return ""

def SafePrintSummary(valobj: lldb.SBValue, internal_dict):
    if valobj.GetType().IsPointerType():
        deref: lldb.SBValue = valobj.Dereference()
        if deref.IsValid():
            valobj = deref
        else:
            return ""
    name: lldb.SBValue = valobj.EvaluateExpression("safe_print()")
    return parseString(name)

def __lldb_init_module(debugger: lldb.SBDebugger, internal_dict):
    types = ["Type", "IntegerType", "FunctionType", "ArrayType", "PointerType", "FloatType"
             , "Constant", "ConstantInt", "ConstantArray", "ConstantZero", "ConstantFP"
             , "Function", "Argument"
             , "BasicBlock"
             , "GlobalVariable"
             , "Instruction", "IBinaryInst", "FBinaryInst", "ICmpInst", "FCmpInst", "CallInst", "BranchInst", "ReturnInst", "GetElementPtrInst", "StoreInst", "LoadInst", "AllocaInst", "ZextInst", "FpToSiInst", "SiToFpInst", "PhiInst"
             , "ASMInstruction", "Reg", "FReg", "CFReg"
             , "Loop" ]
    for i in types:
        debugger.HandleCommand(
            f"type summary add -F lldb_formatters.SafePrintSummary {i} -w my"
        )
    debugger.HandleCommand("type category enable my")