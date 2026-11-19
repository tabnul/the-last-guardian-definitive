// RenderDoc frame captures for GPU work inspection (R5). Off unless TLG_RENDERDOC=1.
//   TLG_RENDERDOC=1          load renderdoc.dll (Linux: lib/librenderdoc.so) from TLG_RENDERDOC_DIR (the portable RenderDoc folder) before the
//                            Vulkan instance is made, and enable its layer for this process only (VK_ADD_LAYER_PATH + VK_INSTANCE_LAYERS;
//                            nothing is registered). Linux: the folder's etc/vulkan/implicit_layer.d/renderdoc_capture.json must have a
//                            library_path that works from there (the tarball's "/io/dist/lib/..." has to be changed to "../../../lib/librenderdoc.so").
//   TLG_RENDERDOC_AT=N       capture the frame after present N (several: N,M,...)
//   TLG_RENDERDOC_TAG=name   capture files user/log/rdc/<name>_frameX.rdc (default "tlg")
// Debug server: /renderdoc (state), /renderdoc?capture=1 (capture the next frame).

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "native/renderdoc_app.h"
#include "native/native_renderdoc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace VideoCore {
namespace {
RENDERDOC_API_1_6_0* rdoc{};
std::vector<u64> capture_at;
std::atomic<u64> triggered{0};
std::string out_template;
} // namespace

void LoadRenderDoc() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const char* on = std::getenv("TLG_RENDERDOC");
    if (on == nullptr || on[0] != '1') {
        return;
    }
    pRENDERDOC_GetAPI get_api{};
#ifdef _WIN32
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (mod == nullptr) {
        const char* dir = std::getenv("TLG_RENDERDOC_DIR");
        if (dir == nullptr || dir[0] == 0) {
            LOG_ERROR(Render, "TLG_RENDERDOC=1 but TLG_RENDERDOC_DIR is not set");
            return;
        }
        const std::string d{dir};
        SetEnvironmentVariableA("VK_ADD_LAYER_PATH", d.c_str());
        SetEnvironmentVariableA("VK_INSTANCE_LAYERS", "VK_LAYER_RENDERDOC_Capture");
        mod = LoadLibraryA((d + "\\renderdoc.dll").c_str());
        if (mod == nullptr) {
            LOG_ERROR(Render, "RenderDoc: cannot load {}\\renderdoc.dll", d);
            return;
        }
    }
    get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
#else
    void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (mod == nullptr) {
        const char* dir = std::getenv("TLG_RENDERDOC_DIR");
        if (dir == nullptr || dir[0] == 0) {
            LOG_ERROR(Render, "TLG_RENDERDOC=1 but TLG_RENDERDOC_DIR is not set");
            return;
        }
        const std::string d{dir};
        // The layer manifest is searched in the same folder tree; the layer is switched on for this process only.
        setenv("VK_ADD_LAYER_PATH", (d + "/etc/vulkan/implicit_layer.d").c_str(), 1);
        setenv("VK_INSTANCE_LAYERS", "VK_LAYER_RENDERDOC_Capture", 1);
        mod = dlopen((d + "/lib/librenderdoc.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (mod == nullptr) {
            LOG_ERROR(Render, "RenderDoc: cannot load {}/lib/librenderdoc.so: {}", d, dlerror());
            return;
        }
    }
    get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
#endif
    if (get_api == nullptr || get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&rdoc)) != 1) {
        rdoc = nullptr;
        LOG_ERROR(Render, "RenderDoc: no API 1.6.0");
        return;
    }
    rdoc->SetCaptureKeys(nullptr, 0);
    rdoc->UnloadCrashHandler();
    const char* tag = std::getenv("TLG_RENDERDOC_TAG");
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "rdc";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    out_template = (dir / (tag != nullptr && tag[0] != 0 ? tag : "tlg")).string();
    rdoc->SetCaptureFilePathTemplate(out_template.c_str());
    if (const char* at = std::getenv("TLG_RENDERDOC_AT"); at != nullptr) {
        for (const char* p = at; *p != 0;) {
            char* end = nullptr;
            const u64 v = std::strtoull(p, &end, 10);
            if (end == p) {
                break;
            }
            capture_at.push_back(v);
            p = *end == ',' ? end + 1 : end;
        }
    }
    int major = 0, minor = 0, patch = 0;
    rdoc->GetAPIVersion(&major, &minor, &patch);
    LOG_WARNING(Render, "RenderDoc {}.{}.{} loaded, captures to {}_frameX.rdc, {} planned", major, minor, patch, out_template, capture_at.size());
}

void NoteRenderDocPresent(u64 index) {
    if (rdoc == nullptr) {
        return;
    }
    for (const u64 at : capture_at) {
        if (at == index) {
            rdoc->TriggerCapture();
            triggered.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool RenderDocCaptureNext() {
    if (rdoc == nullptr) {
        return false;
    }
    rdoc->TriggerCapture();
    triggered.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string RenderDocState() {
    if (rdoc == nullptr) {
        return "{\"loaded\":false}";
    }
    std::string s = "{\"loaded\":true,\"triggered\":" + std::to_string(triggered.load()) + ",\"captures\":" + std::to_string(rdoc->GetNumCaptures()) + ",\"files\":[";
    for (u32 i = 0; i < rdoc->GetNumCaptures(); ++i) {
        u32 len = 0;
        u64 ts = 0;
        rdoc->GetCapture(i, nullptr, &len, &ts);
        std::string path(len, '\0');
        rdoc->GetCapture(i, path.data(), &len, &ts);
        while (!path.empty() && path.back() == '\0') {
            path.pop_back();
        }
        for (char& c : path) {
            if (c == '\\') {
                c = '/';
            }
        }
        s += (i ? ",\"" : "\"") + path + "\"";
    }
    return s + "]}";
}

} // namespace VideoCore
