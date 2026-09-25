#include "../D3D9PresentHook.h"
#include "../RenderCameraProbe.h"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace chaosvr::bridge32 {
namespace {
using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                               const RGNDATA*);
using SetTransformFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetVertexShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

std::atomic<Direct3DCreate9Fn> g_create9{};
std::atomic<CreateDeviceFn> g_createDevice{};
std::atomic<PresentFn> g_present{};
std::atomic<SetTransformFn> g_setTransform{};
std::atomic<SetVertexShaderConstantFFn> g_setVsConstF{};
std::atomic<uint64_t> g_presentId{};
std::atomic<uint64_t> g_setTransformCalls{};
std::atomic<uint64_t> g_viewTransformCalls{};
std::atomic<uint64_t> g_projectionTransformCalls{};
std::atomic<uint64_t> g_viewOverrideCalls{};
std::atomic<uint64_t> g_shaderViewOverrideCalls{};
std::atomic<uint64_t> g_shaderViewRejectedNonRigid{};
std::atomic<int> g_shaderViewTargetRegister{-1};
std::atomic<bool> g_d3d9ExDevice{};
std::atomic<uint64_t> g_vsConstFCalls{};
std::atomic<uint64_t> g_matrixVsConstFCalls{};
std::atomic<uint64_t> g_likelyVsPerspective{};
std::atomic<uint64_t> g_likelyVsRigidView{};
std::array<std::atomic<uint64_t>,256> g_matrixRegisterCalls{};
std::array<std::atomic<uint64_t>,256> g_perspectiveRegisterCalls{};
std::array<std::atomic<uint64_t>,256> g_rigidViewRegisterCalls{};
std::atomic<bool> g_renderPathProbeInstalled{};
std::mutex g_ownerMutex;
D3D9HookOwnership g_ownership{};
std::mutex g_viewOverrideMutex;
rendercam::Mat4 g_viewDelta=rendercam::Identity();
bool g_viewOverrideEnabled=false;
bool g_shaderViewOverrideEnabled=false;
bool g_hasLastViewEye=false;
float g_lastViewEyeX=0.0f, g_lastViewEyeY=0.0f, g_lastViewEyeZ=0.0f;
PresentTimingWriter* g_writer{};
void** g_create9IatSlot{};
void** g_createDeviceVtableSlot{};
void** g_presentVtableSlot{};
void** g_setTransformVtableSlot{};
void** g_setVsConstFVtableSlot{};


std::wstring ModulePathForAddress(const void* p) {
    if (!p) return {};
    HMODULE m{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(p), &m)) return {};
    wchar_t path[1024]{};
    const DWORD n=GetModuleFileNameW(m,path,static_cast<DWORD>(sizeof(path)/sizeof(path[0])));
    return n?std::wstring(path,n):std::wstring{};
}
void SetOwner(std::wstring D3D9HookOwnership::* member,const void* p) {
    std::scoped_lock lock(g_ownerMutex);
    g_ownership.*member=ModulePathForAddress(p);
}

int64_t QpcNow() noexcept {
    LARGE_INTEGER q{};
    return QueryPerformanceCounter(&q) ? q.QuadPart : 0;
}

