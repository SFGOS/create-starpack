#include "create-starpack.hpp"

#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <string>
#include <filesystem>
#include <magic.h>
#include <vector>
#include <cctype>
#include <utility>
#include <string.h>
#include <yaml-cpp/yaml.h>
#include <unordered_set>
#include <unordered_map>
#include <git2.h>
#include <chrono>
#include <unistd.h>

/**
 * @brief Trims leading and trailing whitespace from the given string.
 *
 * Whitespace includes spaces, tabs, newlines, and carriage returns.
 * If the string is all whitespace, returns an empty string.
 *
 * @param s The input string to trim.
 * @return A new string without leading/trailing whitespace.
 */
static inline std::string trim(const std::string &s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    if (start == s.size()) {
        return "";
    }
    size_t end = s.size() - 1;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end]))) {
        --end;
    }
    return s.substr(start, end - start + 1);
}

/**
 * @brief Finds all substrings enclosed in double quotes (") in the given input
 *        and returns them as a vector.
 *
 * Example: input = R"( "foo" "bar" )" -> ["foo", "bar"]
 *
 * @param input The string to search for quoted segments.
 * @return A list of extracted quoted strings (without quotes).
 */
static std::vector<std::string> extract_quoted_strings(const std::string &input)
{
    std::vector<std::string> result;
    std::regex re("\"([^\"]*)\"");
    auto words_begin = std::sregex_iterator(input.begin(), input.end(), re);
    auto words_end   = std::sregex_iterator();

    for (auto it = words_begin; it != words_end; ++it) {
        // Capture group 1 is the text between the quotes.
        result.push_back((*it)[1].str());
    }
    return result;
}

