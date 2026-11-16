// Standalone Windows test of the production installation and path guards.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include "../Reloaded.Editor/resource.h"
namespace { bool testPackageLoaded=false; }
void* Find(const std::string&) { return testPackageLoaded?reinterpret_cast<void*>(1):nullptr; }
bool IsA(void*,const char*) { return true; }
bool Exec(const std::string&) { return true; }
std::filesystem::path Directory() { return {}; }
namespace Logger { void log(const std::string&) {} }
#include "../Reloaded.Editor/PS2ObjectiveNative.inl"
int main(int argc,char**argv)
{
    try {
        if(argc!=2)throw std::runtime_error("Pass an unused task-owned test directory.");
        auto root=std::filesystem::absolute(argv[1]);
        if(std::filesystem::exists(root))throw std::runtime_error("Test directory already exists.");
        std::filesystem::create_directories(root);
        unsigned checks=0;
        auto check=[&](bool ok){if(!ok)throw std::runtime_error("Resource guard check failed.");++checks;};
        auto reject=[&](auto action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed);};
        const PS2Bytes first={'o','l','d'},second={'n','e','w'};
        auto target=PS2BoundedPath(root,"Packages/Textures/P2_test.utx");
        check(target.lexically_relative(root)==std::filesystem::path("Packages/Textures/P2_test.utx"));
        reject([&]{PS2BoundedPath(root,"../outside.utx");});
        PS2Install(target,first);check(PS2Read(target)==first);
        const auto saved=std::filesystem::last_write_time(target);
        PS2Install(target,first);check(std::filesystem::last_write_time(target)==saved);
        testPackageLoaded=true;reject([&]{PS2Install(target,second);});check(PS2Read(target)==first);
        testPackageLoaded=false;PS2Install(target,second);check(PS2Read(target)==second);
        auto backup=std::filesystem::path(target.wstring()+L".replus-"+std::filesystem::path(PS2Sha256(first).substr(0,12)).wstring()+L".bak");
        check(PS2Read(backup)==first);
        auto pending=std::filesystem::path(target.wstring()+L".replus-new");
        {std::ofstream out(pending);out<<"pending";}
        reject([&]{PS2Install(target,first);});check(PS2Read(target)==second);
        check(PS2Sha256(PS2Bytes{'a','b','c'})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        std::cout<<"PASS "<<checks<<" production resource/path guard checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
