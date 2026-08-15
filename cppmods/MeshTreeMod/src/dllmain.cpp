// MeshTreeMod: Windows DLL entry point (UE4SS loads this module).

#include <MeshTreeMod.hpp>

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new RC::MeshTreeMod::MeshTreeMod();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