namespace Starpack {
namespace CreateStarpack {

/**
 * @namespace fs
 * Alias for the <filesystem> namespace to keep code concise.
 */
namespace fs = std::filesystem;

/**
 * @brief Global flag that determines whether or not to strip binaries in postProcessFiles().
 */
bool noStripping = false;

/**
 * @brief Global flag controlling whether to run commands under fakeroot.
 *        Defaults to true if geteuid() != 0. Overridable via --no-fakeroot.
 */
bool useFakeroot = (geteuid() != 0);

//------------------------------------------------------------------------------
// parse_starbuild
//------------------------------------------------------------------------------
// Reads a "STARBUILD" file line by line. Extracts various package metadata, script
// function bodies (prepare, compile, verify, assemble), and arrays like dependencies.
//
// If you have subpackage "dependencies_foo", it stores them in subpackageDependencies["foo"].
//
// This function also populates a list of symlink pairs if lines are encountered with
// "symlink: \"link:target\"" syntax.
bool parse_starbuild(
    const std::string &filepath,
    std::vector<std::string> &package_names,
    std::vector<std::string> &package_descriptions,
    std::unordered_map<std::string, std::vector<std::string>> &subpackageDependencies,
    std::string &package_version,
    std::string &description,
    std::vector<std::string> &dependencies,
    std::vector<std::string> &build_dependencies,
    std::vector<std::string> &sources,
    std::string &prepare_function,
    std::string &compile_function,
    std::string &verify_function,
    std::string &generic_assemble_function,
    std::unordered_map<std::string, std::string> &assemble_functions,
    std::vector<std::pair<std::string, std::string>> &symlinkPairs)
{
    std::ifstream file(filepath);
    if (!file) {
        log_error("Error opening STARBUILD file: " + filepath);
        return false;
    }

    // Regex patterns for line-based matches
    std::regex re_package_name_array("package_name\\s*=\\s*\\((.*)\\)");
    std::regex re_package_name_single("package_name\\s*=\\s*\"(.*)\"");
    std::regex re_package_version("package_version\\s*=\\s*\"(.*)\"");
    std::regex re_description("description\\s*=\\s*\"(.*)\"");
    std::regex re_dependencies("^dependencies\\s*=\\s*\\((.*)\\)");
    std::regex re_build_dependencies("^build_dependencies\\s*=\\s*\\((.*)\\)");

    bool in_prepare         = false;
    bool in_compile         = false;
    bool in_verify          = false;
    bool in_generic_assemble = false;
    bool in_specific_assemble = false;

    std::string current_assemble_key; // e.g. "pkg", from lines like "assemble_pkg() {"

    // store function bodies line by line in these streams
    std::ostringstream prepare_stream;
    std::ostringstream compile_stream;
    std::ostringstream verify_stream;
    std::ostringstream generic_assemble_stream;
    std::ostringstream specific_assemble_stream;

    std::string line;
    std::smatch match;

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        // Skip empty lines or commented lines (#)
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        // 1) package_name = ( "pkg1" "pkg2" ) or package_name = "pkg"
        if (std::regex_match(trimmed, match, re_package_name_array)) {
            auto names = extract_quoted_strings(match[1].str());
            package_names.insert(package_names.end(), names.begin(), names.end());
            continue;
        }
        if (std::regex_match(trimmed, match, re_package_name_single)) {
            package_names.push_back(match[1].str());
            continue;
        }

        // 2) package_descriptions = ( "desc1" "desc2" )
        if (trimmed.find("package_descriptions") == 0 && trimmed.find("(") != std::string::npos) {
            size_t startPos = trimmed.find("(");
            if (startPos != std::string::npos) {
                std::string arr = trimmed.substr(startPos + 1);
                // Possibly multiline, read until closing ")"
                while (arr.find(")") == std::string::npos) {
                    if (!std::getline(file, line)) break;
                    arr += " " + trim(line);
                }
                size_t endPos = arr.find(")");
                if (endPos != std::string::npos) {
                    arr = arr.substr(0, endPos);
                }
                auto descs = extract_quoted_strings(arr);
                package_descriptions.insert(
                    package_descriptions.end(), descs.begin(), descs.end()
                );
            }
            continue;
        }

        // 3) subpackage-specific dependencies: "dependencies_<pkg> = ( "dep1" "dep2" )"
        if (trimmed.rfind("dependencies_", 0) == 0) {
            // e.g. "dependencies_util-linux"
            size_t eqPos = trimmed.find("=");
            if (eqPos != std::string::npos) {
                std::string leftSide = trim(trimmed.substr(0, eqPos));
                // remove "dependencies_"
                std::string subpkgName = leftSide.substr(std::string("dependencies_").size());

                size_t startPos = trimmed.find("(");
                if (startPos != std::string::npos) {
                    std::string arr = trimmed.substr(startPos + 1);
                    while (arr.find(")") == std::string::npos) {
                        if (!std::getline(file, line)) break;
                        arr += " " + trim(line);
                    }
                    size_t endPos = arr.find(")");
                    if (endPos != std::string::npos) {
                        arr = arr.substr(0, endPos);
                    }
                    auto subpkgDeps = extract_quoted_strings(arr);
                    subpackageDependencies[subpkgName].insert(
                        subpackageDependencies[subpkgName].end(),
                        subpkgDeps.begin(),
                        subpkgDeps.end()
                    );
                }
            }
            continue;
        }

        // 4) package_version, single line
        if (std::regex_match(trimmed, match, re_package_version)) {
            package_version = match[1].str();
            continue;
        }

        // description="..."
        if (std::regex_match(trimmed, match, re_description)) {
            description = match[1].str();
            continue;
        }

        // 5) global dependencies or build_dependencies
        if (std::regex_search(trimmed, match, re_dependencies)) {
            auto words = extract_quoted_strings(match[1].str());
            dependencies.insert(dependencies.end(), words.begin(), words.end());
            continue;
        }
        if (std::regex_search(trimmed, match, re_build_dependencies)) {
            auto words = extract_quoted_strings(match[1].str());
            build_dependencies.insert(build_dependencies.end(), words.begin(), words.end());
            continue;
        }

        // 6) parse sources = ( "url" "foo.zip" ), possibly multiline
        if (trimmed.rfind("sources=", 0) == 0) {
            size_t startPos = trimmed.find("(");
            if (startPos != std::string::npos) {
                std::string arr = trimmed.substr(startPos + 1);
                while (arr.find(")") == std::string::npos) {
                    if (!std::getline(file, line)) break;
                    arr += " " + trim(line);
                }
                size_t endPos = arr.find(")");
                if (endPos != std::string::npos) {
                    arr = arr.substr(0, endPos);
                }
                auto words = extract_quoted_strings(arr);
                sources.insert(sources.end(), words.begin(), words.end());
            }
            continue;
        }

        // 7) parse symlink: lines with "symlink: "link:target""
        if (trimmed.rfind("symlink:", 0) == 0) {
            std::string pairStr = trim(trimmed.substr(8));
            if (!pairStr.empty() && pairStr.front() == '\"' && pairStr.back() == '\"') {
                pairStr = pairStr.substr(1, pairStr.size() - 2);
            }
            size_t colonPos = pairStr.find(':');
            if (colonPos != std::string::npos) {
                std::string link = trim(pairStr.substr(0, colonPos));
                std::string target = trim(pairStr.substr(colonPos + 1));
                if (!link.empty() && !target.empty()) {
                    symlinkPairs.push_back({link, target});
                }
            }
            continue;
        }

        // 8) parse function blocks
        //    prepare() { ... }, compile() { ... }, verify() { ... }, assemble() { ... }, assemble_<pkg>() { ... }
        if (trimmed.find("prepare()") == 0 && trimmed.find("{") != std::string::npos) {
            in_prepare = true;
            continue;
        }
        if (trimmed.find("compile()") == 0 && trimmed.find("{") != std::string::npos) {
            in_compile = true;
            continue;
        }
        if (trimmed.find("verify()") == 0 && trimmed.find("{") != std::string::npos) {
            in_verify = true;
            continue;
        }
        if (trimmed.find("assemble()") == 0 && trimmed.find("{") != std::string::npos) {
            in_generic_assemble = true;
            continue;
        }
        if (trimmed.find("assemble_") == 0 &&
            trimmed.find("()") != std::string::npos &&
            trimmed.find("{") != std::string::npos)
        {
            size_t start = std::string("assemble_").size();
            size_t end = trimmed.find("()", start);
            if (end != std::string::npos) {
                current_assemble_key = trimmed.substr(start, end - start);
                in_specific_assemble = true;
                specific_assemble_stream.str("");  // Reset buffer
                continue;
            }
        }

        // 9) detect end of function block
        if (trimmed == "}") {
            if (in_prepare) {
                in_prepare = false;
                continue;
            }
            if (in_compile) {
                in_compile = false;
                continue;
            }
            if (in_verify) {
                in_verify = false;
                continue;
            }
            if (in_generic_assemble) {
                in_generic_assemble = false;
                generic_assemble_function = generic_assemble_stream.str();
                continue;
            }
            if (in_specific_assemble) {
                in_specific_assemble = false;
                assemble_functions[current_assemble_key] = specific_assemble_stream.str();
                continue;
            }
        }

        // 10) accumulate lines inside the currently open function's stream
        if (in_prepare) {
            prepare_stream << line << "\n";
            continue;
        }
        if (in_compile) {
            compile_stream << line << "\n";
            continue;
        }
        if (in_verify) {
            verify_stream << line << "\n";
            continue;
        }
        if (in_generic_assemble) {
            generic_assemble_stream << line << "\n";
            continue;
        }
        if (in_specific_assemble) {
            specific_assemble_stream << line << "\n";
            continue;
        }
    }

    // Store final function bodies
    prepare_function  = prepare_stream.str();
    compile_function  = compile_stream.str();
    verify_function   = verify_stream.str();