bool WritePointer(void** slot, void* value) noexcept {
    if (!slot) return false;
    DWORD old{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = value;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

// Finds an import by name when OriginalFirstThunk is present. If it is absent,
// fall back to matching the resolved function pointer in FirstThunk. The pointer
// fallback matters for bound/stripped PE import tables.
void** FindImportSlot(HMODULE module, const char* dllName, const char* procName, void* resolvedFallback) noexcept {
    if (!module) return nullptr;
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* importedDll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(importedDll, dllName) != 0) continue;

        auto* first = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto* original = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
            : nullptr;

        for (size_t i = 0; first[i].u1.Function; ++i) {
            if (original) {
                if (IMAGE_SNAP_BY_ORDINAL(original[i].u1.Ordinal)) continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original[i].u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name), procName) == 0)
                    return reinterpret_cast<void**>(&first[i].u1.Function);
            } else if (resolvedFallback && reinterpret_cast<void*>(first[i].u1.Function) == resolvedFallback) {
                return reinterpret_cast<void**>(&first[i].u1.Function);
            }
        }
    }
    return nullptr;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst,
                                      HWND overrideWindow, const RGNDATA* dirty) {
    auto original = g_present.load(std::memory_order_acquire);
    if (!original) return D3DERR_INVALIDCALL;

    const int64_t begin = QpcNow();
    const HRESULT hr = original(self, src, dst, overrideWindow, dirty);
    const int64_t end = QpcNow();

    // Device-lost/reset failures must not masquerade as frames delivered to WGC.
    if (SUCCEEDED(hr)) {
        const uint64_t id = g_presentId.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_writer) {
            ChaosVrPresentSampleV1 s{};
            s.qpcPresentBegin = begin;
            s.qpcPresentEnd = end;
            s.presentId = id;
            s.hresult = hr;
            g_writer->Publish(s);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetTransform(IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    auto original = g_setTransform.load(std::memory_order_acquire);
    if (!original) return D3DERR_INVALIDCALL;
    g_setTransformCalls.fetch_add(1, std::memory_order_relaxed);
    if (state == D3DTS_VIEW) {
        g_viewTransformCalls.fetch_add(1, std::memory_order_relaxed);
        if (matrix) {
            rendercam::Mat4 base{}; std::memcpy(base.m.data(),matrix,sizeof(float)*16);
            rendercam::Mat4 delta{}; bool enabled=false;
            {
                std::scoped_lock lock(g_viewOverrideMutex);
                delta=g_viewDelta; enabled=g_viewOverrideEnabled;
                const auto cameraWorld=rendercam::InverseRigidRow(base);
                g_lastViewEyeX=cameraWorld.m[12];
                g_lastViewEyeY=cameraWorld.m[13];
                g_lastViewEyeZ=cameraWorld.m[14];
                g_hasLastViewEye=true;
            }
            if (enabled) {
                g_viewOverrideCalls.fetch_add(1, std::memory_order_relaxed);
                const auto changed=rendercam::ApplyLocalHeadDelta(base,delta);
                D3DMATRIX out{}; std::memcpy(&out,changed.m.data(),sizeof(float)*16);
                return original(self,state,&out);
            }
        }
    } else if (state == D3DTS_PROJECTION) g_projectionTransformCalls.fetch_add(1, std::memory_order_relaxed);
    return original(self, state, matrix);
}

HRESULT STDMETHODCALLTYPE HookSetVertexShaderConstantF(IDirect3DDevice9* self, UINT startRegister,
                                                        const float* data, UINT vector4Count) {
    auto original = g_setVsConstF.load(std::memory_order_acquire);
    if (!original) return D3DERR_INVALIDCALL;
    g_vsConstFCalls.fetch_add(1, std::memory_order_relaxed);
    if (vector4Count == 3 || vector4Count == 4) {
        g_matrixVsConstFCalls.fetch_add(1, std::memory_order_relaxed);
        if (startRegister < g_matrixRegisterCalls.size())
            g_matrixRegisterCalls[startRegister].fetch_add(1, std::memory_order_relaxed);
        if (vector4Count == 4 && data) {
            const auto m = renderprobe::Load4x4(data);
            if (renderprobe::LooksLikePerspective(m)) {
                g_likelyVsPerspective.fetch_add(1, std::memory_order_relaxed);
                if (startRegister < g_perspectiveRegisterCalls.size()) g_perspectiveRegisterCalls[startRegister].fetch_add(1, std::memory_order_relaxed);
            }
            const bool rowView=renderprobe::LooksLikeRigidViewRow(m);
            const bool transposedView=renderprobe::LooksLikeRigidViewTransposed(m);
            if (rowView || transposedView) {
                g_likelyVsRigidView.fetch_add(1, std::memory_order_relaxed);
                if (startRegister < g_rigidViewRegisterCalls.size()) g_rigidViewRegisterCalls[startRegister].fetch_add(1, std::memory_order_relaxed);

                rendercam::Mat4 delta{}; bool enabled=false;
                {
                    std::scoped_lock lock(g_viewOverrideMutex);
                    delta=g_viewDelta; enabled=g_shaderViewOverrideEnabled;
                }
                const int target=g_shaderViewTargetRegister.load(std::memory_order_relaxed);
                if (enabled && target >= 0 && startRegister == static_cast<UINT>(target)) {
                    rendercam::Mat4 base{};
                    if (rowView) {
                        std::copy(m.v.begin(),m.v.end(),base.m.begin());
                    } else {
                        const auto t=renderprobe::Transpose(m);
                        std::copy(t.v.begin(),t.v.end(),base.m.begin());
                    }
                    const auto changed=rendercam::ApplyLocalHeadDelta(base,delta);
                    float out[16]{};
                    if (rowView) {
                        std::copy(changed.m.begin(),changed.m.end(),out);
                    } else {
                        renderprobe::Mat4 temp{}; std::copy(changed.m.begin(),changed.m.end(),temp.v.begin());
                        const auto t=renderprobe::Transpose(temp);
                        std::copy(t.v.begin(),t.v.end(),out);
                    }
                    g_shaderViewOverrideCalls.fetch_add(1,std::memory_order_relaxed);
                    return original(self,startRegister,out,vector4Count);
                }
            } else {
                const int target=g_shaderViewTargetRegister.load(std::memory_order_relaxed);
                if (g_shaderViewOverrideEnabled && target >= 0 && startRegister == static_cast<UINT>(target))
                    g_shaderViewRejectedNonRigid.fetch_add(1,std::memory_order_relaxed);
            }
        }
    }
    return original(self, startRegister, data, vector4Count);
}

bool HookDeviceRenderPath(IDirect3DDevice9* device) noexcept {
    if (!device) return false;
    auto*** object = reinterpret_cast<void***>(device);
    if (!object || !*object) return false;
    void** vtable = *object;
    bool any = false;

    // IDirect3DDevice9 vtable: SetTransform=44, SetVertexShaderConstantF=94.
    {
        void** slot = &vtable[44];
        auto current = reinterpret_cast<SetTransformFn>(*slot);
        SetOwner(&D3D9HookOwnership::setTransformModule,reinterpret_cast<void*>(current));
        if (current == &HookSetTransform) any = true;
        else {
            SetTransformFn expected = nullptr;
            if (g_setTransform.compare_exchange_strong(expected, current, std::memory_order_acq_rel) ||
                g_setTransform.load(std::memory_order_acquire) == current) {
                if (WritePointer(slot, reinterpret_cast<void*>(&HookSetTransform))) { g_setTransformVtableSlot = slot; any = true; }
            }
        }
    }
    {
        void** slot = &vtable[94];
        auto current = reinterpret_cast<SetVertexShaderConstantFFn>(*slot);
        SetOwner(&D3D9HookOwnership::vertexShaderConstantFModule,reinterpret_cast<void*>(current));
        if (current == &HookSetVertexShaderConstantF) any = true;
        else {
            SetVertexShaderConstantFFn expected = nullptr;
            if (g_setVsConstF.compare_exchange_strong(expected, current, std::memory_order_acq_rel) ||
                g_setVsConstF.load(std::memory_order_acquire) == current) {
                if (WritePointer(slot, reinterpret_cast<void*>(&HookSetVertexShaderConstantF))) { g_setVsConstFVtableSlot = slot; any = true; }
            }
        }
    }
    if (any) g_renderPathProbeInstalled.store(true, std::memory_order_release);
    return any;
}

bool HookDevicePresent(IDirect3DDevice9* device) noexcept {
    if (!device) return false;
    auto*** object = reinterpret_cast<void***>(device);
    if (!object || !*object) return false;
    void** vtable = *object;
    // IDirect3DDevice9: Reset=16, Present=17.
    void** slot = &vtable[17];
    auto current = reinterpret_cast<PresentFn>(*slot);
    SetOwner(&D3D9HookOwnership::presentModule,reinterpret_cast<void*>(current));
    if (current == &HookPresent) return true;

    PresentFn expected = nullptr;
    g_present.compare_exchange_strong(expected, current, std::memory_order_acq_rel);
    g_presentVtableSlot = slot;
    return WritePointer(slot, reinterpret_cast<void*>(&HookPresent));
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                           DWORD behavior, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** outDevice) {
    auto original = g_createDevice.load(std::memory_order_acquire);
    if (!original) return D3DERR_INVALIDCALL;
    const HRESULT hr = original(self, adapter, type, focus, behavior, pp, outDevice);
    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        IDirect3DDevice9Ex* ex{};
        if (SUCCEEDED((*outDevice)->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex))) && ex) {
            g_d3d9ExDevice.store(true, std::memory_order_release);
            ex->Release();
        }
        HookDevicePresent(*outDevice);
        HookDeviceRenderPath(*outDevice);
    }
    return hr;
}

