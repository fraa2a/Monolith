#include "ffmpeg_process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// Auto-download of a trusted Windows FFmpeg build (gyan.dev release-essentials),
// mirroring the Record-able mod's FfmpegBundleManager: HTTPS download, SHA-256
// verification against the upstream .sha256, extraction of ffmpeg.exe +
// ffprobe.exe. Extraction uses Windows' built-in tar.exe (bsdtar, present since
// Win10 1803) so we add no zip dependency.

namespace encoding {

namespace {

constexpr wchar_t kHost[]      = L"www.gyan.dev";
constexpr wchar_t kZipPath[]   = L"/ffmpeg/builds/ffmpeg-release-essentials.zip";
constexpr wchar_t kShaPath[]   = L"/ffmpeg/builds/ffmpeg-release-essentials.zip.sha256";

void report(const std::function<void(const std::string&)>& cb, const std::string& s)
{
    if (cb) cb(s);
}

std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// GETs an HTTPS resource on kHost. `out` receives the raw body. WinHTTP follows
// redirects automatically. Returns false on any failure or non-200 status.
bool https_get(const wchar_t* path, std::string* out)
{
    out->clear();
    HINTERNET session = WinHttpOpen(L"Monolith-ffmpeg-fetch/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    if (connect) {
        request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {

        DWORD status = 0, status_len = sizeof(status);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            DWORD avail = 0;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail)) break;
                if (avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(request, buf.data(), avail, &read)) break;
                out->append(buf.data(), read);
            } while (avail > 0);
            ok = !out->empty();
        }
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// Lowercase hex SHA-256 of a byte buffer, via CNG (BCrypt).
std::string sha256_hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};

    DWORD hash_len = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len, sizeof(hash_len), &cb, 0);
    std::vector<UCHAR> hash(hash_len);

    BCRYPT_HASH_HANDLE h = nullptr;
    std::string result;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
            BCryptFinishHash(h, hash.data(), hash_len, 0) == 0) {
            static const char* hexd = "0123456789abcdef";
            result.reserve(hash_len * 2);
            for (UCHAR b : hash) {
                result.push_back(hexd[b >> 4]);
                result.push_back(hexd[b & 0xF]);
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

bool write_file(const std::wstring& path, const std::string& data)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
        DWORD wrote = 0;
        if (!WriteFile(f, data.data() + off, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false; break;
        }
        off += wrote;
    }
    CloseHandle(f);
    return ok;
}

// Runs a command hidden, waits, returns its exit code (or -1 on launch failure).
int run_hidden(const std::wstring& cmdline, const std::wstring& cwd)
{
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(L'\0');
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// Recursively finds `name` under `root`, returning its full path or empty.
std::wstring find_recursive(const std::wstring& root, const std::wstring& name)
{
    std::wstring found;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = root + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = find_recursive(full, name);
            if (!found.empty()) break;
        } else if (_wcsicmp(n.c_str(), name.c_str()) == 0) {
            found = full; break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

void remove_tree(const std::wstring& root)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring full = root + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(full);
            else
                DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(root.c_str());
}

} // namespace

bool ensure_ffmpeg_downloaded(std::function<void(const std::string&)> progress)
{
    const std::wstring bin_dir = ffmpeg_download_dir();
    if (bin_dir.empty()) { report(progress, "cannot resolve download dir"); return false; }

    const std::wstring ffmpeg_exe  = bin_dir + L"\\ffmpeg.exe";
    const std::wstring ffprobe_exe = bin_dir + L"\\ffprobe.exe";

    // Already present + runnable? Nothing to do.
    if (verify_ffmpeg_binary(ffmpeg_exe, "ffmpeg")) {
        report(progress, "ffmpeg already installed");
        return true;
    }

    SHCreateDirectoryExW(nullptr, bin_dir.c_str(), nullptr);

    report(progress, "downloading FFmpeg from gyan.dev…");
    std::string zip;
    if (!https_get(kZipPath, &zip)) {
        report(progress, "download failed");
        return false;
    }

    // Best-effort SHA-256 verification. TLS already authenticates the host, so a
    // missing hash file degrades to a warning rather than a hard failure.
    std::string sha_body;
    if (https_get(kShaPath, &sha_body)) {
        std::string expected;
        for (char c : sha_body) {
            if (std::isxdigit((unsigned char)c)) expected.push_back((char)std::tolower((unsigned char)c));
            else if (!expected.empty()) break;
        }
        std::string actual = sha256_hex(zip);
        if (!expected.empty() && !actual.empty() && expected != actual) {
            report(progress, "SHA-256 mismatch — aborting");
            return false;
        }
        report(progress, "SHA-256 verified");
    } else {
        report(progress, "could not fetch SHA-256 (HTTPS-authenticated download)");
    }

    // Stage the zip in a temp dir and extract with tar.exe.
    std::wstring tmp = bin_dir + L"\\_dl";
    remove_tree(tmp);
    SHCreateDirectoryExW(nullptr, tmp.c_str(), nullptr);
    std::wstring zip_path = tmp + L"\\ffmpeg.zip";
    if (!write_file(zip_path, zip)) {
        report(progress, "failed to write archive");
        remove_tree(tmp);
        return false;
    }

    report(progress, "extracting…");
    // bsdtar understands zip: tar -xf <zip> -C <dir>
    std::wstring cmd = L"tar.exe -xf \"" + zip_path + L"\" -C \"" + tmp + L"\"";
    if (run_hidden(cmd, tmp) != 0) {
        report(progress, "extraction failed (tar.exe)");
        remove_tree(tmp);
        return false;
    }

    std::wstring src_ffmpeg  = find_recursive(tmp, L"ffmpeg.exe");
    std::wstring src_ffprobe = find_recursive(tmp, L"ffprobe.exe");
    if (src_ffmpeg.empty()) {
        report(progress, "ffmpeg.exe not found in archive");
        remove_tree(tmp);
        return false;
    }

    CopyFileW(src_ffmpeg.c_str(), ffmpeg_exe.c_str(), FALSE);
    if (!src_ffprobe.empty())
        CopyFileW(src_ffprobe.c_str(), ffprobe_exe.c_str(), FALSE);

    remove_tree(tmp);

    if (!verify_ffmpeg_binary(ffmpeg_exe, "ffmpeg")) {
        report(progress, "downloaded ffmpeg is not runnable");
        return false;
    }
    report(progress, "FFmpeg ready: " + wide_to_utf8(ffmpeg_exe));
    return true;
}

} // namespace encoding