    return true;
}

/**
 * @brief libcurl write callback that writes received data to a file stream.
 *
 * If the write fails (e.g., out of disk space), returns 0 to signal an error.
 *
 * @param ptr Pointer to the data buffer from libcurl.
 * @param size Size of each data element (bytes).
 * @param nmemb Number of data elements.
 * @param stream The destination file stream pointer.
 * @return The number of bytes actually written, or 0 on error (abort).
 */
static size_t writeToFile(void* ptr, size_t size, size_t nmemb, void* stream)
{
    std::ofstream* outFile = static_cast<std::ofstream*>(stream);
    const size_t expectedBytes = size * nmemb;

    outFile->write(static_cast<const char*>(ptr), expectedBytes);

    // If the stream isn't good, signal error to libcurl by returning 0
    if (!outFile->good()) {
        log_error("Stream error while writing downloaded data to file.");
        return 0; // indicates failure
    }
    return expectedBytes;
}

/**
 * @struct DownloadProgress
 * Used by libcurl progress callback to store partial download info (filename, etc.)
 */
struct DownloadProgress {
    curl_off_t lastTotalDownloaded = 0;
    double lastPercent = 0.0;
    std::string destFile;
};

/**
 * @brief libcurl progress callback. Displays a progress bar for the download.
 */
static int progressCallback(
    void* clientp,
    curl_off_t dltotal, curl_off_t dlnow,
    curl_off_t ultotal, curl_off_t ulnow)
{
    DownloadProgress* prog = static_cast<DownloadProgress*>(clientp);

    if (dltotal <= 0) {
        // Unknown total size, can't do a standard percentage-based bar
        return 0; // no error
    }

    double percent = (100.0 * dlnow) / dltotal;
    if (percent - prog->lastPercent >= 1.0 || dlnow == dltotal) {
        prog->lastPercent = percent;
        int barWidth = 50;
        int pos = static_cast<int>((percent / 100.0) * barWidth);

        std::string bar;
        bar.reserve(barWidth);
        for (int i = 0; i < barWidth; ++i) {
            bar.push_back(i < pos ? '#' : ' ');
        }

        fprintf(stderr, "\r[%s] %3.0f%%  File: %s", bar.c_str(), percent, prog->destFile.c_str());
        fflush(stderr);
    }
    return 0;
}

/**
 * @brief Downloads a URL to the local file specified by destPath using libcurl.
 *
 * - Skips download if the file already exists.
 * - Provides a progress bar.
 * - Verifies final size if server provided Content-Length.
 * - On error, removes partial file and logs accordingly.
 *
 * @param url The remote URL to download.
 * @param destPath The local path where the file will be saved.
 * @return True on successful download, false otherwise.
 */
bool downloadFile(const std::string& url, const std::string& destPath)
{
    // If the file already exists, skip the download:
    if (std::filesystem::exists(destPath)) {
        log_message("File already exists, skipping download: " + destPath);
        return true;
    }

    // Attempt to open the destination file
    std::ofstream outFile(destPath, std::ios::binary);
    if (!outFile.is_open()) {
        log_error("Could not open file for writing: " + destPath);
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        log_error("Failed to initialize libcurl.");
        outFile.close();
        std::filesystem::remove(destPath);
        return false;
    }

    // Configure libcurl
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/8.12.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outFile);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Enable progress callback
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    DownloadProgress prog;
    prog.destFile = destPath;
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);

    // Hard fail on HTTP >= 400
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    // Low speed detection
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    // Perform the download
    CURLcode res = curl_easy_perform(curl);
    fprintf(stderr, "\n"); // newline after progress bar

    bool stream_error_after_loop = !outFile.good();
    curl_off_t expected_size = -1; // from server
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &expected_size);

    // Must close the file before checking file_size
    outFile.close();

    if (res != CURLE_OK) {
        log_error("curl_easy_perform() failed: (" + std::to_string(res) + ") "
                  + std::string(curl_easy_strerror(res)) + " for URL: " + url);
        std::filesystem::remove(destPath);
        curl_easy_cleanup(curl);
        return false;
    }

    if (stream_error_after_loop) {
        log_error("Output file stream error occurred after download for: " + destPath);
        std::filesystem::remove(destPath);
        curl_easy_cleanup(curl);
        return false;
    }

    // Compare file size if it have an expected content length
    std::error_code ec;
    uintmax_t actual_size = std::filesystem::file_size(destPath, ec);
    if (ec) {
        log_error("Could not get file size after download for: " + destPath + ". Error: " + ec.message());
        std::filesystem::remove(destPath);
        curl_easy_cleanup(curl);
        return false;
    }

    if (expected_size != -1 && static_cast<curl_off_t>(actual_size) != expected_size) {
        log_error("Download incomplete or corrupt: expected " + std::to_string(expected_size)
                  + " bytes but got " + std::to_string(actual_size) + " bytes for " + destPath);
        std::filesystem::remove(destPath);
        curl_easy_cleanup(curl);
        return false;
    }

    if (expected_size != -1) {
        log_message("Download size verified for " + destPath + " (" + std::to_string(actual_size) + " bytes).");
    } else {
        log_message("Download completed for " + destPath + " (" + std::to_string(actual_size)
                    + " bytes). Server did not provide expected size.");
    }

    curl_easy_cleanup(curl);
    return true;
}

/**
 * @brief Determines if a file is recognized as an archive by libmagic.
 *
 * Checks for tar, gzip, bzip2, xz, lzip, or zip MIME types.
 *
 * @param filePath The path to the file to examine.
 * @return True if recognized as an archive format, false otherwise.
 */
bool isArchiveFile(const std::string &filePath)
{
    magic_t magic = magic_open(MAGIC_MIME_TYPE);
    if (!magic) {
        log_error("Could not initialize libmagic");
        return false;
    }
    magic_load(magic, nullptr);

    const char *fileType = magic_file(magic, filePath.c_str());
    bool isArchive = fileType && (strstr(fileType, "x-tar") ||
                                  strstr(fileType, "gzip")  ||
                                  strstr(fileType, "bzip2") ||
                                  strstr(fileType, "xz")    ||
                                  strstr(fileType, "lzip")  ||
                                  strstr(fileType, "zip"));

    magic_close(magic);
    return isArchive;
}

/**
 * @brief Extracts a recognized archive into the current directory using libarchive.
 *        Skips extraction if "NOEXTRACT" is in the file name.
 *
 * @param archivePath The local path to the archive file.
 * @return True on successful extraction, false otherwise.
 */
