-- The call returns the hash-table singleton used by the HashOuter removal path.
function Register()
    return "4C 89 64 24 70 4C 89 6C 24 68 E8 ?? ?? ?? ?? 4C 8B E8 45 8B E7 E8 ?? ?? ?? ?? 84 C0 74 0E E8"
end
function OnMatchFound(address)
    return address + 15 + DerefToInt32(address + 11)
end