bool HookCreateDeviceMethod(IDirect3D9* d3d) noexcept {
    if (!d3d) return false;
    auto*** object = reinterpret_cast<void***>(d3d);
    if (!object || !*object) return false;
    void** vtable = *object;
    // IDirect3D9::CreateDevice is vtable slot 16.
    void** slot = &vtable[16];
    auto current = reinterpret_cast<CreateDeviceFn>(*slot);
    SetOwner(&D3D9HookOwnership::createDeviceModule,reinterpret_cast<void*>(current));
    if (current == &HookCreateDevice) return true;

    CreateDeviceFn expected = nullptr;
    g_createDevice.compare_exchange_strong(expected, current, std::memory_order_acq_rel);
    g_createDeviceVtableSlot = slot;
    return WritePointer(slot, reinterpret_cast<void*>(&HookCreateDevice));
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion) {
    auto original = g_create9.load(std::memory_order_acquire);
    if (!original) return nullptr;
    IDirect3D9* d3d = original(sdkVersion);
    if (d3d) HookCreateDeviceMethod(d3d);
    return d3d;
}
}

bool D3D9PresentHook::Start(HMODULE gameModule, PresentTimingWriter* writer) noexcept {
    if (installed_) return true;
    g_writer = writer;

    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
    if (!d3d9) return false;
    void* resolved = reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!resolved) return false;

    void** slot = FindImportSlot(gameModule, "d3d9.dll", "Direct3DCreate9", resolved);
    if (!slot) return false;

    auto original = reinterpret_cast<Direct3DCreate9Fn>(*slot);
    if (!original || original == &HookDirect3DCreate9) return false;
    g_create9.store(original, std::memory_order_release);
    SetOwner(&D3D9HookOwnership::direct3DCreate9Module,reinterpret_cast<void*>(original));
    g_create9IatSlot = slot;
    if (!WritePointer(slot, reinterpret_cast<void*>(&HookDirect3DCreate9))) return false;

    installed_ = true;
    return true;
}