bool extractArchive(const std::string &archivePath)
{
    // Check if the file is an archive by MIME type
    if (!isArchiveFile(archivePath)) {
        log_message("Not an archive, skipping extraction: " + archivePath);
        return true;
    }

    // If user appended "NOEXTRACT" to the source, skip actual extraction
    if (archivePath.find("NOEXTRACT") != std::string::npos) {
        log_message("NOEXTRACT flag found; copying archive without extraction: " + archivePath);
        return true;
    }

    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_lzip(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 10240) != ARCHIVE_OK) {
        log_error("Failed to open archive: " + archivePath + " => " + archive_error_string(a));
        archive_read_free(a);
        return false;
    }

    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *currentFile = archive_entry_pathname(entry);
        std::string fullOutputPath = std::string("./") + currentFile;

        std::filesystem::create_directories(std::filesystem::path(fullOutputPath).parent_path());

        // Switch based on file type
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            std::filesystem::create_directories(fullOutputPath);
            auto mode = archive_entry_perm(entry);
            std::filesystem::permissions(fullOutputPath,
                                         static_cast<std::filesystem::perms>(mode),
                                         std::filesystem::perm_options::replace);
        }
        else if (archive_entry_filetype(entry) == AE_IFLNK) {
            const char *linkTarget = archive_entry_symlink(entry);
            if (!linkTarget) {
                log_error("Symlink entry has no target: " + std::string(currentFile));
                archive_read_free(a);
                return false;
            }
            std::error_code ec;
            std::filesystem::create_symlink(linkTarget, fullOutputPath, ec);
            if (ec) {
                log_error("Failed to create symlink " + fullOutputPath + " -> " + linkTarget
                          + ": " + ec.message());
                archive_read_free(a);
                return false;
            }
        }
        else if (archive_entry_size(entry) > 0) {
            std::ofstream outFile(fullOutputPath, std::ios::binary);
            if (!outFile.is_open()) {
                log_error("Could not open for writing: " + fullOutputPath);
                archive_read_free(a);
                return false;
            }

            const void *buff;
            size_t size;
            la_int64_t offset;
            while (true) {
                int r = archive_read_data_block(a, &buff, &size, &offset);
                if (r == ARCHIVE_EOF) {
                    break;
                } else if (r < ARCHIVE_OK) {
                    log_error("archive_read_data_block: "
                              + std::string(archive_error_string(a)));
                    outFile.close();
                    archive_read_free(a);
                    return false;
                }
                outFile.write(reinterpret_cast<const char*>(buff), size);
            }
            outFile.close();
            auto mode = archive_entry_perm(entry);
            std::filesystem::permissions(fullOutputPath,
                                         static_cast<std::filesystem::perms>(mode),
                                         std::filesystem::perm_options::replace);
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    log_message("Extracted " + archivePath + " into current directory.");
    return true;
}

/**
 * @struct GitProgress
 * Contains a single double to track the last printed progress percentage in the clone process.
 */
struct GitProgress {
    double lastPercent = 0.0;
};

/**
 * @brief Callback invoked by libgit2 during clone/fetch progress. Displays a combined
 *        "receiving + indexing" progress bar.
 */
static int transferProgress(const git_transfer_progress* stats, void* payload)
{
    // Avoid dividing by zero if total_objects not known
    if (stats->total_objects == 0) {
        return 0;
    }

    double receivingWeight = 0.5;
    double indexingWeight = 0.5;
    double receiveRatio = static_cast<double>(stats->received_objects) / stats->total_objects;
    double indexRatio   = static_cast<double>(stats->indexed_objects)  / stats->total_objects;
    double overallProgress = receivingWeight * receiveRatio + indexingWeight * indexRatio;

    double percent = overallProgress * 100.0;

    GitProgress* gp = static_cast<GitProgress*>(payload);
    if (percent - gp->lastPercent >= 1.0) {
        gp->lastPercent = percent;

        int barWidth = 50;
        int pos = static_cast<int>(overallProgress * barWidth);

        std::string bar(barWidth, ' ');
        for (int i = 0; i < pos; ++i) {
            bar[i] = '#';
        }

        fprintf(stderr, "\r[%-50s] %3.0f%%  (recv %.0f%%, idx %.0f%%)",
                bar.c_str(), percent, receiveRatio * 100.0, indexRatio * 100.0);
        fflush(stderr);
    }
    return 0;
}

/**
 * @brief Clones a Git repo to the specified directory, showing a progress bar via libgit2.
 *
 * Skips if the target directory already exists and is non-empty.
 *
 * @param url The remote Git repository URL.
 * @param destDir The local directory where it will be cloned.
 * @return True on success, false otherwise.
 */
bool cloneGitRepo(const std::string &url, const std::string &destDir)
{
    // If directory is non-empty, skip clone to avoid conflicts
    if (std::filesystem::exists(destDir) && !std::filesystem::is_empty(destDir)) {
        log_message("Directory '" + destDir + "' already exists; skipping clone...");
        return true;
    }

    // Use default clone options with a custom fetch progress callback
    git_clone_options cloneOpts = GIT_CLONE_OPTIONS_INIT;
    cloneOpts.fetch_opts.callbacks.transfer_progress = transferProgress;

    GitProgress gp;
    cloneOpts.fetch_opts.callbacks.payload = &gp;

    git_repository *repo = nullptr;
    int error = git_clone(&repo, url.c_str(), destDir.c_str(), &cloneOpts);

    fprintf(stderr, "\n"); // new line after progress bar

    if (error < 0) {
        const git_error *e = git_error_last();
        std::string msg = (e && e->message) ? e->message : "unknown error";
        log_error("Git clone failed: " + msg);
        if (repo) {
            git_repository_free(repo);
        }
        return false;
    }

    git_repository_free(repo);
    return true;
}

/**
 * @brief fetchSources:
 *        - For each source, determines if it's a Git URL, remote URL, or local file
 *        - If Git, calls cloneGitRepo
 *        - If remote, calls downloadFile
 *        - If local, copies the file
 *        - If recognized as an archive, calls extractArchive (unless NOEXTRACT is in the name)
 *
 * The results are tracked in 'intermediatePaths' for subsequent cleanup or reference.
 *
 * @param sources The list of source strings from the STARBUILD.
 * @param intermediatePaths A container to store file/directory names that we create (for cleanup).
 * @param starbuildDir Path to the directory containing the STARBUILD file.
 * @return True if all sources were processed successfully, false otherwise.
 */
