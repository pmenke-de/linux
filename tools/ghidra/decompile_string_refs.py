# Decompile functions that reference matching strings.
# Usage from analyzeHeadless:
#   -postScript decompile_string_refs.py pattern1 pattern2 ...

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


def iter_defined_data():
    listing = currentProgram.getListing()
    it = listing.getDefinedData(True)
    while it.hasNext():
        yield it.next()


def string_value(data):
    try:
        value = data.getValue()
        if value is None:
            return None
        text = str(value)
        if text:
            return text
    except:
        return None
    return None


def matching_data(patterns):
    matches = []
    for data in iter_defined_data():
        text = string_value(data)
        if not text:
            continue
        for pat in patterns:
            if pat in text:
                matches.append((data, text))
                break
    return matches


def funcs_for_matches(matches):
    funcs = {}
    for data, text in matches:
        refs = getReferencesTo(data.getAddress())
        for ref in refs:
            func = getFunctionContaining(ref.getFromAddress())
            if func is None:
                continue
            key = func.getEntryPoint().toString()
            if key not in funcs:
                funcs[key] = (func, [])
            funcs[key][1].append((data.getAddress(), text, ref.getFromAddress()))
    return funcs


def decompile_functions(func_map):
    ifc = DecompInterface()
    ifc.openProgram(currentProgram)
    monitor = ConsoleTaskMonitor()

    for key in sorted(func_map.keys()):
        func, refs = func_map[key]
        print("=== FUNCTION %s %s ===" % (func.getName(), func.getEntryPoint()))
        for saddr, text, fromaddr in refs:
            print("REF %s -> %s" % (fromaddr, saddr))
            print("STR %s" % text)
        res = ifc.decompileFunction(func, 60, monitor)
        if not res.decompileCompleted():
            print("DECOMPILATION FAILED: %s" % res.getErrorMessage())
            continue
        print(res.getDecompiledFunction().getC())
        print("")

    ifc.dispose()


if __name__ == "__main__":
    args = getScriptArgs()
    if not args:
        print("No patterns provided")
        exit(1)

    matches = matching_data(args)
    print("Matched %d strings" % len(matches))
    funcs = funcs_for_matches(matches)
    print("Matched %d functions" % len(funcs))
    decompile_functions(funcs)