void D3D9PresentHook::Stop() noexcept {
    // Restore only slots that still point at us. Never overwrite a later hook.
    if (g_setVsConstFVtableSlot && *g_setVsConstFVtableSlot == reinterpret_cast<void*>(&HookSetVertexShaderConstantF))
        WritePointer(g_setVsConstFVtableSlot, reinterpret_cast<void*>(g_setVsConstF.load()));
    if (g_setTransformVtableSlot && *g_setTransformVtableSlot == reinterpret_cast<void*>(&HookSetTransform))
        WritePointer(g_setTransformVtableSlot, reinterpret_cast<void*>(g_setTransform.load()));
    if (g_presentVtableSlot && *g_presentVtableSlot == reinterpret_cast<void*>(&HookPresent))
        WritePointer(g_presentVtableSlot, reinterpret_cast<void*>(g_present.load()));
    if (g_createDeviceVtableSlot && *g_createDeviceVtableSlot == reinterpret_cast<void*>(&HookCreateDevice))
        WritePointer(g_createDeviceVtableSlot, reinterpret_cast<void*>(g_createDevice.load()));
    if (g_create9IatSlot && *g_create9IatSlot == reinterpret_cast<void*>(&HookDirect3DCreate9))
        WritePointer(g_create9IatSlot, reinterpret_cast<void*>(g_create9.load()));
    g_writer = nullptr;
    g_presentVtableSlot = nullptr;
    g_setTransformVtableSlot = nullptr;
    g_setVsConstFVtableSlot = nullptr;
    g_createDeviceVtableSlot = nullptr;
    g_create9IatSlot = nullptr;
    g_renderPathProbeInstalled.store(false, std::memory_order_release);
    installed_ = false;
}

