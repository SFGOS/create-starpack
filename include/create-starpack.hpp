#ifndef CREATE_STARPACK_HPP
#define CREATE_STARPACK_HPP

#include <string>
#include <vector>
#include <utility> // for std::pair
#include <ostream>
#include <iostream>

// ---------------------------------------------------------------------------
// ANSI Color Macros for Console Output
// ---------------------------------------------------------------------------
// These are used in the inline logging functions below.
#define COLOR_RESET "\033[0m"
#define COLOR_INFO  "\033[32m"
#define COLOR_WARN  "\033[33m"
#define COLOR_ERROR "\033[31m"

namespace Starpack {
namespace CreateStarpack {

/**
 * @brief A global flag indicating whether commands should be run under fakeroot.
 *
 * This is typically set to true by default if the user is not root. If the user
 * provides an argument like "--no-fakeroot," this can be disabled. This is useful
 * when packaging in an environment that requires faking file ownership or permissions.
 */
extern bool useFakeroot;

/**
 * @brief A global flag that indicates whether binary stripping should be disabled.
 *
 * If true, the system will skip calls to 'strip' on built/assembled binaries, preserving
 * debug symbols and size. Settable via a command-line flag like "--nostrip."
 */
extern bool noStripping;

/**
 * @brief Creates a starpack package from a given STARBUILD file.
 *
 * @param starbuildPath The full or relative path to the STARBUILD script.
 * @param clean If true, intermediate build artifacts (downloaded sources, etc.)
 *        are removed once the .starpack archives are produced.
 * @return True on success, false if an error occurs at any stage of the packaging pipeline.
 *
 * This function orchestrates reading the STARBUILD, fetching sources, running
 * user-supplied scripts (prepare, compile, verify, assemble), optionally stripping
 * binaries and removing .la/.a files, and finally bundling everything into a ".starpack" file.
 */
bool createPackage(const std::string &starbuildPath, bool clean);

/**
 * @brief Clones a Git repository into a given directory, showing a progress bar.
 *
 * Typically used to handle "git+<URL>" style sources. Uses libgit2 for the clone,
 * providing real-time progress updates on the console. Skips cloning if the
 * directory is already non-empty.
 *
 * @param gitUrl The remote repository URL (e.g. "git+https://github.com/user/repo.git").
 * @param destDir The local directory name or path to clone into.
 * @return True if the clone completes successfully or is skipped, false on failure.
 */
bool cloneGitRepo(const std::string &gitUrl, const std::string &destDir);

/**
 * @brief Parses a STARBUILD file, extracting definitions for package names, version,
 *        dependencies, scripts, and symlinks. Typically used in createPackage().
 *
 *
 * @param filepath        The path to the STARBUILD file.
 * @param package_names   Output list of package names (one or more).
 * @param package_version The shared version string for these packages.
 * @param description     The generic or fallback description if multiple packages exist.
 * @param preinstall      Possibly a script or function body for pre-install steps.
 * @param postinstall     Possibly a script or function body for post-install steps.
 * @param dependencies    A global fallback list of package dependencies.
 * @param build_dependencies Additional list of build-time dependencies.
 * @param sources         A list of source strings (URLs, local paths, git+...).
 * @param prepare_function  Script body for "prepare()" logic.
 * @param compile_function  Script body for "compile()" logic.
 * @param verify_function   Script body for "verify()" logic.
 * @param assemble_function Script body for "assemble()" logic.
 * @param hooks           Possibly a list of .hook filenames found or declared in the file.
 * @param symlinkPairs    A list of symlink pairs ("link", "target") for special packaging.
 *
 * @return True on successful parse, false otherwise.
 */
bool parse_starbuild(const std::string &filepath,
    std::vector<std::string> &package_names,
    std::string &package_version,
    std::string &description,
    std::string &preinstall,
    std::string &postinstall,
    std::vector<std::string> &dependencies,
    std::vector<std::string> &clashes,
    std::vector<std::string> &gives,
    std::vector<std::string> &optional_dependencies,
    std::vector<std::string> &build_dependencies,
    std::vector<std::string> &sources,
    std::string &prepare_function,
    std::string &compile_function,
    std::string &verify_function,
    std::string &assemble_function,
    std::vector<std::string> &hooks,
    std::vector<std::pair<std::string, std::string>> &symlinkPairs
);

// ---------------------------------------------------------------------------
// Inline Logging Functions
// ---------------------------------------------------------------------------

/**
 * @brief Logs an informational message in green color to stderr, prefixed with "[INFO]".
 *
 * @param message The message text to log.
 */
inline void log_message(const std::string &message)
{
    std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << message << std::endl;
}

/**
 * @brief Logs a warning message in yellow color to stderr, prefixed with "[WARN]".
 *
 * @param message The warning message text to log.
 */
inline void log_warning(const std::string &message)
{
    std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << message << std::endl;
}

/**
 * @brief Logs an error message in red color to stderr, prefixed with "[ERROR]".
 *
 * @param message The error message text to log.
 */
inline void log_error(const std::string &message)
{
    std::cerr << COLOR_ERROR << "[ERROR] " << COLOR_RESET << message << std::endl;
}

// ---------------------------------------------------------------------------
// Additional Utility Declarations
// ---------------------------------------------------------------------------

/**
 * @brief A libcurl write callback function used for retrieving HTTP responses into a buffer.
 *
 *
 * @param contents Data pointer from libcurl.
 * @param size     Size of each data chunk (in bytes).
 * @param nmemb    Number of data chunks.
 * @param userp    A pointer to user-defined data (like a std::string).
 * @return The number of bytes processed, or 0 on error.
 */
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);

/**
 * @brief Fetches repository data from a URL (e.g., "repo.db.yaml") and returns it as a string.
 *
 * This might use libcurl internally to perform a GET request, storing the response in memory.
 *
 * @param url The remote URL to fetch from.
 * @return A string containing the retrieved data (possibly YAML content).
 */
std::string fetchRepoData(const std::string& url);

/**
 * @brief Removes the portion of a string from the first slash or backslash onward.
 *
 * For instance, "mypackage/arch" => "mypackage", "mypackage\stuff" => "mypackage".
 * If neither slash nor backslash is found, returns the original string.
 *
 * @param input The input string to modify.
 * @return The substring up to (but not including) the first slash or backslash.
 */
std::string removeSlashAndAfter(const std::string& input);

} // namespace CreateStarpack
} // namespace Starpack

#endif // CREATE_STARPACK_HPP
