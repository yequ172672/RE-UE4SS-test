-- ProcessLocalScriptFunction loads the bytecode dispatch table with RIP-relative LEA.
function Register()
    return "48 8B 42 20 4C 8D 35 ?? ?? ?? ?? 48 8B 72 10 49 8B F8 48 8B DA 80 38 04 74"
end
function OnMatchFound(address)
    return address + 11 + DerefToInt32(address + 7)
end