uint64_t D3D9PresentHook::SuccessfulPresents() const noexcept {
    return g_presentId.load(std::memory_order_relaxed);
}

bool D3D9PresentHook::RenderPathProbeInstalled() const noexcept {
    return g_renderPathProbeInstalled.load(std::memory_order_acquire);
}

D3D9HookOwnership D3D9PresentHook::Ownership() const {
    std::scoped_lock lock(g_ownerMutex);
    return g_ownership;
}

void D3D9PresentHook::SetFixedFunctionViewOverride(const rendercam::Mat4& localHeadDelta, bool enabled) noexcept {
    std::scoped_lock lock(g_viewOverrideMutex);
    g_viewDelta=localHeadDelta; g_viewOverrideEnabled=enabled;
}

void D3D9PresentHook::SetShaderViewOverride(const rendercam::Mat4& localHeadDelta, bool enabled, int targetRegister) noexcept {
    std::scoped_lock lock(g_viewOverrideMutex);
    g_viewDelta=localHeadDelta; g_shaderViewOverrideEnabled=enabled;
    g_shaderViewTargetRegister.store(enabled?targetRegister:-1,std::memory_order_release);
}

D3D9RenderPathStats D3D9PresentHook::RenderPathStats() const noexcept {
    D3D9RenderPathStats s{};
    s.successfulPresents = g_presentId.load(std::memory_order_relaxed);
    s.setTransformCalls = g_setTransformCalls.load(std::memory_order_relaxed);
    s.viewTransformCalls = g_viewTransformCalls.load(std::memory_order_relaxed);
    s.projectionTransformCalls = g_projectionTransformCalls.load(std::memory_order_relaxed);
    s.fixedFunctionViewOverrideCalls = g_viewOverrideCalls.load(std::memory_order_relaxed);
    s.shaderViewOverrideCalls = g_shaderViewOverrideCalls.load(std::memory_order_relaxed);
    s.shaderViewRejectedNonRigid = g_shaderViewRejectedNonRigid.load(std::memory_order_relaxed);
    s.shaderViewTargetRegister = g_shaderViewTargetRegister.load(std::memory_order_relaxed);
    s.d3d9ExDevice = g_d3d9ExDevice.load(std::memory_order_acquire);
    {
        std::scoped_lock lock(g_viewOverrideMutex);
        s.hasLastViewEye=g_hasLastViewEye;
        s.lastViewEyeX=g_lastViewEyeX; s.lastViewEyeY=g_lastViewEyeY; s.lastViewEyeZ=g_lastViewEyeZ;
    }
    s.vertexShaderConstantFCalls = g_vsConstFCalls.load(std::memory_order_relaxed);
    s.matrixSizedVsConstantCalls = g_matrixVsConstFCalls.load(std::memory_order_relaxed);
    s.likelyVsPerspectiveMatrices = g_likelyVsPerspective.load(std::memory_order_relaxed);
    s.likelyVsRigidViewMatrices = g_likelyVsRigidView.load(std::memory_order_relaxed);
    std::array<uint64_t,256> counts{},projCounts{},viewCounts{};
    for (size_t i=0;i<counts.size();++i) {
        counts[i]=g_matrixRegisterCalls[i].load(std::memory_order_relaxed);
        projCounts[i]=g_perspectiveRegisterCalls[i].load(std::memory_order_relaxed);
        viewCounts[i]=g_rigidViewRegisterCalls[i].load(std::memory_order_relaxed);
    }
    const auto top=renderprobe::TopRegisters(counts);
    const auto ptop=renderprobe::TopRegisters(projCounts);
    const auto vtop=renderprobe::TopRegisters(viewCounts);
    for (size_t i=0;i<top.size();++i) {
        s.topMatrixRegisters[i]={top[i].startRegister,top[i].calls};
        s.topPerspectiveRegisters[i]={ptop[i].startRegister,ptop[i].calls};
        s.topRigidViewRegisters[i]={vtop[i].startRegister,vtop[i].calls};
    }
    return s;
}

} // namespace chaosvr::bridge32