bool fetchSources(const std::vector<std::string>& sources,
                  std::vector<std::string>& intermediatePaths,
                  const std::filesystem::path &starbuildDir)
{
    for (auto &src : sources) {
        // 1) If it starts with "git+", treat as a Git repo
        if (src.rfind("git+", 0) == 0) {
            std::string gitUrl = src.substr(4);
            size_t frag = gitUrl.find_first_of("#?");
            if (frag != std::string::npos) {
                gitUrl = gitUrl.substr(0, frag);
            }

            // Derive local directory name
            size_t pos = gitUrl.find_last_of('/');
            std::string repoName = (pos != std::string::npos) ? gitUrl.substr(pos + 1)
                                                              : gitUrl;
            // Remove trailing .git
            if (repoName.size() >= 4 && repoName.substr(repoName.size() - 4) == ".git") {
                repoName.erase(repoName.size() - 4);
            }

            if (std::filesystem::exists(repoName) && !std::filesystem::is_empty(repoName)) {
                log_message("Directory '" + repoName + "' already exists; skipping clone...");
                intermediatePaths.push_back(repoName);
                continue;
            }

            log_message("Cloning Git repo: " + gitUrl + " => " + repoName);
            if (!cloneGitRepo(gitUrl, repoName)) {
                return false;
            }
            intermediatePaths.push_back(repoName);
            continue;
        }

        // 2) If there's a custom filename in "name::URL" format
        std::string customFilename;
        std::string actualUrl;
        size_t doubleColonPos = src.find("::");
        if (doubleColonPos != std::string::npos) {
            customFilename = src.substr(0, doubleColonPos);
            actualUrl      = src.substr(doubleColonPos + 2);
        }

        // If we do have a custom name+URL
        if (!customFilename.empty() && !actualUrl.empty()) {
            bool isRemote = (actualUrl.find("://") != std::string::npos);
            if (!isRemote) {
                log_error("Invalid custom URL syntax: " + src);
                return false;
            }
            if (!std::filesystem::exists(customFilename)) {
                log_message("Downloading to custom file name: " + customFilename
                            + " from " + actualUrl);
                if (!downloadFile(actualUrl, customFilename)) {
                    log_error("Could not download " + actualUrl);
                    return false;
                }
            } else {
                log_message("File already exists, skipping download: " + customFilename);
            }
            intermediatePaths.push_back(customFilename);
            // Possibly extract if recognized as an archive
            if (isArchiveFile(customFilename)) {
                if (!extractArchive(customFilename)) {
                    return false;
                }
            }
            continue;
        }

        // 3) Otherwise, normal fallback: check if it's a remote URL or local file
        bool isURL = (src.find("://") != std::string::npos);
        std::string filename;

        if (isURL) {
            size_t pos = src.find_last_of("/");
            filename = (pos != std::string::npos) ? src.substr(pos + 1) : src;
            if (filename.empty()) {
                filename = "source.tar";
            }
            if (!std::filesystem::exists(filename)) {
                if (!downloadFile(src, filename)) {
                    log_error("Could not download " + src);
                    return false;
                }
            } else {
                log_message("File already exists, skipping download: " + filename);
            }
        } else {
            // local file
            std::filesystem::path srcPath = starbuildDir / src;
            if (!std::filesystem::exists(srcPath)) {
                log_error("Local source file does not exist: " + srcPath.string());
                return false;
            }
            filename = srcPath.filename().string();
            if (!std::filesystem::exists(filename)) {
                try {
                    std::filesystem::copy_file(srcPath, filename,
                                               std::filesystem::copy_options::overwrite_existing);
                    log_message("Copied local file: " + filename);
                } catch (std::filesystem::filesystem_error &e) {
                    log_error(std::string("Failed to copy local file: ") + e.what());
                    return false;
                }
            } else {
                log_message("Local file already present: " + filename);
            }
        }

        intermediatePaths.push_back(filename);
        if (isArchiveFile(filename)) {
            if (!extractArchive(filename)) {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief runWithBash: Invokes /bin/bash -c '...' optionally under fakeroot.
 *
 * Sets environment variables: pkgdir, packagedir, srcdir, package_name, package_version.
 * Accepts a shell script body as a string. If the script is empty, does nothing.
 *
 * @param script The shell script content to run.
 * @param pkg_packagedir The "files/" directory for the subpackage or single package
 * @param srcdir The directory containing STARBUILD and any local source files
 * @param package_name The subpackage or single package name
 * @param package_version The package version
 * @return True if script returns 0, false otherwise.
 */
static bool runWithBash(const std::string &script,
                        const std::string &pkg_packagedir,
                        const std::string &srcdir,
                        const std::string &package_name,
                        const std::string &package_version)
{
    if (script.empty()) {
        return true;
    }

    // Escape single quotes for correct embedding in 'bash -c'
    std::string escapedScript = script;
    size_t pos = 0;
    while ((pos = escapedScript.find('\'', pos)) != std::string::npos) {
        escapedScript.replace(pos, 1, "'\\''");
        pos += 4;
    }

    std::string cmdPrefix = useFakeroot ? "fakeroot " : "";
    std::string cmd =
        cmdPrefix + "/bin/bash -c '"
        "export pkgdir=\"" + pkg_packagedir + "\" && "
        "export packagedir=\"" + pkg_packagedir + "\" && "
        "export srcdir=\"" + srcdir + "\" && "
        "export package_name=\"" + package_name + "\" && "
        "export package_version=\"" + package_version + "\" && "
        + escapedScript + "'";

    int ret = std::system(cmd.c_str());
    return (ret == 0);
}

/**
 * @brief postProcessFiles:
 *        - Strips unneeded symbols from binaries (unless noStripping is true)
 *        - Removes .la and .a files
 *
 * Called after the subpackage's "assemble" script completes. If `noStripping`
 * is set, the entire step is skipped.
 *
 * @param packagedir The subpackage directory containing "files/" for the build.
 * @return True on success (including if noStripping is enabled), false otherwise.
 */
static bool postProcessFiles(const std::string &packagedir)
{
    if (noStripping) {
        log_message("nostripping flag enabled; skipping binary stripping and .la/.a removal.");
        return true; // proceed, no error
    }

    // 1) Stripping ELF binaries
    int ret = std::system("command -v strip > /dev/null 2>&1"); 
    if (ret != 0) {
        // No 'strip' available, warn but don't fail
        log_warning("'strip' command not found. Binaries won't be stripped.");
    } else {
        log_message("Stripping binaries in " + packagedir + "...");
        // Use 'find' to locate candidates and strip them.
        std::string stripCmd =
            "find " + packagedir +
            R"( -type f ! -name '*.o' -exec strip --strip-unneeded --strip-debug {} + )"
            " > /dev/null 2>&1";
        ret = std::system(stripCmd.c_str());
        if (ret != 0) {
            log_warning("Strip command returned non-zero exit code " + std::to_string(ret)
                        + "; check logs for potential errors.");
        } else {
            log_message("Finished stripping binaries for " + packagedir + ".");
        }
    }

    // 2) Remove .la files
    try {
        bool removed_la = false;
        for (const auto &p : fs::recursive_directory_iterator(
                                 packagedir, fs::directory_options::skip_permission_denied))
        {
            std::error_code ec_stat;
            auto status = fs::symlink_status(p.path(), ec_stat);
            if (!ec_stat && fs::is_regular_file(status) && p.path().extension() == ".la") {
                log_message("Removing " + p.path().string());
                std::error_code ec_remove;
                if (!fs::remove(p.path(), ec_remove) && ec_remove) {
                    log_warning("Failed to remove .la file " + p.path().string()
                                + ": " + ec_remove.message());
                } else {
                    removed_la = true;
                }
            }
        }
        if (!removed_la) {
            log_message("No .la files found in " + packagedir + ".");
        }
    } catch (const fs::filesystem_error &e) {
        log_error("Error while removing .la files: " + std::string(e.what()));
    }

    // 3) Remove .a files
    try {
        bool removed_a = false;
        for (const auto &p : fs::recursive_directory_iterator(
                                 packagedir, fs::directory_options::skip_permission_denied))
        {
            std::error_code ec_stat;
            auto status = fs::symlink_status(p.path(), ec_stat);
            if (!ec_stat && fs::is_regular_file(status) && p.path().extension() == ".a") {
                log_message("Removing " + p.path().string());
                std::error_code ec_remove;
                if (!fs::remove(p.path(), ec_remove) && ec_remove) {
                    log_warning("Failed to remove .a file " + p.path().string()
                                + ": " + ec_remove.message());
                } else {
                    removed_a = true;
                }
            }
        }
        if (!removed_a) {
            log_message("No .a files found in " + packagedir + ".");
        }
    } catch (const fs::filesystem_error &e) {
        log_error("Error while removing .a files: " + std::string(e.what()));
    }

    return true; // Non-fatal if it can't strip or remove .la/.a
}

/**
 * @brief packageStarpack: Combines metadata.yaml, files/, hooks, etc. into a
 *        single .starpack archive. The archive is tarred with transformed paths
 *        (leading to "files/" except for metadata.yaml and hooks).
 *
 * @param starbuildDirStr  The directory containing the user's starbuild scripts & possibly hooks.
 * @param packagedir       The subpackage directory to be archived.
 * @param metadataContent  The metadata.yaml contents as a string.
 * @param outputFile       The final .starpack output path.
 * @param symlinkPairs     Any symlinks to be created in packagedir prior to tar.
 * @param pkgName          The name of the subpackage (or single package).
 * @param singlePackage    True if there's only one package in the build.
 * @return True on success, false otherwise.
 */
bool packageStarpack(const std::string &starbuildDirStr,
                     const std::string &packagedir,
                     const std::string &metadataContent,
                     const std::string &outputFile,
                     const std::vector<std::pair<std::string, std::string>> &symlinkPairs,
                     const std::string &pkgName,
                     bool singlePackage)
{
    // 1) Write metadata.yaml into packagedir
    fs::path metaPath = fs::path(packagedir) / "metadata.yaml";
    try {
        std::ofstream metaOut(metaPath, std::ios::binary);
        if (!metaOut.is_open()) {
            log_error("Failed to write metadata.yaml to " + metaPath.string());
            return false;
        }
        metaOut.write(metadataContent.data(), static_cast<std::streamsize>(metadataContent.size()));
        metaOut.close();
        log_message("Wrote metadata.yaml to " + metaPath.string());
    } catch (const std::exception &ex) {
        log_error("Exception writing metadata.yaml: " + std::string(ex.what()));
        return false;
    }

    // 2) Attempt to copy hooks. If singlePackage = false, it relies on other logic, etc.
    std::vector<fs::path> hooksToCopy;
    fs::path srcDirPath = fs::path(starbuildDirStr);

    // 3) Create symlinks in packagedir as specified by symlinkPairs
    for (const auto &pair : symlinkPairs) {
        fs::path linkPath = fs::path(packagedir) / pair.first;
        if (fs::exists(linkPath)) {
            log_warning("Symlink target " + linkPath.string() + " already exists; skipping creation.");
            continue;
        }
        fs::create_directories(linkPath.parent_path());
        std::error_code ec;
        fs::create_symlink(pair.second, linkPath, ec);
        if (ec) {
            log_error("Failed to create symlink " + linkPath.string() + " -> " + pair.second
                      + ": " + ec.message());
            return false;
        }
        log_message("Created symlink: " + linkPath.string() + " -> " + pair.second);
    }

    // 4) Use tar & zstd to produce the final .starpack
    //    Transform paths so that leading "./" => "files/", except for metadata.yaml => "metadata.yaml"
    auto shellEscape = [](const std::string &path) {
        std::ostringstream oss;
        oss << "\"" << path << "\"";
        return oss.str();
    };

    std::ostringstream cmd;
    cmd << "cd " << shellEscape(packagedir)
        << " && tar --owner=0 --group=0 "
        << "--transform='s|^\\./metadata\\.yaml$|metadata.yaml|' "
        << "--transform=\"s|^\\./hooks|hooks|\" "
        << "--transform='s|^\\./|files/|' "
        << "-cf - ."
        << " | zstd --ultra -22 -v" // Added zstd compression
        << " > " << shellEscape(outputFile);

    log_message("Running tar command:\n" + cmd.str());

    int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
        log_error("tar|zstd command failed with exit code " + std::to_string(ret));
        return false;
    }

    log_message("Successfully created starpack archive: " + outputFile);
    return true;
}

/**
 * @brief cleanupBuildArtifacts: Removes directories/files created during the build process,
 *        such as "files" or any downloaded sources in intermediatePaths.
 *
 * @param starbuildDir The directory containing STARBUILD.
 * @param intermediatePaths A list of local filenames or directories to remove.
 */
void cleanupBuildArtifacts(const fs::path &starbuildDir,
                           const std::vector<std::string>& intermediatePaths)
{
    fs::path filesDir = starbuildDir / "files";
    if (fs::exists(filesDir)) {
        fs::remove_all(filesDir);
        log_message("Removed directory: " + filesDir.string());
    }
    for (const auto &pathStr : intermediatePaths) {
        fs::path p = starbuildDir / pathStr;
        if (fs::exists(p)) {
            fs::remove_all(p);
            log_message("Removed: " + p.string());
        }
    }
}

/**
 * @brief createPackage: The main starpack build pipeline for single or multi-package
 *        defined in a STARBUILD. This function orchestrates:
 *        - parse_starbuild
 *        - fetchSources
 *        - running user scripts (prepare, compile, verify, assemble)
 *        - postProcessFiles (strip, remove .la/.a)
 *        - packageStarpack (tar+zstd)
 *        - optional cleanup of intermediate artifacts
 *
 * @param starbuildPath Path to the STARBUILD file
 * @param clean Whether to remove intermediate artifacts afterward
 * @return True if everything succeeds, false otherwise.
 */
bool createPackage(const std::string &starbuildPath, bool clean)
{
    using namespace std::filesystem;

    std::vector<std::string> package_names;
    std::vector<std::string> package_descriptions;
    std::unordered_map<std::string, std::vector<std::string>> subpackageDependencies;

    std::string package_version;
    std::string description;

    std::vector<std::string> dependencies;
    std::vector<std::string> build_dependencies;
    std::vector<std::string> sources;

    std::string prepare_function;
    std::string compile_function;
    std::string verify_function;
    std::string generic_assemble_function;

    std::unordered_map<std::string, std::string> assemble_functions;
    std::vector<std::pair<std::string, std::string>> symlinkPairs;

    // 1) Parse the STARBUILD file
    if (!parse_starbuild(
            starbuildPath,
            package_names,
            package_descriptions,
            subpackageDependencies,
            package_version,
            description,
            dependencies,
            build_dependencies,
            sources,
            prepare_function,
            compile_function,
            verify_function,
            generic_assemble_function,
            assemble_functions,
            symlinkPairs
        ))
    {
        log_error("Failed to parse STARBUILD: " + starbuildPath);
        return false;
    }

    if (package_names.empty()) {
        log_error("No package_name defined in STARBUILD.");
        return false;
    }

    // Directory containing STARBUILD
    path starbuildDir = absolute(starbuildPath).parent_path();
    std::string srcdir = starbuildDir.string();

    // 2) Fetch sources (downloads, clones, local copies) and store intermediate paths for cleanup
    std::vector<std::string> intermediatePaths;
    if (!fetchSources(sources, intermediatePaths, starbuildDir)) {
        log_error("fetchSources() failed.");
        return false;
    }

    // 3) run prepare(), compile(), verify() globally
    log_message("Running prepare()...");
    if (!runWithBash(prepare_function, srcdir, srcdir, package_names[0], package_version)) {
        log_error("prepare() failed.");
        return false;
    }

    log_message("Running compile()...");
    if (!runWithBash(compile_function, srcdir, srcdir, package_names[0], package_version)) {
        log_error("compile() failed.");
        return false;
    }

    log_message("Running verify()...");
    if (!runWithBash(verify_function, srcdir, srcdir, package_names[0], package_version)) {
        log_error("verify() failed.");
        return false;
    }

    // 4) For each subpackage, assemble and post-process
    for (size_t i = 0; i < package_names.size(); i++) {
        const std::string &pkgName = package_names[i];

        // Staging directory "packages/pkgName/files"
        fs::path pkgDir = starbuildDir / "packages" / pkgName / "files";
        create_directories(pkgDir);
        std::string pkg_packagedir = pkgDir.string();

        // If single package, auto-copy hooks. If multiple, skip
        if (package_names.size() == 1) {
            // create "hooks" in etc/starpack.d/universal-hooks
            fs::path hooksDir = pkgDir / "etc" / "starpack.d" / "universal-hooks";
            fs::create_directories(hooksDir);

            // Copy all *.hook that match pkgName prefix and do not start with a digit after
            std::regex singleHookPattern("^(?!\\d).+\\.hook$", std::regex_constants::icase);
            for (auto &entry : fs::directory_iterator(starbuildDir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                std::string filename = entry.path().filename().string();
                if (std::regex_match(filename, singleHookPattern)) {
                    fs::path dest = hooksDir / filename;
                    try {
                        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
                        log_message("Auto-copied hook " + filename + " => " + dest.string());
                    } catch (const fs::filesystem_error &ex) {
                        log_error("Failed to copy hook " + filename + ": " + ex.what());
                    }
                }
            }
        } else {
            // Multi-package scenario
            log_message("Multiple packages detected; auto-hook copying is disabled for " + pkgName);
        }

        // 4a) run assemble_<pkg> or assemble() if no subpackage assembly
        log_message("Assembling package: " + pkgName);
        bool assembleResult = false;
        auto it = assemble_functions.find(pkgName);
        if (it != assemble_functions.end()) {
            log_message("Running assemble_" + pkgName + "() function...");
            assembleResult = runWithBash(it->second, pkg_packagedir, srcdir, pkgName, package_version);
        } else if (!generic_assemble_function.empty()) {
            log_message("Running assemble() function for package " + pkgName + "...");
            assembleResult = runWithBash(generic_assemble_function, pkg_packagedir, srcdir, pkgName, package_version);
        } else {
            log_message("No assemble function defined for package " + pkgName
                        + ". Skipping assemble phase.");
            assembleResult = true; // OK
        }

        if (!assembleResult) {
            log_error("Assemble phase failed for package " + pkgName);
            return false;
        }

        // 4b) strip binaries, remove .la / .a files
        if (!postProcessFiles(pkg_packagedir)) {
            log_error("Post-processing failed for package " + pkgName);
            return false;
        }

        // Build final dependencies array: global + subpackage
        std::vector<std::string> finalDeps = dependencies;
        auto subIt = subpackageDependencies.find(pkgName);
        if (subIt != subpackageDependencies.end()) {
            finalDeps.insert(finalDeps.end(),
                             subIt->second.begin(), subIt->second.end());
        }

        // Build YAML for metadata
        YAML::Node metadata;
        metadata["name"]        = pkgName;
        metadata["version"]     = package_version;

        // If package_descriptions is large enough, use the subpackage's own description
        std::string pkgDesc = description;
        if (i < package_descriptions.size()) {
            pkgDesc = package_descriptions[i];
        }
        metadata["description"] = pkgDesc;

        // Insert final dependencies
        YAML::Node depsNode(YAML::NodeType::Sequence);
        for (auto &dep : finalDeps) {
            depsNode.push_back(dep);
        }
        metadata["dependencies"] = depsNode;

        // Turn the YAML node into a string
        YAML::Emitter emitter;
        emitter << metadata;
        std::string metadataContent = emitter.c_str();

        // Single vs multiple package check
        bool isSinglePackage = (package_names.size() == 1);

        // Final .starpack file named "pkgName-version.starpack" in the starbuild directory
        std::string outputFile =
            (fs::path(srcdir) / (pkgName + "-" + package_version + ".starpack")).string();

        // Tar+zstd the subpackage
        bool ok = packageStarpack(
            starbuildDir.string(),
            pkgDir.string(),
            metadataContent,
            outputFile,
            symlinkPairs,
            pkgName,
            isSinglePackage
        );
        if (!ok) {
            log_error("Packaging failed for package " + pkgName);
            return false;
        }
    }

    log_message("All steps complete. Final .starpack archive(s) have been created.");

    // If user wants to do a cleanup pass
    if (clean) {
        log_message("Cleaning up intermediate files...");
        cleanupBuildArtifacts(starbuildDir, intermediatePaths);
    }
    return true;
}

} // namespace CreateStarpack
} // namespace Starpack

/**
 * @brief handleNoStripFlag - Called if user passes --nostrip. 
 *        Sets global noStripping = true in the build system logic.
 */
void handleNoStripFlag()
{
    std::cout << "No-strip flag enabled: binaries will not be stripped.\n";
}

/**
 * @brief handleNoFakerootFlag - Called if user passes --no-fakeroot.
 *        Disables the global useFakeroot variable in the logic.
 */
void handleNoFakerootFlag()
{
    std::cout << "No-fakeroot flag enabled: fakeroot will be disabled.\n";
}

/**
 * @brief main - Entry point for create-starpack. 
 *
 * Ensures the process runs as root, initializes libgit2, parses command-line flags,
 * and invokes createPackage(...) with the provided STARBUILD path.
 *
 * @return 0 on success, non-zero on error.
 */
int main(int argc, char* argv[])
{
     // Check if the effective UID is 0 (root)
     if (geteuid() == 0) {
        // The user is root, so we give a warning
        std::cerr << "Warning: It is generally NOT recommended to run create-starpack as root.\n"
                  << "You are doing this at your own risk!\n"
                  << "Do you want to proceed anyway? [y/N] ";

        // Read a single line of user input
        std::string response;
        std::getline(std::cin, response);

        // Trim response (optional) and convert to lowercase
        auto trim_left  = [](std::string &s) { s.erase(0, s.find_first_not_of(" \t\n\r")); };
        auto trim_right = [](std::string &s) { s.erase(s.find_last_not_of(" \t\n\r") + 1); };
        trim_left(response);
        trim_right(response);
        std::transform(response.begin(), response.end(), response.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

        // If the response is not 'y' or 'yes', bail out
        if (response != "y" && response != "yes") {
            std::cerr << "Aborting at user request.\n";
            return 1;
        }

        // Otherwise, the user confirmed they want to proceed
        std::cerr << "Proceeding as root (at your own risk)!\n";
    }
    else {
        // The user is NOT root, so no special warning is needed.
        // You can still do a mild informational message if desired:
        std::cerr << "Running create-starpack as a non-root user. Proceeding...\n";
    }

    // Initialize libgit2 for usage in clone/fetch
    git_libgit2_init();

    bool clean         = false;   // If true, remove intermediate artifacts
    bool localNoStrip  = false;   // If true, skip binary stripping
    bool noFakeroot    = false;   // If true, disable fakeroot usage
    std::string starbuildPath;    // Path to the STARBUILD file

    // Parse command-line flags
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--clean") {
            clean = true;
        } else if (arg == "--nostrip") {
            localNoStrip = true;
        } else if (arg == "--no-fakeroot") {
            noFakeroot = true;
        } else if (arg.rfind("--", 0) == 0) {
            // ignore unknown --foo flags
            continue;
        } else {
            // interpret the first non-flag as the path to STARBUILD
            starbuildPath = arg;
        }
    }

    if (starbuildPath.empty()) {
        // Default fallback if user didn't provide one
        starbuildPath = "./STARBUILD";
    }

    // Apply the flags to the logic used by create-starpack
    if (localNoStrip) {
        std::cout << "No-strip flag enabled: binaries will not be stripped.\n";
        Starpack::CreateStarpack::noStripping = true;
    }
    if (noFakeroot) {
        std::cout << "No-fakeroot flag enabled: fakeroot will be disabled.\n";
        Starpack::CreateStarpack::useFakeroot = false;
    }

    // Run the main createPackage pipeline
    bool success = Starpack::CreateStarpack::createPackage(starbuildPath, clean);

    git_libgit2_shutdown();

    if (!success) {
        std::cerr << "Failed to create starpack from " << starbuildPath << "\n";
        return 1;
    }

    return 0;
}
