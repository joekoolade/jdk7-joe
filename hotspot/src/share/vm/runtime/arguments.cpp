/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

/*
 * This file has been modified by Azul Systems, Inc. in 2014. These
 * modifications are Copyright (c) 2014 Azul Systems, Inc., and are made
 * available on the same license terms set forth above. 
 */

#include "precompiled.hpp"
#include "classfile/javaAssertions.hpp"
#include "compiler/compilerOracle.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/cardTableRS.hpp"
#include "memory/referenceProcessor.hpp"
#include "memory/universe.inline.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"
#include "services/management.hpp"
#include "services/memTracker.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/taskqueue.hpp"
#ifdef TARGET_OS_FAMILY_linux
# include "os_linux.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "os_solaris.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "os_windows.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "os_bsd.inline.hpp"
#endif
#ifndef SERIALGC
#include "gc_implementation/concurrentMarkSweep/compactibleFreeListSpace.hpp"
#endif

// Note: This is a special bug reporting site for the JVM
#define DEFAULT_VENDOR_URL_BUG "http://www.azulsystems.com/support/"
#define DEFAULT_JAVA_LAUNCHER  "generic"

char**  Arguments::_jvm_flags_array             = NULL;
int     Arguments::_num_jvm_flags               = 0;
char**  Arguments::_jvm_args_array              = NULL;
int     Arguments::_num_jvm_args                = 0;
char*  Arguments::_java_command                 = NULL;
SystemProperty* Arguments::_system_properties   = NULL;
const char*  Arguments::_gc_log_filename        = NULL;
bool   Arguments::_has_profile                  = false;
bool   Arguments::_has_alloc_profile            = false;
uintx  Arguments::_min_heap_size                = 0;
Arguments::Mode Arguments::_mode                = _mixed;
bool   Arguments::_java_compiler                = false;
bool   Arguments::_xdebug_mode                  = false;
const char*  Arguments::_java_vendor_url_bug    = DEFAULT_VENDOR_URL_BUG;
const char*  Arguments::_sun_java_launcher      = DEFAULT_JAVA_LAUNCHER;
int    Arguments::_sun_java_launcher_pid        = -1;
bool   Arguments::_created_by_gamma_launcher    = false;

// These parameters are reset in method parse_vm_init_args(JavaVMInitArgs*)
bool   Arguments::_AlwaysCompileLoopMethods     = AlwaysCompileLoopMethods;
bool   Arguments::_UseOnStackReplacement        = UseOnStackReplacement;
bool   Arguments::_BackgroundCompilation        = BackgroundCompilation;
bool   Arguments::_ClipInlining                 = ClipInlining;

char*  Arguments::SharedArchivePath             = NULL;

AgentLibraryList Arguments::_libraryList;
AgentLibraryList Arguments::_agentList;

abort_hook_t     Arguments::_abort_hook         = NULL;
exit_hook_t      Arguments::_exit_hook          = NULL;
vfprintf_hook_t  Arguments::_vfprintf_hook      = NULL;


SystemProperty *Arguments::_java_ext_dirs = NULL;
SystemProperty *Arguments::_java_endorsed_dirs = NULL;
SystemProperty *Arguments::_sun_boot_library_path = NULL;
SystemProperty *Arguments::_java_library_path = NULL;
SystemProperty *Arguments::_java_home = NULL;
SystemProperty *Arguments::_java_class_path = NULL;
SystemProperty *Arguments::_sun_boot_class_path = NULL;

char* Arguments::_meta_index_path = NULL;
char* Arguments::_meta_index_dir = NULL;

// Check if head of 'option' matches 'name', and sets 'tail' remaining part of option string

static bool match_option(const JavaVMOption *option, const char* name,
                         const char** tail) {
  int len = (int)strlen(name);
  if (strncmp(option->optionString, name, len) == 0) {
    *tail = option->optionString + len;
    return true;
  } else {
    return false;
  }
}

static void logOption(const char* opt) {
  if (PrintVMOptions) {
    jio_fprintf(defaultStream::output_stream(), "VM option '%s'\n", opt);
  }
}

// Process java launcher properties.
void Arguments::process_sun_java_launcher_properties(JavaVMInitArgs* args) {
  // See if sun.java.launcher or sun.java.launcher.pid is defined.
  // Must do this before setting up other system properties,
  // as some of them may depend on launcher type.
  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption* option = args->options + index;
    const char* tail;

    if (match_option(option, "-Dsun.java.launcher=", &tail)) {
      process_java_launcher_argument(tail, option->extraInfo);
      continue;
    }
    if (match_option(option, "-Dsun.java.launcher.pid=", &tail)) {
      _sun_java_launcher_pid = atoi(tail);
      continue;
    }
    if (match_option(option, "-XX:+OverrideVMProperties", &tail)) {
      FLAG_SET_CMDLINE(bool, OverrideVMProperties, true);
    }
  }
}

// Initialize system properties key and value.
void Arguments::init_system_properties() {
  // If OverrideVMProperties is enabled, make the properties writeable.
  // This may be needed to appease some tools.
  const bool writable = OverrideVMProperties;

  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.specification.name", "Java Virtual Machine Specification",  writable));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.version", VM_Version::vm_release(),  writable));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.name", VM_Version::vm_name(),  writable));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.info", VM_Version::vm_info_string(),  true));

  // following are JVMTI agent writeable properties.
  // Properties values are set to NULL and they are
  // os specific they are initialized in os::init_system_properties_values().
  _java_ext_dirs = new SystemProperty("java.ext.dirs", NULL,  true);
  _java_endorsed_dirs = new SystemProperty("java.endorsed.dirs", NULL,  true);
  _sun_boot_library_path = new SystemProperty("sun.boot.library.path", NULL,  true);
  _java_library_path = new SystemProperty("java.library.path", NULL,  true);
  _java_home =  new SystemProperty("java.home", NULL,  true);
  _sun_boot_class_path = new SystemProperty("sun.boot.class.path", NULL,  true);

  _java_class_path = new SystemProperty("java.class.path", "",  true);

  // Add to System Property list.
  PropertyList_add(&_system_properties, _java_ext_dirs);
  PropertyList_add(&_system_properties, _java_endorsed_dirs);
  PropertyList_add(&_system_properties, _sun_boot_library_path);
  PropertyList_add(&_system_properties, _java_library_path);
  PropertyList_add(&_system_properties, _java_home);
  PropertyList_add(&_system_properties, _java_class_path);
  PropertyList_add(&_system_properties, _sun_boot_class_path);

  // Set OS specific system properties values
  os::init_system_properties_values();
}


  // Update/Initialize System properties after JDK version number is known
void Arguments::init_version_specific_system_properties() {
  enum { bufsz = 16 };
  char buffer[bufsz];
  const char* spec_vendor = "Sun Microsystems Inc.";
  uint32_t spec_version = 0;

  if (JDK_Version::is_gte_jdk17x_version()) {
    spec_vendor = "Oracle Corporation";
    spec_version = JDK_Version::current().major_version();
  }
  jio_snprintf(buffer, bufsz, "1." UINT32_FORMAT, spec_version);

  // If OverrideVMProperties is enabled, make the properties writeable.
  // This may be needed to appease some tools.
  const bool writable = OverrideVMProperties;

  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.specification.vendor",  spec_vendor, writable));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.specification.version", buffer,      writable));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.vendor", VM_Version::vm_vendor(),    writable));
}

/**
 * Provide a slightly more user-friendly way of eliminating -XX flags.
 * When a flag is eliminated, it can be added to this list in order to
 * continue accepting this flag on the command-line, while issuing a warning
 * and ignoring the value.  Once the JDK version reaches the 'accept_until'
 * limit, we flatly refuse to admit the existence of the flag.  This allows
 * a flag to die correctly over JDK releases using HSX.
 */
typedef struct {
  const char* name;
  JDK_Version obsoleted_in; // when the flag went away
  JDK_Version accept_until; // which version to start denying the existence
} ObsoleteFlag;

static ObsoleteFlag obsolete_jvm_flags[] = {
  { "UseTrainGC",                    JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "UseSpecialLargeObjectHandling", JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "UseOversizedCarHandling",       JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "TraceCarAllocation",            JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "PrintTrainGCProcessingStats",   JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "LogOfCarSpaceSize",             JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "OversizedCarThreshold",         JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "MinTickInterval",               JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "DefaultTickInterval",           JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "MaxTickInterval",               JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "DelayTickAdjustment",           JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "ProcessingToTenuringRatio",     JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "MinTrainLength",                JDK_Version::jdk(5), JDK_Version::jdk(7) },
  { "AppendRatio",         JDK_Version::jdk_update(6,10), JDK_Version::jdk(7) },
  { "DefaultMaxRAM",       JDK_Version::jdk_update(6,18), JDK_Version::jdk(7) },
  { "DefaultInitialRAMFraction",
                           JDK_Version::jdk_update(6,18), JDK_Version::jdk(7) },
  { "UseDepthFirstScavengeOrder",
                           JDK_Version::jdk_update(6,22), JDK_Version::jdk(7) },
  { "HandlePromotionFailure",
                           JDK_Version::jdk_update(6,24), JDK_Version::jdk(8) },
  { "MaxLiveObjectEvacuationRatio",
                           JDK_Version::jdk_update(6,24), JDK_Version::jdk(8) },
  { "ForceSharedSpaces",   JDK_Version::jdk_update(6,25), JDK_Version::jdk(8) },
  { "UseParallelOldGCCompacting",
                           JDK_Version::jdk_update(6,27), JDK_Version::jdk(8) },
  { "UseParallelDensePrefixUpdate",
                           JDK_Version::jdk_update(6,27), JDK_Version::jdk(8) },
  { "UseParallelOldGCDensePrefix",
                           JDK_Version::jdk_update(6,27), JDK_Version::jdk(8) },
  { "AllowTransitionalJSR292",       JDK_Version::jdk(7), JDK_Version::jdk(8) },
  { "UseCompressedStrings",          JDK_Version::jdk(7), JDK_Version::jdk(8) },
  { "AlwaysInflate",       JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "AnonymousClasses",    JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "CMSOldPLABReactivityCeiling",
                           JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "EventLogLength",      JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "GCOverheadReporting", JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "GCOverheadReportingPeriodMS",
                           JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "HPILibPath",          JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "PreSpinYield",        JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "PreBlockSpin",        JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "PostSpinYield",       JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "PreserveMarkStackSize",
                           JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "ReadSpinIterations",  JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "StressTieredRuntime", JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "Tier1Inline",         JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "Tier1FreqInlineSize", JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "Tier1MaxInlineSize",  JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "Tier1LoopOptsCount",  JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
  { "UseSpinning",         JDK_Version::jdk_update(7,40), JDK_Version::jdk(8) },
#ifdef PRODUCT
  { "DesiredMethodLimit",
                           JDK_Version::jdk_update(7, 2), JDK_Version::jdk(8) },
#endif // PRODUCT
  { NULL, JDK_Version(0), JDK_Version(0) }
};

// Returns true if the flag is obsolete and fits into the range specified
// for being ignored.  In the case that the flag is ignored, the 'version'
// value is filled in with the version number when the flag became
// obsolete so that that value can be displayed to the user.
bool Arguments::is_newly_obsolete(const char *s, JDK_Version* version) {
  int i = 0;
  assert(version != NULL, "Must provide a version buffer");
  while (obsolete_jvm_flags[i].name != NULL) {
    const ObsoleteFlag& flag_status = obsolete_jvm_flags[i];
    // <flag>=xxx form
    // [-|+]<flag> form
    if ((strncmp(flag_status.name, s, strlen(flag_status.name)) == 0) ||
        ((s[0] == '+' || s[0] == '-') &&
        (strncmp(flag_status.name, &s[1], strlen(flag_status.name)) == 0))) {
      if (JDK_Version::current().compare(flag_status.accept_until) == -1) {
          *version = flag_status.obsoleted_in;
          return true;
      }
    }
    i++;
  }
  return false;
}

// Constructs the system class path (aka boot class path) from the following
// components, in order:
//
//     prefix           // from -Xbootclasspath/p:...
//     endorsed         // the expansion of -Djava.endorsed.dirs=...
//     base             // from os::get_system_properties() or -Xbootclasspath=
//     suffix           // from -Xbootclasspath/a:...
//
// java.endorsed.dirs is a list of directories; any jar or zip files in the
// directories are added to the sysclasspath just before the base.
//
// This could be AllStatic, but it isn't needed after argument processing is
// complete.
class SysClassPath: public StackObj {
public:
  SysClassPath(const char* base);
  ~SysClassPath();

  inline void set_base(const char* base);
  inline void add_prefix(const char* prefix);
  inline void add_suffix_to_prefix(const char* suffix);
  inline void add_suffix(const char* suffix);
  inline void reset_path(const char* base);

  // Expand the jar/zip files in each directory listed by the java.endorsed.dirs
  // property.  Must be called after all command-line arguments have been
  // processed (in particular, -Djava.endorsed.dirs=...) and before calling
  // combined_path().
  void expand_endorsed();

  inline const char* get_base()     const { return _items[_scp_base]; }
  inline const char* get_prefix()   const { return _items[_scp_prefix]; }
  inline const char* get_suffix()   const { return _items[_scp_suffix]; }
  inline const char* get_endorsed() const { return _items[_scp_endorsed]; }

  // Combine all the components into a single c-heap-allocated string; caller
  // must free the string if/when no longer needed.
  char* combined_path();

private:
  // Utility routines.
  static char* add_to_path(const char* path, const char* str, bool prepend);
  static char* add_jars_to_path(char* path, const char* directory);

  inline void reset_item_at(int index);

  // Array indices for the items that make up the sysclasspath.  All except the
  // base are allocated in the C heap and freed by this class.
  enum {
    _scp_prefix,        // from -Xbootclasspath/p:...
    _scp_endorsed,      // the expansion of -Djava.endorsed.dirs=...
    _scp_base,          // the default sysclasspath
    _scp_suffix,        // from -Xbootclasspath/a:...
    _scp_nitems         // the number of items, must be last.
  };

  const char* _items[_scp_nitems];
  DEBUG_ONLY(bool _expansion_done;)
};

SysClassPath::SysClassPath(const char* base) {
  memset(_items, 0, sizeof(_items));
  _items[_scp_base] = base;
  DEBUG_ONLY(_expansion_done = false;)
}

SysClassPath::~SysClassPath() {
  // Free everything except the base.
  for (int i = 0; i < _scp_nitems; ++i) {
    if (i != _scp_base) reset_item_at(i);
  }
  DEBUG_ONLY(_expansion_done = false;)
}

inline void SysClassPath::set_base(const char* base) {
  _items[_scp_base] = base;
}

inline void SysClassPath::add_prefix(const char* prefix) {
  _items[_scp_prefix] = add_to_path(_items[_scp_prefix], prefix, true);
}

inline void SysClassPath::add_suffix_to_prefix(const char* suffix) {
  _items[_scp_prefix] = add_to_path(_items[_scp_prefix], suffix, false);
}

inline void SysClassPath::add_suffix(const char* suffix) {
  _items[_scp_suffix] = add_to_path(_items[_scp_suffix], suffix, false);
}

inline void SysClassPath::reset_item_at(int index) {
  assert(index < _scp_nitems && index != _scp_base, "just checking");
  if (_items[index] != NULL) {
    FREE_C_HEAP_ARRAY(char, _items[index], mtInternal);
    _items[index] = NULL;
  }
}

inline void SysClassPath::reset_path(const char* base) {
  // Clear the prefix and suffix.
  reset_item_at(_scp_prefix);
  reset_item_at(_scp_suffix);
  set_base(base);
}

//------------------------------------------------------------------------------

void SysClassPath::expand_endorsed() {
  assert(_items[_scp_endorsed] == NULL, "can only be called once.");

  const char* path = Arguments::get_property("java.endorsed.dirs");
  if (path == NULL) {
    path = Arguments::get_endorsed_dir();
    assert(path != NULL, "no default for java.endorsed.dirs");
  }

  char* expanded_path = NULL;
  const char separator = *os::path_separator();
  const char* const end = path + strlen(path);
  while (path < end) {
    const char* tmp_end = strchr(path, separator);
    if (tmp_end == NULL) {
      expanded_path = add_jars_to_path(expanded_path, path);
      path = end;
    } else {
      char* dirpath = NEW_C_HEAP_ARRAY(char, tmp_end - path + 1, mtInternal);
      memcpy(dirpath, path, tmp_end - path);
      dirpath[tmp_end - path] = '\0';
      expanded_path = add_jars_to_path(expanded_path, dirpath);
      FREE_C_HEAP_ARRAY(char, dirpath, mtInternal);
      path = tmp_end + 1;
    }
  }
  _items[_scp_endorsed] = expanded_path;
  DEBUG_ONLY(_expansion_done = true;)
}

// Combine the bootclasspath elements, some of which may be null, into a single
// c-heap-allocated string.
char* SysClassPath::combined_path() {
  assert(_items[_scp_base] != NULL, "empty default sysclasspath");
  assert(_expansion_done, "must call expand_endorsed() first.");

  size_t lengths[_scp_nitems];
  size_t total_len = 0;

  const char separator = *os::path_separator();

  // Get the lengths.
  int i;
  for (i = 0; i < _scp_nitems; ++i) {
    if (_items[i] != NULL) {
      lengths[i] = strlen(_items[i]);
      // Include space for the separator char (or a NULL for the last item).
      total_len += lengths[i] + 1;
    }
  }
  assert(total_len > 0, "empty sysclasspath not allowed");

  // Copy the _items to a single string.
  char* cp = NEW_C_HEAP_ARRAY(char, total_len, mtInternal);
  char* cp_tmp = cp;
  for (i = 0; i < _scp_nitems; ++i) {
    if (_items[i] != NULL) {
      memcpy(cp_tmp, _items[i], lengths[i]);
      cp_tmp += lengths[i];
      *cp_tmp++ = separator;
    }
  }
  *--cp_tmp = '\0';     // Replace the extra separator.
  return cp;
}

// Note:  path must be c-heap-allocated (or NULL); it is freed if non-null.
char*
SysClassPath::add_to_path(const char* path, const char* str, bool prepend) {
  char *cp;

  assert(str != NULL, "just checking");
  if (path == NULL) {
    size_t len = strlen(str) + 1;
    cp = NEW_C_HEAP_ARRAY(char, len, mtInternal);
    memcpy(cp, str, len);                       // copy the trailing null
  } else {
    const char separator = *os::path_separator();
    size_t old_len = strlen(path);
    size_t str_len = strlen(str);
    size_t len = old_len + str_len + 2;

    if (prepend) {
      cp = NEW_C_HEAP_ARRAY(char, len, mtInternal);
      char* cp_tmp = cp;
      memcpy(cp_tmp, str, str_len);
      cp_tmp += str_len;
      *cp_tmp = separator;
      memcpy(++cp_tmp, path, old_len + 1);      // copy the trailing null
      FREE_C_HEAP_ARRAY(char, path, mtInternal);
    } else {
      cp = REALLOC_C_HEAP_ARRAY(char, path, len, mtInternal);
      char* cp_tmp = cp + old_len;
      *cp_tmp = separator;
      memcpy(++cp_tmp, str, str_len + 1);       // copy the trailing null
    }
  }
  return cp;
}

// Scan the directory and append any jar or zip files found to path.
// Note:  path must be c-heap-allocated (or NULL); it is freed if non-null.
char* SysClassPath::add_jars_to_path(char* path, const char* directory) {
  DIR* dir = os::opendir(directory);
  if (dir == NULL) return path;

  char dir_sep[2] = { '\0', '\0' };
  size_t directory_len = strlen(directory);
  const char fileSep = *os::file_separator();
  if (directory[directory_len - 1] != fileSep) dir_sep[0] = fileSep;

  /* Scan the directory for jars/zips, appending them to path. */
  struct dirent *entry;
  char *dbuf = NEW_C_HEAP_ARRAY(char, os::readdir_buf_size(directory), mtInternal);
  while ((entry = os::readdir(dir, (dirent *) dbuf)) != NULL) {
    const char* name = entry->d_name;
    const char* ext = name + strlen(name) - 4;
    bool isJarOrZip = ext > name &&
      (os::file_name_strcmp(ext, ".jar") == 0 ||
       os::file_name_strcmp(ext, ".zip") == 0);
    if (isJarOrZip) {
      char* jarpath = NEW_C_HEAP_ARRAY(char, directory_len + 2 + strlen(name), mtInternal);
      sprintf(jarpath, "%s%s%s", directory, dir_sep, name);
      path = add_to_path(path, jarpath, false);
      FREE_C_HEAP_ARRAY(char, jarpath, mtInternal);
    }
  }
  FREE_C_HEAP_ARRAY(char, dbuf, mtInternal);
  os::closedir(dir);
  return path;
}

// Parses a memory size specification string.
static bool atomull(const char *s, julong* result) {
  julong n = 0;
  int args_read = sscanf(s, os::julong_format_specifier(), &n);
  if (args_read != 1) {
    return false;
  }
  while (*s != '\0' && isdigit(*s)) {
    s++;
  }
  // 4705540: illegal if more characters are found after the first non-digit
  if (strlen(s) > 1) {
    return false;
  }
  switch (*s) {
    case 'T': case 't':
      *result = n * G * K;
      // Check for overflow.
      if (*result/((julong)G * K) != n) return false;
      return true;
    case 'G': case 'g':
      *result = n * G;
      if (*result/G != n) return false;
      return true;
    case 'M': case 'm':
      *result = n * M;
      if (*result/M != n) return false;
      return true;
    case 'K': case 'k':
      *result = n * K;
      if (*result/K != n) return false;
      return true;
    case '\0':
      *result = n;
      return true;
    default:
      return false;
  }
}

Arguments::ArgsRange Arguments::check_memory_size(julong size, julong min_size) {
  if (size < min_size) return arg_too_small;
  // Check that size will fit in a size_t (only relevant on 32-bit)
  if (size > max_uintx) return arg_too_big;
  return arg_in_range;
}

// Describe an argument out of range error
void Arguments::describe_range_error(ArgsRange errcode) {
  switch(errcode) {
  case arg_too_big:
    jio_fprintf(defaultStream::error_stream(),
                "The specified size exceeds the maximum "
                "representable size.\n");
    break;
  case arg_too_small:
  case arg_unreadable:
  case arg_in_range:
    // do nothing for now
    break;
  default:
    ShouldNotReachHere();
  }
}

static bool set_bool_flag(char* name, bool value, FlagValueOrigin origin) {
  return CommandLineFlags::boolAtPut(name, &value, origin);
}

static bool set_fp_numeric_flag(char* name, char* value, FlagValueOrigin origin) {
  double v;
  if (sscanf(value, "%lf", &v) != 1) {
    return false;
  }

  if (CommandLineFlags::doubleAtPut(name, &v, origin)) {
    return true;
  }
  return false;
}

static bool set_numeric_flag(char* name, char* value, FlagValueOrigin origin) {
  julong v;
  intx intx_v;
  bool is_neg = false;
  // Check the sign first since atomull() parses only unsigned values.
  if (*value == '-') {
    if (!CommandLineFlags::intxAt(name, &intx_v)) {
      return false;
    }
    value++;
    is_neg = true;
  }
  if (!atomull(value, &v)) {
    return false;
  }
  intx_v = (intx) v;
  if (is_neg) {
    intx_v = -intx_v;
  }
  if (CommandLineFlags::intxAtPut(name, &intx_v, origin)) {
    return true;
  }
  uintx uintx_v = (uintx) v;
  if (!is_neg && CommandLineFlags::uintxAtPut(name, &uintx_v, origin)) {
    return true;
  }
  uint64_t uint64_t_v = (uint64_t) v;
  if (!is_neg && CommandLineFlags::uint64_tAtPut(name, &uint64_t_v, origin)) {
    return true;
  }
  return false;
}

static bool set_string_flag(char* name, const char* value, FlagValueOrigin origin) {
  if (!CommandLineFlags::ccstrAtPut(name, &value, origin))  return false;
  // Contract:  CommandLineFlags always returns a pointer that needs freeing.
  FREE_C_HEAP_ARRAY(char, value, mtInternal);
  return true;
}

static bool append_to_string_flag(char* name, const char* new_value, FlagValueOrigin origin) {
  const char* old_value = "";
  if (!CommandLineFlags::ccstrAt(name, &old_value))  return false;
  size_t old_len = old_value != NULL ? strlen(old_value) : 0;
  size_t new_len = strlen(new_value);
  const char* value;
  char* free_this_too = NULL;
  if (old_len == 0) {
    value = new_value;
  } else if (new_len == 0) {
    value = old_value;
  } else {
    char* buf = NEW_C_HEAP_ARRAY(char, old_len + 1 + new_len + 1, mtInternal);
    // each new setting adds another LINE to the switch:
    sprintf(buf, "%s\n%s", old_value, new_value);
    value = buf;
    free_this_too = buf;
  }
  (void) CommandLineFlags::ccstrAtPut(name, &value, origin);
  // CommandLineFlags always returns a pointer that needs freeing.
  FREE_C_HEAP_ARRAY(char, value, mtInternal);
  if (free_this_too != NULL) {
    // CommandLineFlags made its own copy, so I must delete my own temp. buffer.
    FREE_C_HEAP_ARRAY(char, free_this_too, mtInternal);
  }
  return true;
}

bool Arguments::parse_argument(const char* arg, FlagValueOrigin origin) {

  // range of acceptable characters spelled out for portability reasons
#define NAME_RANGE  "[abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_]"
#define BUFLEN 255
  char name[BUFLEN+1];
  char dummy;

  if (sscanf(arg, "-%" XSTR(BUFLEN) NAME_RANGE "%c", name, &dummy) == 1) {
    return set_bool_flag(name, false, origin);
  }
  if (sscanf(arg, "+%" XSTR(BUFLEN) NAME_RANGE "%c", name, &dummy) == 1) {
    return set_bool_flag(name, true, origin);
  }

  char punct;
  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "%c", name, &punct) == 2 && punct == '=') {
    const char* value = strchr(arg, '=') + 1;
    Flag* flag = Flag::find_flag(name, strlen(name));
    if (flag != NULL && flag->is_ccstr()) {
      if (flag->ccstr_accumulates()) {
        return append_to_string_flag(name, value, origin);
      } else {
        if (value[0] == '\0') {
          value = NULL;
        }
        return set_string_flag(name, value, origin);
      }
    }
  }

  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE ":%c", name, &punct) == 2 && punct == '=') {
    const char* value = strchr(arg, '=') + 1;
    // -XX:Foo:=xxx will reset the string flag to the given value.
    if (value[0] == '\0') {
      value = NULL;
    }
    return set_string_flag(name, value, origin);
  }

#define SIGNED_FP_NUMBER_RANGE "[-0123456789.]"
#define SIGNED_NUMBER_RANGE    "[-0123456789]"
#define        NUMBER_RANGE    "[0123456789]"
  char value[BUFLEN + 1];
  char value2[BUFLEN + 1];
  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "=" "%" XSTR(BUFLEN) SIGNED_NUMBER_RANGE "." "%" XSTR(BUFLEN) NUMBER_RANGE "%c", name, value, value2, &dummy) == 3) {
    // Looks like a floating-point number -- try again with more lenient format string
    if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "=" "%" XSTR(BUFLEN) SIGNED_FP_NUMBER_RANGE "%c", name, value, &dummy) == 2) {
      return set_fp_numeric_flag(name, value, origin);
    }
  }

#define VALUE_RANGE "[-kmgtKMGT0123456789]"
  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "=" "%" XSTR(BUFLEN) VALUE_RANGE "%c", name, value, &dummy) == 2) {
    return set_numeric_flag(name, value, origin);
  }

  return false;
}

void Arguments::add_string(char*** bldarray, int* count, const char* arg) {
  assert(bldarray != NULL, "illegal argument");

  if (arg == NULL) {
    return;
  }

  int index = *count;

  // expand the array and add arg to the last element
  (*count)++;
  if (*bldarray == NULL) {
    *bldarray = NEW_C_HEAP_ARRAY(char*, *count, mtInternal);
  } else {
    *bldarray = REALLOC_C_HEAP_ARRAY(char*, *bldarray, *count, mtInternal);
  }
  (*bldarray)[index] = strdup(arg);
}

void Arguments::build_jvm_args(const char* arg) {
  add_string(&_jvm_args_array, &_num_jvm_args, arg);
}

void Arguments::build_jvm_flags(const char* arg) {
  add_string(&_jvm_flags_array, &_num_jvm_flags, arg);
}

// utility function to return a string that concatenates all
// strings in a given char** array
const char* Arguments::build_resource_string(char** args, int count) {
  if (args == NULL || count == 0) {
    return NULL;
  }
  size_t length = strlen(args[0]) + 1; // add 1 for the null terminator
  for (int i = 1; i < count; i++) {
    length += strlen(args[i]) + 1; // add 1 for a space
  }
  char* s = NEW_RESOURCE_ARRAY(char, length);
  strcpy(s, args[0]);
  for (int j = 1; j < count; j++) {
    strcat(s, " ");
    strcat(s, args[j]);
  }
  return (const char*) s;
}

void Arguments::print_on(outputStream* st) {
  st->print_cr("VM Arguments:");
  if (num_jvm_flags() > 0) {
    st->print("jvm_flags: "); print_jvm_flags_on(st);
  }
  if (num_jvm_args() > 0) {
    st->print("jvm_args: "); print_jvm_args_on(st);
  }
  st->print_cr("java_command: %s", java_command() ? java_command() : "<unknown>");
  st->print_cr("Launcher Type: %s", _sun_java_launcher);
}

void Arguments::print_jvm_flags_on(outputStream* st) {
  if (_num_jvm_flags > 0) {
    for (int i=0; i < _num_jvm_flags; i++) {
      st->print("%s ", _jvm_flags_array[i]);
    }
    st->print_cr("");
  }
}

void Arguments::print_jvm_args_on(outputStream* st) {
  if (_num_jvm_args > 0) {
    for (int i=0; i < _num_jvm_args; i++) {
      st->print("%s ", _jvm_args_array[i]);
    }
    st->print_cr("");
  }
}

bool Arguments::process_argument(const char* arg,
    jboolean ignore_unrecognized, FlagValueOrigin origin) {

  JDK_Version since = JDK_Version();

  if (parse_argument(arg, origin) || ignore_unrecognized) {
    return true;
  }

  bool has_plus_minus = (*arg == '+' || *arg == '-');
  const char* const argname = has_plus_minus ? arg + 1 : arg;
  if (is_newly_obsolete(arg, &since)) {
    char version[256];
    since.to_string(version, sizeof(version));
    warning("ignoring option %s; support was removed in %s", argname, version);
    return true;
  }

  // For locked flags, report a custom error message if available.
  // Otherwise, report a standard VM option message.

  size_t arg_len;
  const char* equal_sign = strchr(argname, '=');
  if (equal_sign == NULL) {
    arg_len = strlen(argname);
  } else {
    arg_len = equal_sign - argname;
  }

  Flag* found_flag = Flag::find_flag((char*)argname, arg_len, true);
  if (found_flag != NULL) {
    char locked_message_buf[BUFLEN];
    found_flag->get_locked_message(locked_message_buf, BUFLEN);
    if (strlen(locked_message_buf) == 0) {
     if (found_flag->is_bool() && !has_plus_minus) {
        jio_fprintf(defaultStream::error_stream(),
                    "Missing +/- setting for VM option '%s'\n", argname);
      } else if (!found_flag->is_bool() && has_plus_minus) {
        jio_fprintf(defaultStream::error_stream(),
                    "Unexpected +/- setting in VM option '%s'\n", argname);
      } else {
        jio_fprintf(defaultStream::error_stream(),
                    "Improperly specified VM option '%s'\n", argname);
      }
    } else {
      jio_fprintf(defaultStream::error_stream(), "%s", locked_message_buf);
    }
  } else {
    jio_fprintf(defaultStream::error_stream(),
                "Unrecognized VM option '%s'\n", argname);
  }

  // allow for commandline "commenting out" options like -XX:#+Verbose
  return arg[0] == '#';
}

bool Arguments::process_settings_file(const char* file_name, bool should_exist, jboolean ignore_unrecognized) {
  FILE* stream = fopen(file_name, "rb");
  if (stream == NULL) {
    if (should_exist) {
      jio_fprintf(defaultStream::error_stream(),
                  "Could not open settings file %s\n", file_name);
      return false;
    } else {
      return true;
    }
  }

  char token[1024];
  int  pos = 0;

  bool in_white_space = true;
  bool in_comment     = false;
  bool in_quote       = false;
  char quote_c        = 0;
  bool result         = true;

  int c = getc(stream);
  while(c != EOF && pos < (int)(sizeof(token)-1)) {
    if (in_white_space) {
      if (in_comment) {
        if (c == '\n') in_comment = false;
      } else {
        if (c == '#') in_comment = true;
        else if (!isspace(c)) {
          in_white_space = false;
          token[pos++] = c;
        }
      }
    } else {
      if (c == '\n' || (!in_quote && isspace(c))) {
        // token ends at newline, or at unquoted whitespace
        // this allows a way to include spaces in string-valued options
        token[pos] = '\0';
        logOption(token);
        result &= process_argument(token, ignore_unrecognized, CONFIG_FILE);
        build_jvm_flags(token);
        pos = 0;
        in_white_space = true;
        in_quote = false;
      } else if (!in_quote && (c == '\'' || c == '"')) {
        in_quote = true;
        quote_c = c;
      } else if (in_quote && (c == quote_c)) {
        in_quote = false;
      } else {
        token[pos++] = c;
      }
    }
    c = getc(stream);
  }
  if (pos > 0) {
    token[pos] = '\0';
    result &= process_argument(token, ignore_unrecognized, CONFIG_FILE);
    build_jvm_flags(token);
  }
  fclose(stream);
  return result;
}

//=============================================================================================================
// Parsing of properties (-D)

const char* Arguments::get_property(const char* key) {
  return PropertyList_get_value(system_properties(), key);
}

bool Arguments::add_property(const char* prop) {
  const char* eq = strchr(prop, '=');
  char* key;
  // ns must be static--its address may be stored in a SystemProperty object.
  const static char ns[1] = {0};
  char* value = (char *)ns;

  size_t key_len = (eq == NULL) ? strlen(prop) : (eq - prop);
  key = AllocateHeap(key_len + 1, mtInternal);
  strncpy(key, prop, key_len);
  key[key_len] = '\0';

  if (eq != NULL) {
    size_t value_len = strlen(prop) - key_len - 1;
    value = AllocateHeap(value_len + 1, mtInternal);
    strncpy(value, &prop[key_len + 1], value_len + 1);
  }

  if (strcmp(key, "java.compiler") == 0) {
    process_java_compiler_argument(value);
    FreeHeap(key);
    if (eq != NULL) {
      FreeHeap(value);
    }
    return true;
  } else if (strcmp(key, "sun.java.command") == 0) {
    _java_command = value;

    // Record value in Arguments, but let it get passed to Java.
  } else if (strcmp(key, "sun.java.launcher.pid") == 0) {
    // launcher.pid property is private and is processed
    // in process_sun_java_launcher_properties();
    // the sun.java.launcher property is passed on to the java application
    FreeHeap(key);
    if (eq != NULL) {
      FreeHeap(value);
    }
    return true;
  } else if (strcmp(key, "java.vendor.url.bug") == 0) {
    // save it in _java_vendor_url_bug, so JVM fatal error handler can access
    // its value without going through the property list or making a Java call.
    _java_vendor_url_bug = value;
  } else if (strcmp(key, "sun.boot.library.path") == 0) {
    PropertyList_unique_add(&_system_properties, key, value, true);
    return true;
  }
  // Create new property and add at the end of the list
  PropertyList_unique_add(&_system_properties, key, value);
  return true;
}

//===========================================================================================================
// Setting int/mixed/comp mode flags

void Arguments::set_mode_flags(Mode mode) {
  // Set up default values for all flags.
  // If you add a flag to any of the branches below,
  // add a default value for it here.
  set_java_compiler(false);
  _mode                      = mode;

  // Ensure Agent_OnLoad has the correct initial values.
  // This may not be the final mode; mode may change later in onload phase.
  PropertyList_unique_add(&_system_properties, "java.vm.info",
                          (char*)VM_Version::vm_info_string(), false);

  UseInterpreter             = true;
  UseCompiler                = true;
  UseLoopCounter             = true;

#ifndef ZERO
  // Turn these off for mixed and comp.  Leave them on for Zero.
  if (FLAG_IS_DEFAULT(UseFastAccessorMethods)) {
    UseFastAccessorMethods = (mode == _int);
  }
  if (FLAG_IS_DEFAULT(UseFastEmptyMethods)) {
    UseFastEmptyMethods = (mode == _int);
  }
#endif

  // Default values may be platform/compiler dependent -
  // use the saved values
  ClipInlining               = Arguments::_ClipInlining;
  AlwaysCompileLoopMethods   = Arguments::_AlwaysCompileLoopMethods;
  UseOnStackReplacement      = Arguments::_UseOnStackReplacement;
  BackgroundCompilation      = Arguments::_BackgroundCompilation;

  // Change from defaults based on mode
  switch (mode) {
  default:
    ShouldNotReachHere();
    break;
  case _int:
    UseCompiler              = false;
    UseLoopCounter           = false;
    AlwaysCompileLoopMethods = false;
    UseOnStackReplacement    = false;
    break;
  case _mixed:
    // same as default
    break;
  case _comp:
    UseInterpreter           = false;
    BackgroundCompilation    = false;
    ClipInlining             = false;
    // Be much more aggressive in tiered mode with -Xcomp and exercise C2 more.
    // We will first compile a level 3 version (C1 with full profiling), then do one invocation of it and
    // compile a level 4 (C2) and then continue executing it.
    if (TieredCompilation) {
      Tier3InvokeNotifyFreqLog = 0;
      Tier4InvocationThreshold = 0;
    }
    break;
  }
}

// Conflict: required to use shared spaces (-Xshare:on), but
// incompatible command line options were chosen.

static void no_shared_spaces() {
  if (RequireSharedSpaces) {
    jio_fprintf(defaultStream::error_stream(),
      "Class data sharing is inconsistent with other specified options.\n");
    vm_exit_during_initialization("Unable to use shared archive.", NULL);
  } else {
    FLAG_SET_DEFAULT(UseSharedSpaces, false);
  }
}

void Arguments::set_tiered_flags() {
  // With tiered, set default policy to AdvancedThresholdPolicy, which is 3.
  if (FLAG_IS_DEFAULT(CompilationPolicyChoice)) {
    FLAG_SET_DEFAULT(CompilationPolicyChoice, 3);
  }
  if (CompilationPolicyChoice < 2) {
    vm_exit_during_initialization(
      "Incompatible compilation policy selected", NULL);
  }
  // Increase the code cache size - tiered compiles a lot more.
  if (FLAG_IS_DEFAULT(ReservedCodeCacheSize)) {
    FLAG_SET_DEFAULT(ReservedCodeCacheSize, ReservedCodeCacheSize * 2);
  }
}

static void disable_adaptive_size_policy(const char* collector_name) {
  if (UseAdaptiveSizePolicy) {
    if (FLAG_IS_CMDLINE(UseAdaptiveSizePolicy)) {
      warning("disabling UseAdaptiveSizePolicy; it is incompatible with %s.",
              collector_name);
    }
    FLAG_SET_DEFAULT(UseAdaptiveSizePolicy, false);
  }
}

// If the user has chosen ParallelGCThreads > 0, we set UseParNewGC
// if it's not explictly set or unset. If the user has chosen
// UseParNewGC and not explicitly set ParallelGCThreads we
// set it, unless this is a single cpu machine.
void Arguments::set_parnew_gc_flags() {
  assert(!UseSerialGC && !UseParallelOldGC && !UseParallelGC && !UseG1GC,
         "control point invariant");
  assert(UseParNewGC, "Error");

  // Turn off AdaptiveSizePolicy for parnew until it is complete.
  disable_adaptive_size_policy("UseParNewGC");

  if (ParallelGCThreads == 0) {
    FLAG_SET_DEFAULT(ParallelGCThreads,
                     Abstract_VM_Version::parallel_worker_threads());
    if (ParallelGCThreads == 1) {
      FLAG_SET_DEFAULT(UseParNewGC, false);
      FLAG_SET_DEFAULT(ParallelGCThreads, 0);
    }
  }
  if (UseParNewGC) {
    // CDS doesn't work with ParNew yet
    no_shared_spaces();

    // By default YoungPLABSize and OldPLABSize are set to 4096 and 1024 respectively,
    // these settings are default for Parallel Scavenger. For ParNew+Tenured configuration
    // we set them to 1024 and 1024.
    // See CR 6362902.
    if (FLAG_IS_DEFAULT(YoungPLABSize)) {
      FLAG_SET_DEFAULT(YoungPLABSize, (intx)1024);
    }
    if (FLAG_IS_DEFAULT(OldPLABSize)) {
      FLAG_SET_DEFAULT(OldPLABSize, (intx)1024);
    }

    // AlwaysTenure flag should make ParNew promote all at first collection.
    // See CR 6362902.
    if (AlwaysTenure) {
      FLAG_SET_CMDLINE(intx, MaxTenuringThreshold, 0);
    }
    // When using compressed oops, we use local overflow stacks,
    // rather than using a global overflow list chained through
    // the klass word of the object's pre-image.
    if (UseCompressedOops && !ParGCUseLocalOverflow) {
      if (!FLAG_IS_DEFAULT(ParGCUseLocalOverflow)) {
        warning("Forcing +ParGCUseLocalOverflow: needed if using compressed references");
      }
      FLAG_SET_DEFAULT(ParGCUseLocalOverflow, true);
    }
    assert(ParGCUseLocalOverflow || !UseCompressedOops, "Error");
  }
}

// Adjust some sizes to suit CMS and/or ParNew needs; these work well on
// sparc/solaris for certain applications, but would gain from
// further optimization and tuning efforts, and would almost
// certainly gain from analysis of platform and environment.
void Arguments::set_cms_and_parnew_gc_flags() {
  assert(!UseSerialGC && !UseParallelOldGC && !UseParallelGC, "Error");
  assert(UseConcMarkSweepGC, "CMS is expected to be on here");

  // If we are using CMS, we prefer to UseParNewGC,
  // unless explicitly forbidden.
  if (FLAG_IS_DEFAULT(UseParNewGC)) {
    FLAG_SET_ERGO(bool, UseParNewGC, true);
  }

  // Turn off AdaptiveSizePolicy for CMS until it is complete.
  disable_adaptive_size_policy("UseConcMarkSweepGC");

  // In either case, adjust ParallelGCThreads and/or UseParNewGC
  // as needed.
  if (UseParNewGC) {
    set_parnew_gc_flags();
  }

  size_t max_heap = align_size_down(MaxHeapSize,
                                    CardTableRS::ct_max_alignment_constraint());

  // Now make adjustments for CMS
  intx   tenuring_default = (intx)6;
  size_t young_gen_per_worker = CMSYoungGenPerWorker;

  // Preferred young gen size for "short" pauses:
  // upper bound depends on # of threads and NewRatio.
  const uintx parallel_gc_threads =
    (ParallelGCThreads == 0 ? 1 : ParallelGCThreads);
  const size_t preferred_max_new_size_unaligned =
    MIN2(max_heap/(NewRatio+1), ScaleForWordSize(young_gen_per_worker * parallel_gc_threads));
  size_t preferred_max_new_size =
    align_size_up(preferred_max_new_size_unaligned, os::vm_page_size());

  // Unless explicitly requested otherwise, size young gen
  // for "short" pauses ~ CMSYoungGenPerWorker*ParallelGCThreads

  // If either MaxNewSize or NewRatio is set on the command line,
  // assume the user is trying to set the size of the young gen.
  if (FLAG_IS_DEFAULT(MaxNewSize) && FLAG_IS_DEFAULT(NewRatio)) {

    // Set MaxNewSize to our calculated preferred_max_new_size unless
    // NewSize was set on the command line and it is larger than
    // preferred_max_new_size.
    if (!FLAG_IS_DEFAULT(NewSize)) {   // NewSize explicitly set at command-line
      FLAG_SET_ERGO(uintx, MaxNewSize, MAX2(NewSize, preferred_max_new_size));
    } else {
      FLAG_SET_ERGO(uintx, MaxNewSize, preferred_max_new_size);
    }
    if (PrintGCDetails && Verbose) {
      // Too early to use gclog_or_tty
      tty->print_cr("CMS ergo set MaxNewSize: " SIZE_FORMAT, MaxNewSize);
    }

    // Code along this path potentially sets NewSize and OldSize
    if (PrintGCDetails && Verbose) {
      // Too early to use gclog_or_tty
      tty->print_cr("CMS set min_heap_size: " SIZE_FORMAT
           " initial_heap_size:  " SIZE_FORMAT
           " max_heap: " SIZE_FORMAT,
           min_heap_size(), InitialHeapSize, max_heap);
    }
    size_t min_new = preferred_max_new_size;
    if (FLAG_IS_CMDLINE(NewSize)) {
      min_new = NewSize;
    }
    if (max_heap > min_new && min_heap_size() > min_new) {
      // Unless explicitly requested otherwise, make young gen
      // at least min_new, and at most preferred_max_new_size.
      if (FLAG_IS_DEFAULT(NewSize)) {
        FLAG_SET_ERGO(uintx, NewSize, MAX2(NewSize, min_new));
        FLAG_SET_ERGO(uintx, NewSize, MIN2(preferred_max_new_size, NewSize));
        if (PrintGCDetails && Verbose) {
          // Too early to use gclog_or_tty
          tty->print_cr("CMS ergo set NewSize: " SIZE_FORMAT, NewSize);
        }
      }
      // Unless explicitly requested otherwise, size old gen
      // so it's NewRatio x of NewSize.
      if (FLAG_IS_DEFAULT(OldSize)) {
        if (max_heap > NewSize) {
          FLAG_SET_ERGO(uintx, OldSize, MIN2(NewRatio*NewSize, max_heap - NewSize));
          if (PrintGCDetails && Verbose) {
            // Too early to use gclog_or_tty
            tty->print_cr("CMS ergo set OldSize: " SIZE_FORMAT, OldSize);
          }
        }
      }
    }
  }
  // Unless explicitly requested otherwise, definitely
  // promote all objects surviving "tenuring_default" scavenges.
  if (FLAG_IS_DEFAULT(MaxTenuringThreshold) &&
      FLAG_IS_DEFAULT(SurvivorRatio)) {
    FLAG_SET_ERGO(intx, MaxTenuringThreshold, tenuring_default);
  }
  // If we decided above (or user explicitly requested)
  // `promote all' (via MaxTenuringThreshold := 0),
  // prefer minuscule survivor spaces so as not to waste
  // space for (non-existent) survivors
  if (FLAG_IS_DEFAULT(SurvivorRatio) && MaxTenuringThreshold == 0) {
    FLAG_SET_ERGO(intx, SurvivorRatio, MAX2((intx)1024, SurvivorRatio));
  }
  // If OldPLABSize is set and CMSParPromoteBlocksToClaim is not,
  // set CMSParPromoteBlocksToClaim equal to OldPLABSize.
  // This is done in order to make ParNew+CMS configuration to work
  // with YoungPLABSize and OldPLABSize options.
  // See CR 6362902.
  if (!FLAG_IS_DEFAULT(OldPLABSize)) {
    if (FLAG_IS_DEFAULT(CMSParPromoteBlocksToClaim)) {
      // OldPLABSize is not the default value but CMSParPromoteBlocksToClaim
      // is.  In this situtation let CMSParPromoteBlocksToClaim follow
      // the value (either from the command line or ergonomics) of
      // OldPLABSize.  Following OldPLABSize is an ergonomics decision.
      FLAG_SET_ERGO(uintx, CMSParPromoteBlocksToClaim, OldPLABSize);
    } else {
      // OldPLABSize and CMSParPromoteBlocksToClaim are both set.
      // CMSParPromoteBlocksToClaim is a collector-specific flag, so
      // we'll let it to take precedence.
      jio_fprintf(defaultStream::error_stream(),
                  "Both OldPLABSize and CMSParPromoteBlocksToClaim"
                  " options are specified for the CMS collector."
                  " CMSParPromoteBlocksToClaim will take precedence.\n");
    }
  }
  if (!FLAG_IS_DEFAULT(ResizeOldPLAB) && !ResizeOldPLAB) {
    // OldPLAB sizing manually turned off: Use a larger default setting,
    // unless it was manually specified. This is because a too-low value
    // will slow down scavenges.
    if (FLAG_IS_DEFAULT(CMSParPromoteBlocksToClaim)) {
      FLAG_SET_ERGO(uintx, CMSParPromoteBlocksToClaim, 50); // default value before 6631166
    }
  }
  // Overwrite OldPLABSize which is the variable we will internally use everywhere.
  FLAG_SET_ERGO(uintx, OldPLABSize, CMSParPromoteBlocksToClaim);
  // If either of the static initialization defaults have changed, note this
  // modification.
  if (!FLAG_IS_DEFAULT(CMSParPromoteBlocksToClaim) || !FLAG_IS_DEFAULT(OldPLABWeight)) {
    CFLS_LAB::modify_initialization(OldPLABSize, OldPLABWeight);
  }
  if (PrintGCDetails && Verbose) {
    tty->print_cr("MarkStackSize: %uk  MarkStackSizeMax: %uk",
      MarkStackSize / K, MarkStackSizeMax / K);
    tty->print_cr("ConcGCThreads: %u", ConcGCThreads);
  }
}

void set_object_alignment() {
  // Object alignment.
  assert(is_power_of_2(ObjectAlignmentInBytes), "ObjectAlignmentInBytes must be power of 2");
  MinObjAlignmentInBytes     = ObjectAlignmentInBytes;
  assert(MinObjAlignmentInBytes >= HeapWordsPerLong * HeapWordSize, "ObjectAlignmentInBytes value is too small");
  MinObjAlignment            = MinObjAlignmentInBytes / HeapWordSize;
  assert(MinObjAlignmentInBytes == MinObjAlignment * HeapWordSize, "ObjectAlignmentInBytes value is incorrect");
  MinObjAlignmentInBytesMask = MinObjAlignmentInBytes - 1;

  LogMinObjAlignmentInBytes  = exact_log2(ObjectAlignmentInBytes);
  LogMinObjAlignment         = LogMinObjAlignmentInBytes - LogHeapWordSize;

  // Oop encoding heap max
  OopEncodingHeapMax = (uint64_t(max_juint) + 1) << LogMinObjAlignmentInBytes;

  // Set CMS global values
  CompactibleFreeListSpace::set_cms_values();
}

bool verify_object_alignment() {
  // Object alignment.
  if (!is_power_of_2(ObjectAlignmentInBytes)) {
    jio_fprintf(defaultStream::error_stream(),
                "error: ObjectAlignmentInBytes=%d must be power of 2\n",
                (int)ObjectAlignmentInBytes);
    return false;
  }
  if ((int)ObjectAlignmentInBytes < BytesPerLong) {
    jio_fprintf(defaultStream::error_stream(),
                "error: ObjectAlignmentInBytes=%d must be greater or equal %d\n",
                (int)ObjectAlignmentInBytes, BytesPerLong);
    return false;
  }
  // It does not make sense to have big object alignment
  // since a space lost due to alignment will be greater
  // then a saved space from compressed oops.
  if ((int)ObjectAlignmentInBytes > 256) {
    jio_fprintf(defaultStream::error_stream(),
                "error: ObjectAlignmentInBytes=%d must not be greater then 256\n",
                (int)ObjectAlignmentInBytes);
    return false;
  }
  // In case page size is very small.
  if ((int)ObjectAlignmentInBytes >= os::vm_page_size()) {
    jio_fprintf(defaultStream::error_stream(),
                "error: ObjectAlignmentInBytes=%d must be less then page size %d\n",
                (int)ObjectAlignmentInBytes, os::vm_page_size());
    return false;
  }
  return true;
}

inline uintx max_heap_for_compressed_oops() {
  // Avoid sign flip.
  if (OopEncodingHeapMax < MaxPermSize + os::vm_page_size()) {
    return 0;
  }
  LP64_ONLY(return OopEncodingHeapMax - MaxPermSize - os::vm_page_size());
  NOT_LP64(ShouldNotReachHere(); return 0);
}

bool Arguments::should_auto_select_low_pause_collector() {
  if (UseAutoGCSelectPolicy &&
      !FLAG_IS_DEFAULT(MaxGCPauseMillis) &&
      (MaxGCPauseMillis <= AutoGCSelectPauseMillis)) {
    if (PrintGCDetails) {
      // Cannot use gclog_or_tty yet.
      tty->print_cr("Automatic selection of the low pause collector"
       " based on pause goal of %d (ms)", MaxGCPauseMillis);
    }
    return true;
  }
  return false;
}

void Arguments::set_ergonomics_flags() {
  // Parallel GC is not compatible with sharing. If one specifies
  // that they want sharing explicitly, do not set ergonomics flags.
  if (DumpSharedSpaces || RequireSharedSpaces) {
    return;
  }

  if (os::is_server_class_machine()) {
    // If no other collector is requested explicitly,
    // let the VM select the collector based on
    // machine class and automatic selection policy.
    if (!UseSerialGC &&
        !UseConcMarkSweepGC &&
        !UseG1GC &&
        !UseParNewGC &&
        !DumpSharedSpaces &&
        FLAG_IS_DEFAULT(UseParallelGC)) {
      if (should_auto_select_low_pause_collector()) {
        FLAG_SET_ERGO(bool, UseConcMarkSweepGC, true);
      } else {
        FLAG_SET_ERGO(bool, UseParallelGC, true);
      }
      no_shared_spaces();
    }
  }

#ifndef ZERO
#ifdef _LP64
  // Check that UseCompressedOops can be set with the max heap size allocated
  // by ergonomics.
  if (MaxHeapSize <= max_heap_for_compressed_oops()) {
#if !defined(COMPILER1) || defined(TIERED)
    if (FLAG_IS_DEFAULT(UseCompressedOops)) {
      FLAG_SET_ERGO(bool, UseCompressedOops, true);
    }
#endif
#ifdef _WIN64
    if (UseLargePages && UseCompressedOops) {
      // Cannot allocate guard pages for implicit checks in indexed addressing
      // mode, when large pages are specified on windows.
      // This flag could be switched ON if narrow oop base address is set to 0,
      // see code in Universe::initialize_heap().
      Universe::set_narrow_oop_use_implicit_null_checks(false);
    }
#endif //  _WIN64
  } else {
    if (UseCompressedOops && !FLAG_IS_DEFAULT(UseCompressedOops)) {
      warning("Max heap size too large for Compressed Oops");
      FLAG_SET_DEFAULT(UseCompressedOops, false);
    }
  }
  // Also checks that certain machines are slower with compressed oops
  // in vm_version initialization code.
#endif // _LP64
#endif // !ZERO
}

void Arguments::set_parallel_gc_flags() {
  assert(UseParallelGC || UseParallelOldGC, "Error");
  // Enable ParallelOld unless it was explicitly disabled (cmd line or rc file).
  if (FLAG_IS_DEFAULT(UseParallelOldGC)) {
    FLAG_SET_DEFAULT(UseParallelOldGC, true);
  }
  FLAG_SET_DEFAULT(UseParallelGC, true);

  if (UseAdaptiveSizePolicy) {
    // We don't want to limit adaptive heap sizing's freedom to adjust the heap
    // unless the user actually sets these flags.
    if (FLAG_IS_DEFAULT(MinHeapFreeRatio)) {
      FLAG_SET_DEFAULT(MinHeapFreeRatio, 0);
    }
    if (FLAG_IS_DEFAULT(MaxHeapFreeRatio)) {
      FLAG_SET_DEFAULT(MaxHeapFreeRatio, 100);
    }
  }

  // If no heap maximum was requested explicitly, use some reasonable fraction
  // of the physical memory, up to a maximum of 1GB.
  if (UseParallelGC) {
    FLAG_SET_DEFAULT(ParallelGCThreads,
                     Abstract_VM_Version::parallel_worker_threads());

    // If InitialSurvivorRatio or MinSurvivorRatio were not specified, but the
    // SurvivorRatio has been set, reset their default values to SurvivorRatio +
    // 2.  By doing this we make SurvivorRatio also work for Parallel Scavenger.
    // See CR 6362902 for details.
    if (!FLAG_IS_DEFAULT(SurvivorRatio)) {
      if (FLAG_IS_DEFAULT(InitialSurvivorRatio)) {
         FLAG_SET_DEFAULT(InitialSurvivorRatio, SurvivorRatio + 2);
      }
      if (FLAG_IS_DEFAULT(MinSurvivorRatio)) {
        FLAG_SET_DEFAULT(MinSurvivorRatio, SurvivorRatio + 2);
      }
    }

    if (UseParallelOldGC) {
      // Par compact uses lower default values since they are treated as
      // minimums.  These are different defaults because of the different
      // interpretation and are not ergonomically set.
      if (FLAG_IS_DEFAULT(MarkSweepDeadRatio)) {
        FLAG_SET_DEFAULT(MarkSweepDeadRatio, 1);
      }
      if (FLAG_IS_DEFAULT(PermMarkSweepDeadRatio)) {
        FLAG_SET_DEFAULT(PermMarkSweepDeadRatio, 5);
      }
    }
  }
  if (UseNUMA) {
    if (FLAG_IS_DEFAULT(MinHeapDeltaBytes)) {
      FLAG_SET_DEFAULT(MinHeapDeltaBytes, 64*M);
    }
    // For those collectors or operating systems (eg, Windows) that do
    // not support full UseNUMA, we will map to UseNUMAInterleaving for now
    UseNUMAInterleaving = true;
  }
}

void Arguments::set_g1_gc_flags() {
  assert(UseG1GC, "Error");
#ifdef COMPILER1
  FastTLABRefill = false;
#endif
  FLAG_SET_DEFAULT(ParallelGCThreads,
                     Abstract_VM_Version::parallel_worker_threads());
  if (ParallelGCThreads == 0) {
    FLAG_SET_DEFAULT(ParallelGCThreads,
                     Abstract_VM_Version::parallel_worker_threads());
  }
  no_shared_spaces();

  if (FLAG_IS_DEFAULT(MarkStackSize)) {
    FLAG_SET_DEFAULT(MarkStackSize, 128 * TASKQUEUE_SIZE);
  }
  if (PrintGCDetails && Verbose) {
    tty->print_cr("MarkStackSize: %uk  MarkStackSizeMax: %uk",
      MarkStackSize / K, MarkStackSizeMax / K);
    tty->print_cr("ConcGCThreads: %u", ConcGCThreads);
  }

  if (FLAG_IS_DEFAULT(GCTimeRatio) || GCTimeRatio == 0) {
    // In G1, we want the default GC overhead goal to be higher than
    // say in PS. So we set it here to 10%. Otherwise the heap might
    // be expanded more aggressively than we would like it to. In
    // fact, even 10% seems to not be high enough in some cases
    // (especially small GC stress tests that the main thing they do
    // is allocation). We might consider increase it further.
    FLAG_SET_DEFAULT(GCTimeRatio, 9);
  }
}

void Arguments::set_heap_base_min_address() {
  if (FLAG_IS_DEFAULT(HeapBaseMinAddress) && UseG1GC && HeapBaseMinAddress < 1*G) {
    // By default HeapBaseMinAddress is 2G on all platforms except Solaris x86.
    // G1 currently needs a lot of C-heap, so on Solaris we have to give G1
    // some extra space for the C-heap compared to other collectors.
    // Use FLAG_SET_DEFAULT here rather than FLAG_SET_ERGO to make sure that
    // code that checks for default values work correctly.
    FLAG_SET_DEFAULT(HeapBaseMinAddress, 1*G);
  }
}

void Arguments::set_heap_size() {
  if (!FLAG_IS_DEFAULT(DefaultMaxRAMFraction)) {
    // Deprecated flag
    FLAG_SET_CMDLINE(uintx, MaxRAMFraction, DefaultMaxRAMFraction);
  }

  const julong phys_mem =
    FLAG_IS_DEFAULT(MaxRAM) ? MIN2(os::physical_memory(), (julong)MaxRAM)
                            : (julong)MaxRAM;

  // If the maximum heap size has not been set with -Xmx,
  // then set it as fraction of the size of physical memory,
  // respecting the maximum and minimum sizes of the heap.
  if (FLAG_IS_DEFAULT(MaxHeapSize)) {
    julong reasonable_max = phys_mem / MaxRAMFraction;

    if (phys_mem <= MaxHeapSize * MinRAMFraction) {
      // Small physical memory, so use a minimum fraction of it for the heap
      reasonable_max = phys_mem / MinRAMFraction;
    } else {
      // Not-small physical memory, so require a heap at least
      // as large as MaxHeapSize
      reasonable_max = MAX2(reasonable_max, (julong)MaxHeapSize);
    }
    if (!FLAG_IS_DEFAULT(ErgoHeapSizeLimit) && ErgoHeapSizeLimit != 0) {
      // Limit the heap size to ErgoHeapSizeLimit
      reasonable_max = MIN2(reasonable_max, (julong)ErgoHeapSizeLimit);
    }
    if (UseCompressedOops) {
      // Limit the heap size to the maximum possible when using compressed oops
      julong max_coop_heap = (julong)max_heap_for_compressed_oops();
      if (HeapBaseMinAddress + MaxHeapSize < max_coop_heap) {
        // Heap should be above HeapBaseMinAddress to get zero based compressed oops
        // but it should be not less than default MaxHeapSize.
        max_coop_heap -= HeapBaseMinAddress;
      }
      reasonable_max = MIN2(reasonable_max, max_coop_heap);
    }
    reasonable_max = os::allocatable_physical_memory(reasonable_max);

    if (!FLAG_IS_DEFAULT(InitialHeapSize)) {
      // An initial heap size was specified on the command line,
      // so be sure that the maximum size is consistent.  Done
      // after call to allocatable_physical_memory because that
      // method might reduce the allocation size.
      reasonable_max = MAX2(reasonable_max, (julong)InitialHeapSize);
    }

    if (PrintGCDetails && Verbose) {
      // Cannot use gclog_or_tty yet.
      tty->print_cr("  Maximum heap size " SIZE_FORMAT, reasonable_max);
    }
    FLAG_SET_ERGO(uintx, MaxHeapSize, (uintx)reasonable_max);
  }

  // If the initial_heap_size has not been set with InitialHeapSize
  // or -Xms, then set it as fraction of the size of physical memory,
  // respecting the maximum and minimum sizes of the heap.
  if (FLAG_IS_DEFAULT(InitialHeapSize)) {
    julong reasonable_minimum = (julong)(OldSize + NewSize);

    reasonable_minimum = MIN2(reasonable_minimum, (julong)MaxHeapSize);

    reasonable_minimum = os::allocatable_physical_memory(reasonable_minimum);

    julong reasonable_initial = phys_mem / InitialRAMFraction;

    reasonable_initial = MAX2(reasonable_initial, reasonable_minimum);
    reasonable_initial = MIN2(reasonable_initial, (julong)MaxHeapSize);

    reasonable_initial = os::allocatable_physical_memory(reasonable_initial);

    if (PrintGCDetails && Verbose) {
      // Cannot use gclog_or_tty yet.
      tty->print_cr("  Initial heap size " SIZE_FORMAT, (uintx)reasonable_initial);
      tty->print_cr("  Minimum heap size " SIZE_FORMAT, (uintx)reasonable_minimum);
    }
    FLAG_SET_ERGO(uintx, InitialHeapSize, (uintx)reasonable_initial);
    set_min_heap_size((uintx)reasonable_minimum);
  }
}

// This must be called after ergonomics because we want bytecode rewriting
// if the server compiler is used, or if UseSharedSpaces is disabled.
void Arguments::set_bytecode_flags() {
  // Better not attempt to store into a read-only space.
  if (UseSharedSpaces) {
    FLAG_SET_DEFAULT(RewriteBytecodes, false);
    FLAG_SET_DEFAULT(RewriteFrequentPairs, false);
  }

  if (!RewriteBytecodes) {
    FLAG_SET_DEFAULT(RewriteFrequentPairs, false);
  }
}

// Aggressive optimization flags  -XX:+AggressiveOpts
void Arguments::set_aggressive_opts_flags() {
#ifdef COMPILER2
  if (AggressiveOpts || !FLAG_IS_DEFAULT(AutoBoxCacheMax)) {
    // EliminateAutoBox code is broken in C2
    if (FLAG_IS_DEFAULT(EliminateAutoBox)) {
      // FLAG_SET_DEFAULT(EliminateAutoBox, true);
    }
    if (EliminateAutoBox) {
      FLAG_SET_DEFAULT(EliminateAutoBox, false);
    }
    if (FLAG_IS_DEFAULT(AutoBoxCacheMax)) {
      FLAG_SET_DEFAULT(AutoBoxCacheMax, 20000);
    }

    // Feed the cache size setting into the JDK
    char buffer[1024];
    sprintf(buffer, "java.lang.Integer.IntegerCache.high=" INTX_FORMAT, AutoBoxCacheMax);
    add_property(buffer);
  }
  if (AggressiveOpts && FLAG_IS_DEFAULT(BiasedLockingStartupDelay)) {
    FLAG_SET_DEFAULT(BiasedLockingStartupDelay, 500);
  }
#endif

  if (AggressiveOpts) {
// Sample flag setting code
//    if (FLAG_IS_DEFAULT(EliminateZeroing)) {
//      FLAG_SET_DEFAULT(EliminateZeroing, true);
//    }
  }
}

//===========================================================================================================
// Parsing of java.compiler property

void Arguments::process_java_compiler_argument(char* arg) {
  // For backwards compatibility, Djava.compiler=NONE or ""
  // causes us to switch to -Xint mode UNLESS -Xdebug
  // is also specified.
  if (strlen(arg) == 0 || strcasecmp(arg, "NONE") == 0) {
    set_java_compiler(true);    // "-Djava.compiler[=...]" most recently seen.
  }
}

void Arguments::process_java_launcher_argument(const char* launcher, void* extra_info) {
  _sun_java_launcher = strdup(launcher);
  if (strcmp("gamma", _sun_java_launcher) == 0) {
    _created_by_gamma_launcher = true;
  }
}

bool Arguments::created_by_java_launcher() {
  assert(_sun_java_launcher != NULL, "property must have value");
  return strcmp(DEFAULT_JAVA_LAUNCHER, _sun_java_launcher) != 0;
}

bool Arguments::created_by_gamma_launcher() {
  return _created_by_gamma_launcher;
}

//===========================================================================================================
// Parsing of main arguments

bool Arguments::verify_interval(uintx val, uintx min,
                                uintx max, const char* name) {
  // Returns true iff value is in the inclusive interval [min..max]
  // false, otherwise.
  if (val >= min && val <= max) {
    return true;
  }
  jio_fprintf(defaultStream::error_stream(),
              "%s of " UINTX_FORMAT " is invalid; must be between " UINTX_FORMAT
              " and " UINTX_FORMAT "\n",
              name, val, min, max);
  return false;
}

bool Arguments::verify_min_value(intx val, intx min, const char* name) {
  // Returns true if given value is at least specified min threshold
  // false, otherwise.
  if (val >= min ) {
      return true;
  }
  jio_fprintf(defaultStream::error_stream(),
              "%s of " INTX_FORMAT " is invalid; must be at least " INTX_FORMAT "\n",
              name, val, min);
  return false;
}

bool Arguments::verify_percentage(uintx value, const char* name) {
  if (is_percentage(value)) {
    return true;
  }
  jio_fprintf(defaultStream::error_stream(),
              "%s of " UINTX_FORMAT " is invalid; must be between 0 and 100\n",
              name, value);
  return false;
}

static void force_serial_gc() {
  FLAG_SET_DEFAULT(UseSerialGC, true);
  FLAG_SET_DEFAULT(UseParNewGC, false);
  FLAG_SET_DEFAULT(UseConcMarkSweepGC, false);
  FLAG_SET_DEFAULT(CMSIncrementalMode, false);  // special CMS suboption
  FLAG_SET_DEFAULT(UseParallelGC, false);
  FLAG_SET_DEFAULT(UseParallelOldGC, false);
  FLAG_SET_DEFAULT(UseG1GC, false);
}

static bool verify_serial_gc_flags() {
  return (UseSerialGC &&
        !(UseParNewGC || (UseConcMarkSweepGC || CMSIncrementalMode) || UseG1GC ||
          UseParallelGC || UseParallelOldGC));
}

// check if do gclog rotation
// +UseGCLogFileRotation is a must,
// no gc log rotation when log file not supplied or
// NumberOfGCLogFiles is 0
void check_gclog_consistency() {
  if (UseGCLogFileRotation) {
    if ((Arguments::gc_log_filename() == NULL) || (NumberOfGCLogFiles == 0)) {
      jio_fprintf(defaultStream::output_stream(),
                  "To enable GC log rotation, use -Xloggc:<filename> -XX:+UseGCLogFileRotation -XX:NumberOfGCLogFiles=<num_of_files>\n"
                  "where num_of_file > 0\n"
                  "GC log rotation is turned off\n");
      UseGCLogFileRotation = false;
    }
  }

  if (UseGCLogFileRotation && (GCLogFileSize != 0) && (GCLogFileSize < 8*K)) {
    FLAG_SET_CMDLINE(uintx, GCLogFileSize, 8*K);
    jio_fprintf(defaultStream::output_stream(),
                "GCLogFileSize changed to minimum 8K\n");
  }
}

bool Arguments::verify_MinHeapFreeRatio(FormatBuffer<80>& err_msg, uintx min_heap_free_ratio) {
  if (!is_percentage(min_heap_free_ratio)) {
    err_msg.print("MinHeapFreeRatio must have a value between 0 and 100");
    return false;
  }
  if (min_heap_free_ratio > MaxHeapFreeRatio) {
    err_msg.print("MinHeapFreeRatio (" UINTX_FORMAT ") must be less than or "
                  "equal to MaxHeapFreeRatio (" UINTX_FORMAT ")", min_heap_free_ratio,
                  MaxHeapFreeRatio);
    return false;
  }
  return true;
}

bool Arguments::verify_MaxHeapFreeRatio(FormatBuffer<80>& err_msg, uintx max_heap_free_ratio) {
  if (!is_percentage(max_heap_free_ratio)) {
    err_msg.print("MaxHeapFreeRatio must have a value between 0 and 100");
    return false;
  }
  if (max_heap_free_ratio < MinHeapFreeRatio) {
    err_msg.print("MaxHeapFreeRatio (" UINTX_FORMAT ") must be greater than or "
                  "equal to MinHeapFreeRatio (" UINTX_FORMAT ")", max_heap_free_ratio,
                  MinHeapFreeRatio);
    return false;
  }
  return true;
}

// This function is called for -Xloggc:<filename>, it can be used
// to check if a given file name(or string) conforms to the following
// specification:
// A valid string only contains "[A-Z][a-z][0-9].-_%[p|t]"
// %p and %t only allowed once. We only limit usage of filename not path
bool is_filename_valid(const char *file_name) {
  const char* p = file_name;
  char file_sep = os::file_separator()[0];
  const char* cp;
  // skip prefix path
  for (cp = file_name; *cp != '\0'; cp++) {
    if (*cp == '/' || *cp == file_sep) {
      p = cp + 1;
    }
  }

  int count_p = 0;
  int count_t = 0;
  while (*p != '\0') {
    if ((*p >= '0' && *p <= '9') ||
        (*p >= 'A' && *p <= 'Z') ||
        (*p >= 'a' && *p <= 'z') ||
         *p == '-'               ||
         *p == '_'               ||
         *p == '.') {
       p++;
       continue;
    }
    if (*p == '%') {
      if(*(p + 1) == 'p') {
        p += 2;
        count_p ++;
        continue;
      }
      if (*(p + 1) == 't') {
        p += 2;
        count_t ++;
        continue;
      }
    }
    return false;
  }
  return count_p < 2 && count_t < 2;
}

// Check consistency of GC selection
bool Arguments::check_gc_consistency() {
  check_gclog_consistency();
  bool status = true;
  // Ensure that the user has not selected conflicting sets
  // of collectors. [Note: this check is merely a user convenience;
  // collectors over-ride each other so that only a non-conflicting
  // set is selected; however what the user gets is not what they
  // may have expected from the combination they asked for. It's
  // better to reduce user confusion by not allowing them to
  // select conflicting combinations.
  uint i = 0;
  if (UseSerialGC)                       i++;
  if (UseConcMarkSweepGC || UseParNewGC) i++;
  if (UseParallelGC || UseParallelOldGC) i++;
  if (UseG1GC)                           i++;
  if (i > 1) {
    jio_fprintf(defaultStream::error_stream(),
                "Conflicting collector combinations in option list; "
                "please refer to the release notes for the combinations "
                "allowed\n");
    status = false;
  }

  return status;
}

// Check stack pages settings
bool Arguments::check_stack_pages()
{
  bool status = true;
  status = status && verify_min_value(StackYellowPages, 1, "StackYellowPages");
  status = status && verify_min_value(StackRedPages, 1, "StackRedPages");
  // greater stack shadow pages can't generate instruction to bang stack
  status = status && verify_interval(StackShadowPages, 1, 50, "StackShadowPages");
  return status;
}

// Check the consistency of vm_init_args
bool Arguments::check_vm_args_consistency() {
  // Method for adding checks for flag consistency.
  // The intent is to warn the user of all possible conflicts,
  // before returning an error.
  // Note: Needs platform-dependent factoring.
  bool status = true;

#if ( (defined(COMPILER2) && defined(SPARC)))
  // NOTE: The call to VM_Version_init depends on the fact that VM_Version_init
  // on sparc doesn't require generation of a stub as is the case on, e.g.,
  // x86.  Normally, VM_Version_init must be called from init_globals in
  // init.cpp, which is called by the initial java thread *after* arguments
  // have been parsed.  VM_Version_init gets called twice on sparc.
  extern void VM_Version_init();
  VM_Version_init();
  if (!VM_Version::has_v9()) {
    jio_fprintf(defaultStream::error_stream(),
                "V8 Machine detected, Server requires V9\n");
    status = false;
  }
#endif /* COMPILER2 && SPARC */

  // Allow both -XX:-UseStackBanging and -XX:-UseBoundThreads in non-product
  // builds so the cost of stack banging can be measured.
#if (defined(PRODUCT) && defined(SOLARIS))
  if (!UseBoundThreads && !UseStackBanging) {
    jio_fprintf(defaultStream::error_stream(),
                "-UseStackBanging conflicts with -UseBoundThreads\n");

     status = false;
  }
#endif

  if (TLABRefillWasteFraction == 0) {
    jio_fprintf(defaultStream::error_stream(),
                "TLABRefillWasteFraction should be a denominator, "
                "not " SIZE_FORMAT "\n",
                TLABRefillWasteFraction);
    status = false;
  }

  status = status && verify_percentage(AdaptiveSizePolicyWeight,
                              "AdaptiveSizePolicyWeight");
  status = status && verify_percentage(AdaptivePermSizeWeight, "AdaptivePermSizeWeight");
  status = status && verify_percentage(ThresholdTolerance, "ThresholdTolerance");

  {
    // Using "else if" below to avoid printing two error messages if min > max.
    // This will also prevent us from reporting both min>100 and max>100 at the
    // same time, but that is less annoying than printing two identical errors IMHO.
    FormatBuffer<80> err_msg("");
    if (!verify_MinHeapFreeRatio(err_msg, MinHeapFreeRatio)) {
      jio_fprintf(defaultStream::error_stream(), "%s\n", err_msg.buffer());
      status = false;
    } else if (!verify_MaxHeapFreeRatio(err_msg, MaxHeapFreeRatio)) {
      jio_fprintf(defaultStream::error_stream(), "%s\n", err_msg.buffer());
      status = false;
    }
  }
  // Keeping the heap 100% free is hard ;-) so limit it to 99%.
  MinHeapFreeRatio = MIN2(MinHeapFreeRatio, (uintx) 99);

  if (FullGCALot && FLAG_IS_DEFAULT(MarkSweepAlwaysCompactCount)) {
    MarkSweepAlwaysCompactCount = 1;  // Move objects every gc.
  }

  if (UseParallelOldGC && ParallelOldGCSplitALot) {
    // Settings to encourage splitting.
    if (!FLAG_IS_CMDLINE(NewRatio)) {
      FLAG_SET_CMDLINE(intx, NewRatio, 2);
    }
    if (!FLAG_IS_CMDLINE(ScavengeBeforeFullGC)) {
      FLAG_SET_CMDLINE(bool, ScavengeBeforeFullGC, false);
    }
  }

  status = status && verify_percentage(GCHeapFreeLimit, "GCHeapFreeLimit");
  status = status && verify_percentage(GCTimeLimit, "GCTimeLimit");
  if (GCTimeLimit == 100) {
    // Turn off gc-overhead-limit-exceeded checks
    FLAG_SET_DEFAULT(UseGCOverheadLimit, false);
  }

  status = status && verify_percentage(GCHeapFreeLimit, "GCHeapFreeLimit");

  status = status && check_gc_consistency();
  status = status && check_stack_pages();

  if (_has_alloc_profile) {
    if (UseParallelGC || UseParallelOldGC) {
      jio_fprintf(defaultStream::error_stream(),
                  "error:  invalid argument combination.\n"
                  "Allocation profiling (-Xaprof) cannot be used together with "
                  "Parallel GC (-XX:+UseParallelGC or -XX:+UseParallelOldGC).\n");
      status = false;
    }
    if (UseConcMarkSweepGC) {
      jio_fprintf(defaultStream::error_stream(),
                  "error:  invalid argument combination.\n"
                  "Allocation profiling (-Xaprof) cannot be used together with "
                  "the CMS collector (-XX:+UseConcMarkSweepGC).\n");
      status = false;
    }
  }

  if (CMSIncrementalMode) {
    if (!UseConcMarkSweepGC) {
      jio_fprintf(defaultStream::error_stream(),
                  "error:  invalid argument combination.\n"
                  "The CMS collector (-XX:+UseConcMarkSweepGC) must be "
                  "selected in order\nto use CMSIncrementalMode.\n");
      status = false;
    } else {
      status = status && verify_percentage(CMSIncrementalDutyCycle,
                                  "CMSIncrementalDutyCycle");
      status = status && verify_percentage(CMSIncrementalDutyCycleMin,
                                  "CMSIncrementalDutyCycleMin");
      status = status && verify_percentage(CMSIncrementalSafetyFactor,
                                  "CMSIncrementalSafetyFactor");
      status = status && verify_percentage(CMSIncrementalOffset,
                                  "CMSIncrementalOffset");
      status = status && verify_percentage(CMSExpAvgFactor,
                                  "CMSExpAvgFactor");
      // If it was not set on the command line, set
      // CMSInitiatingOccupancyFraction to 1 so icms can initiate cycles early.
      if (CMSInitiatingOccupancyFraction < 0) {
        FLAG_SET_DEFAULT(CMSInitiatingOccupancyFraction, 1);
      }
    }
  }

  // CMS space iteration, which FLSVerifyAllHeapreferences entails,
  // insists that we hold the requisite locks so that the iteration is
  // MT-safe. For the verification at start-up and shut-down, we don't
  // yet have a good way of acquiring and releasing these locks,
  // which are not visible at the CollectedHeap level. We want to
  // be able to acquire these locks and then do the iteration rather
  // than just disable the lock verification. This will be fixed under
  // bug 4788986.
  if (UseConcMarkSweepGC && FLSVerifyAllHeapReferences) {
    if (VerifyDuringStartup) {
      warning("Heap verification at start-up disabled "
              "(due to current incompatibility with FLSVerifyAllHeapReferences)");
      VerifyDuringStartup = false; // Disable verification at start-up
    }

    if (VerifyBeforeExit) {
      warning("Heap verification at shutdown disabled "
              "(due to current incompatibility with FLSVerifyAllHeapReferences)");
      VerifyBeforeExit = false; // Disable verification at shutdown
    }
  }

  // Note: only executed in non-PRODUCT mode
  if (!UseAsyncConcMarkSweepGC &&
      (ExplicitGCInvokesConcurrent ||
       ExplicitGCInvokesConcurrentAndUnloadsClasses)) {
    jio_fprintf(defaultStream::error_stream(),
                "error: +ExplicitGCInvokesConcurrent[AndUnloadsClasses] conflicts"
                " with -UseAsyncConcMarkSweepGC");
    status = false;
  }

  status = status && verify_min_value(ParGCArrayScanChunk, 1, "ParGCArrayScanChunk");

#ifndef SERIALGC
  if (UseG1GC) {
    status = status && verify_percentage(InitiatingHeapOccupancyPercent,
                                         "InitiatingHeapOccupancyPercent");
    status = status && verify_min_value(G1RefProcDrainInterval, 1,
                                        "G1RefProcDrainInterval");
    status = status && verify_min_value((intx)G1ConcMarkStepDurationMillis, 1,
                                        "G1ConcMarkStepDurationMillis");
    status = status && verify_interval(G1ConcRSHotCardLimit, 0, max_jubyte,
                                       "G1ConcRSHotCardLimit");
    status = status && verify_interval(G1ConcRSLogCacheSize, 0, 27,
                                       "G1ConcRSLogCacheSize");
  }
#endif

  status = status && verify_interval(RefDiscoveryPolicy,
                                     ReferenceProcessor::DiscoveryPolicyMin,
                                     ReferenceProcessor::DiscoveryPolicyMax,
                                     "RefDiscoveryPolicy");

  // Limit the lower bound of this flag to 1 as it is used in a division
  // expression.
  status = status && verify_interval(TLABWasteTargetPercent,
                                     1, 100, "TLABWasteTargetPercent");

  status = status && verify_object_alignment();

#ifdef SPARC
  if (UseConcMarkSweepGC || UseG1GC) {
    // Issue a stern warning if the user has explicitly set
    // UseMemSetInBOT (it is known to cause issues), but allow
    // use for experimentation and debugging.
    if (VM_Version::is_sun4v() && UseMemSetInBOT) {
      assert(!FLAG_IS_DEFAULT(UseMemSetInBOT), "Error");
      warning("Experimental flag -XX:+UseMemSetInBOT is known to cause instability"
          " on sun4v; please understand that you are using at your own risk!");
    }
  }
#endif // SPARC

  // check native memory tracking flags
  if (PrintNMTStatistics && MemTracker::tracking_level() == MemTracker::NMT_off) {
    warning("PrintNMTStatistics is disabled, because native memory tracking is not enabled");
    PrintNMTStatistics = false;
  }

#ifdef COMPILER1
  status &= verify_interval(SafepointPollOffset, 0, os::vm_page_size() - BytesPerWord, "SafepointPollOffset");
#endif

  return status;
}

bool Arguments::is_bad_option(const JavaVMOption* option, jboolean ignore,
  const char* option_type) {
  if (ignore) return false;

  const char* spacer = " ";
  if (option_type == NULL) {
    option_type = ++spacer; // Set both to the empty string.
  }

  if (os::obsolete_option(option)) {
    jio_fprintf(defaultStream::error_stream(),
                "Obsolete %s%soption: %s\n", option_type, spacer,
      option->optionString);
    return false;
  } else {
    jio_fprintf(defaultStream::error_stream(),
                "Unrecognized %s%soption: %s\n", option_type, spacer,
      option->optionString);
    return true;
  }
}

static const char* user_assertion_options[] = {
  "-da", "-ea", "-disableassertions", "-enableassertions", 0
};

static const char* system_assertion_options[] = {
  "-dsa", "-esa", "-disablesystemassertions", "-enablesystemassertions", 0
};

// Return true if any of the strings in null-terminated array 'names' matches.
// If tail_allowed is true, then the tail must begin with a colon; otherwise,
// the option must match exactly.
static bool match_option(const JavaVMOption* option, const char** names, const char** tail,
  bool tail_allowed) {
  for (/* empty */; *names != NULL; ++names) {
    if (match_option(option, *names, tail)) {
      if (**tail == '\0' || tail_allowed && **tail == ':') {
        return true;
      }
    }
  }
  return false;
}

bool Arguments::parse_uintx(const char* value,
                            uintx* uintx_arg,
                            uintx min_size) {

  // Check the sign first since atomull() parses only unsigned values.
  bool value_is_positive = !(*value == '-');

  if (value_is_positive) {
    julong n;
    bool good_return = atomull(value, &n);
    if (good_return) {
      bool above_minimum = n >= min_size;
      bool value_is_too_large = n > max_uintx;

      if (above_minimum && !value_is_too_large) {
        *uintx_arg = n;
        return true;
      }
    }
  }
  return false;
}

Arguments::ArgsRange Arguments::parse_memory_size(const char* s,
                                                  julong* long_arg,
                                                  julong min_size) {
  if (!atomull(s, long_arg)) return arg_unreadable;
  return check_memory_size(*long_arg, min_size);
}

// Parse JavaVMInitArgs structure

jint Arguments::parse_vm_init_args(const JavaVMInitArgs* args) {
  // For components of the system classpath.
  SysClassPath scp(Arguments::get_sysclasspath());
  bool scp_assembly_required = false;

  // Save default settings for some mode flags
  Arguments::_AlwaysCompileLoopMethods = AlwaysCompileLoopMethods;
  Arguments::_UseOnStackReplacement    = UseOnStackReplacement;
  Arguments::_ClipInlining             = ClipInlining;
  Arguments::_BackgroundCompilation    = BackgroundCompilation;

  // Setup flags for mixed which is the default
  set_mode_flags(_mixed);

  // Parse JAVA_TOOL_OPTIONS environment variable (if present)
  jint result = parse_java_tool_options_environment_variable(&scp, &scp_assembly_required);
  if (result != JNI_OK) {
    return result;
  }

  // Parse JavaVMInitArgs structure passed in
  result = parse_each_vm_init_arg(args, &scp, &scp_assembly_required, COMMAND_LINE);
  if (result != JNI_OK) {
    return result;
  }

  if (AggressiveOpts) {
    // Insert alt-rt.jar between user-specified bootclasspath
    // prefix and the default bootclasspath.  os::set_boot_path()
    // uses meta_index_dir as the default bootclasspath directory.
    const char* altclasses_jar = "alt-rt.jar";
    size_t altclasses_path_len = strlen(get_meta_index_dir()) + 1 +
                                 strlen(altclasses_jar);
    char* altclasses_path = NEW_C_HEAP_ARRAY(char, altclasses_path_len, mtInternal);
    strcpy(altclasses_path, get_meta_index_dir());
    strcat(altclasses_path, altclasses_jar);
    scp.add_suffix_to_prefix(altclasses_path);
    scp_assembly_required = true;
    FREE_C_HEAP_ARRAY(char, altclasses_path, mtInternal);
  }

  // Parse _JAVA_OPTIONS environment variable (if present) (mimics classic VM)
  result = parse_java_options_environment_variable(&scp, &scp_assembly_required);
  if (result != JNI_OK) {
    return result;
  }

  // Do final processing now that all arguments have been parsed
  result = finalize_vm_init_args(&scp, scp_assembly_required);
  if (result != JNI_OK) {
    return result;
  }

  return JNI_OK;
}

jint Arguments::parse_each_vm_init_arg(const JavaVMInitArgs* args,
                                       SysClassPath* scp_p,
                                       bool* scp_assembly_required_p,
                                       FlagValueOrigin origin) {
  // Remaining part of option string
  const char* tail;

  // iterate over arguments
  for (int index = 0; index < args->nOptions; index++) {
    bool is_absolute_path = false;  // for -agentpath vs -agentlib

    const JavaVMOption* option = args->options + index;

    if (!match_option(option, "-Djava.class.path", &tail) &&
        !match_option(option, "-Dsun.java.command", &tail) &&
        !match_option(option, "-Dsun.java.launcher", &tail)) {

        // add all jvm options to the jvm_args string. This string
        // is used later to set the java.vm.args PerfData string constant.
        // the -Djava.class.path and the -Dsun.java.command options are
        // omitted from jvm_args string as each have their own PerfData
        // string constant object.
        build_jvm_args(option->optionString);
    }

    // -verbose:[class/gc/jni]
    if (match_option(option, "-verbose", &tail)) {
      if (!strcmp(tail, ":class") || !strcmp(tail, "")) {
        FLAG_SET_CMDLINE(bool, TraceClassLoading, true);
        FLAG_SET_CMDLINE(bool, TraceClassUnloading, true);
      } else if (!strcmp(tail, ":gc")) {
        FLAG_SET_CMDLINE(bool, PrintGC, true);
      } else if (!strcmp(tail, ":jni")) {
        FLAG_SET_CMDLINE(bool, PrintJNIResolving, true);
      }
    // -da / -ea / -disableassertions / -enableassertions
    // These accept an optional class/package name separated by a colon, e.g.,
    // -da:java.lang.Thread.
    } else if (match_option(option, user_assertion_options, &tail, true)) {
      bool enable = option->optionString[1] == 'e';     // char after '-' is 'e'
      if (*tail == '\0') {
        JavaAssertions::setUserClassDefault(enable);
      } else {
        assert(*tail == ':', "bogus match by match_option()");
        JavaAssertions::addOption(tail + 1, enable);
      }
    // -dsa / -esa / -disablesystemassertions / -enablesystemassertions
    } else if (match_option(option, system_assertion_options, &tail, false)) {
      bool enable = option->optionString[1] == 'e';     // char after '-' is 'e'
      JavaAssertions::setSystemClassDefault(enable);
    // -bootclasspath:
    } else if (match_option(option, "-Xbootclasspath:", &tail)) {
      scp_p->reset_path(tail);
      *scp_assembly_required_p = true;
    // -bootclasspath/a:
    } else if (match_option(option, "-Xbootclasspath/a:", &tail)) {
      scp_p->add_suffix(tail);
      *scp_assembly_required_p = true;
    // -bootclasspath/p:
    } else if (match_option(option, "-Xbootclasspath/p:", &tail)) {
      scp_p->add_prefix(tail);
      *scp_assembly_required_p = true;
    // -Xrun
    } else if (match_option(option, "-Xrun", &tail)) {
      if (tail != NULL) {
        const char* pos = strchr(tail, ':');
        size_t len = (pos == NULL) ? strlen(tail) : pos - tail;
        char* name = (char*)memcpy(NEW_C_HEAP_ARRAY(char, len + 1, mtInternal), tail, len);
        name[len] = '\0';

        char *options = NULL;
        if(pos != NULL) {
          size_t len2 = strlen(pos+1) + 1; // options start after ':'.  Final zero must be copied.
          options = (char*)memcpy(NEW_C_HEAP_ARRAY(char, len2, mtInternal), pos+1, len2);
        }
        add_init_library(name, options);
      }
    // -agentlib and -agentpath
    } else if (match_option(option, "-agentlib:", &tail) ||
          (is_absolute_path = match_option(option, "-agentpath:", &tail))) {
      if(tail != NULL) {
        const char* pos = strchr(tail, '=');
        size_t len = (pos == NULL) ? strlen(tail) : pos - tail;
        char* name = strncpy(NEW_C_HEAP_ARRAY(char, len + 1, mtInternal), tail, len);
        name[len] = '\0';

        char *options = NULL;
        if(pos != NULL) {
          options = strcpy(NEW_C_HEAP_ARRAY(char, strlen(pos + 1) + 1, mtInternal), pos + 1);
        }
        add_init_agent(name, options, is_absolute_path);

      }
    // -javaagent
    } else if (match_option(option, "-javaagent:", &tail)) {
      if(tail != NULL) {
        char *options = strcpy(NEW_C_HEAP_ARRAY(char, strlen(tail) + 1, mtInternal), tail);
        add_init_agent("instrument", options, false);
      }
    // -Xnoclassgc
    } else if (match_option(option, "-Xnoclassgc", &tail)) {
      FLAG_SET_CMDLINE(bool, ClassUnloading, false);
    // -Xincgc: i-CMS
    } else if (match_option(option, "-Xincgc", &tail)) {
      FLAG_SET_CMDLINE(bool, UseConcMarkSweepGC, true);
      FLAG_SET_CMDLINE(bool, CMSIncrementalMode, true);
    // -Xnoincgc: no i-CMS
    } else if (match_option(option, "-Xnoincgc", &tail)) {
      FLAG_SET_CMDLINE(bool, UseConcMarkSweepGC, false);
      FLAG_SET_CMDLINE(bool, CMSIncrementalMode, false);
    // -Xconcgc
    } else if (match_option(option, "-Xconcgc", &tail)) {
      FLAG_SET_CMDLINE(bool, UseConcMarkSweepGC, true);
    // -Xnoconcgc
    } else if (match_option(option, "-Xnoconcgc", &tail)) {
      FLAG_SET_CMDLINE(bool, UseConcMarkSweepGC, false);
    // -Xbatch
    } else if (match_option(option, "-Xbatch", &tail)) {
      FLAG_SET_CMDLINE(bool, BackgroundCompilation, false);
    // -Xmn for compatibility with other JVM vendors
    } else if (match_option(option, "-Xmn", &tail)) {
      julong long_initial_eden_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_initial_eden_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid initial eden size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, MaxNewSize, (uintx)long_initial_eden_size);
      FLAG_SET_CMDLINE(uintx, NewSize, (uintx)long_initial_eden_size);
    // -Xms
    } else if (match_option(option, "-Xms", &tail)) {
      julong long_initial_heap_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_initial_heap_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid initial heap size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, InitialHeapSize, (uintx)long_initial_heap_size);
      // Currently the minimum size and the initial heap sizes are the same.
      set_min_heap_size(InitialHeapSize);
    // -Xmx
    } else if (match_option(option, "-Xmx", &tail)) {
      julong long_max_heap_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_max_heap_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum heap size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, MaxHeapSize, (uintx)long_max_heap_size);
    // Xmaxf
    } else if (match_option(option, "-Xmaxf", &tail)) {
      int maxf = (int)(atof(tail) * 100);
      if (maxf < 0 || maxf > 100) {
        jio_fprintf(defaultStream::error_stream(),
                    "Bad max heap free percentage size: %s\n",
                    option->optionString);
        return JNI_EINVAL;
      } else {
        FLAG_SET_CMDLINE(uintx, MaxHeapFreeRatio, maxf);
      }
    // Xminf
    } else if (match_option(option, "-Xminf", &tail)) {
      int minf = (int)(atof(tail) * 100);
      if (minf < 0 || minf > 100) {
        jio_fprintf(defaultStream::error_stream(),
                    "Bad min heap free percentage size: %s\n",
                    option->optionString);
        return JNI_EINVAL;
      } else {
        FLAG_SET_CMDLINE(uintx, MinHeapFreeRatio, minf);
      }
    // -Xss
    } else if (match_option(option, "-Xss", &tail)) {
      julong long_ThreadStackSize = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_ThreadStackSize, 1000);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid thread stack size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      // Internally track ThreadStackSize in units of 1024 bytes.
      FLAG_SET_CMDLINE(intx, ThreadStackSize,
                              round_to((int)long_ThreadStackSize, K) / K);
    // -Xoss
    } else if (match_option(option, "-Xoss", &tail)) {
          // HotSpot does not have separate native and Java stacks, ignore silently for compatibility
    // -Xmaxjitcodesize
    } else if (match_option(option, "-Xmaxjitcodesize", &tail) ||
               match_option(option, "-XX:ReservedCodeCacheSize=", &tail)) {
      julong long_ReservedCodeCacheSize = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_ReservedCodeCacheSize,
                                            (size_t)InitialCodeCacheSize);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum code cache size: %s. Should be greater than or equal to InitialCodeCacheSize=%dK\n",
                    option->optionString, InitialCodeCacheSize/K);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, ReservedCodeCacheSize, (uintx)long_ReservedCodeCacheSize);
    // -green
    } else if (match_option(option, "-green", &tail)) {
      jio_fprintf(defaultStream::error_stream(),
                  "Green threads support not available\n");
          return JNI_EINVAL;
    // -native
    } else if (match_option(option, "-native", &tail)) {
          // HotSpot always uses native threads, ignore silently for compatibility
    // -Xsqnopause
    } else if (match_option(option, "-Xsqnopause", &tail)) {
          // EVM option, ignore silently for compatibility
    // -Xrs
    } else if (match_option(option, "-Xrs", &tail)) {
          // Classic/EVM option, new functionality
      FLAG_SET_CMDLINE(bool, ReduceSignalUsage, true);
    } else if (match_option(option, "-Xusealtsigs", &tail)) {
          // change default internal VM signals used - lower case for back compat
      FLAG_SET_CMDLINE(bool, UseAltSigs, true);
    // -Xoptimize
    } else if (match_option(option, "-Xoptimize", &tail)) {
          // EVM option, ignore silently for compatibility
    // -Xprof
    } else if (match_option(option, "-Xprof", &tail)) {
      _has_profile = true;
    // -Xaprof
    } else if (match_option(option, "-Xaprof", &tail)) {
      _has_alloc_profile = true;
    // -Xconcurrentio
    } else if (match_option(option, "-Xconcurrentio", &tail)) {
      FLAG_SET_CMDLINE(bool, UseLWPSynchronization, true);
      FLAG_SET_CMDLINE(bool, BackgroundCompilation, false);
      FLAG_SET_CMDLINE(intx, DeferThrSuspendLoopCount, 1);
      FLAG_SET_CMDLINE(bool, UseTLAB, false);
      FLAG_SET_CMDLINE(uintx, NewSizeThreadIncrease, 16 * K);  // 20Kb per thread added to new generation

      // -Xinternalversion
    } else if (match_option(option, "-Xinternalversion", &tail)) {
      jio_fprintf(defaultStream::output_stream(), "%s\n",
                  VM_Version::internal_vm_info_string());
      vm_exit(0);
#ifndef PRODUCT
    // -Xprintflags
    } else if (match_option(option, "-Xprintflags", &tail)) {
      CommandLineFlags::printFlags(tty, false);
      vm_exit(0);
#endif
    // -D
    } else if (match_option(option, "-D", &tail)) {
      if (!add_property(tail)) {
        return JNI_ENOMEM;
      }
      // Out of the box management support
      if (match_option(option, "-Dcom.sun.management", &tail)) {
        FLAG_SET_CMDLINE(bool, ManagementServer, true);
      }
    // -Xint
    } else if (match_option(option, "-Xint", &tail)) {
          set_mode_flags(_int);
    // -Xmixed
    } else if (match_option(option, "-Xmixed", &tail)) {
          set_mode_flags(_mixed);
    // -Xcomp
    } else if (match_option(option, "-Xcomp", &tail)) {
      // for testing the compiler; turn off all flags that inhibit compilation
          set_mode_flags(_comp);

    // -Xshare:dump
    } else if (match_option(option, "-Xshare:dump", &tail)) {
#ifdef TIERED
      FLAG_SET_CMDLINE(bool, DumpSharedSpaces, true);
      set_mode_flags(_int);     // Prevent compilation, which creates objects
#elif defined(COMPILER2)
      vm_exit_during_initialization(
          "Dumping a shared archive is not supported on the Server JVM.", NULL);
#else
      FLAG_SET_CMDLINE(bool, DumpSharedSpaces, true);
      set_mode_flags(_int);     // Prevent compilation, which creates objects
#endif
    // -Xshare:on
    } else if (match_option(option, "-Xshare:on", &tail)) {
      FLAG_SET_CMDLINE(bool, UseSharedSpaces, true);
      FLAG_SET_CMDLINE(bool, RequireSharedSpaces, true);
    // -Xshare:auto
    } else if (match_option(option, "-Xshare:auto", &tail)) {
      FLAG_SET_CMDLINE(bool, UseSharedSpaces, true);
      FLAG_SET_CMDLINE(bool, RequireSharedSpaces, false);
    // -Xshare:off
    } else if (match_option(option, "-Xshare:off", &tail)) {
      FLAG_SET_CMDLINE(bool, UseSharedSpaces, false);
      FLAG_SET_CMDLINE(bool, RequireSharedSpaces, false);

    // -Xverify
    } else if (match_option(option, "-Xverify", &tail)) {
      if (strcmp(tail, ":all") == 0 || strcmp(tail, "") == 0) {
        FLAG_SET_CMDLINE(bool, BytecodeVerificationLocal, true);
        FLAG_SET_CMDLINE(bool, BytecodeVerificationRemote, true);
      } else if (strcmp(tail, ":remote") == 0) {
        FLAG_SET_CMDLINE(bool, BytecodeVerificationLocal, false);
        FLAG_SET_CMDLINE(bool, BytecodeVerificationRemote, true);
      } else if (strcmp(tail, ":none") == 0) {
        FLAG_SET_CMDLINE(bool, BytecodeVerificationLocal, false);
        FLAG_SET_CMDLINE(bool, BytecodeVerificationRemote, false);
      } else if (is_bad_option(option, args->ignoreUnrecognized, "verification")) {
        return JNI_EINVAL;
      }
    // -Xdebug
    } else if (match_option(option, "-Xdebug", &tail)) {
      // note this flag has been used, then ignore
      set_xdebug_mode(true);
    // -Xnoagent
    } else if (match_option(option, "-Xnoagent", &tail)) {
      // For compatibility with classic. HotSpot refuses to load the old style agent.dll.
    } else if (match_option(option, "-Xboundthreads", &tail)) {
      // Bind user level threads to kernel threads (Solaris only)
      FLAG_SET_CMDLINE(bool, UseBoundThreads, true);
    } else if (match_option(option, "-Xloggc:", &tail)) {
      // Redirect GC output to the file. -Xloggc:<filename>
      // ostream_init_log(), when called will use this filename
      // to initialize a fileStream.
      _gc_log_filename = strdup(tail);
     if (!is_filename_valid(_gc_log_filename)) {
       jio_fprintf(defaultStream::output_stream(),
                  "Invalid file name for use with -Xloggc: Filename can only contain the "
                  "characters [A-Z][a-z][0-9]-_.%%[p|t] but it has been %s\n"
                  "Note %%p or %%t can only be used once\n", _gc_log_filename);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(bool, PrintGC, true);
      FLAG_SET_CMDLINE(bool, PrintGCTimeStamps, true);

    // JNI hooks
    } else if (match_option(option, "-Xcheck", &tail)) {
      if (!strcmp(tail, ":jni")) {
        CheckJNICalls = true;
      } else if (is_bad_option(option, args->ignoreUnrecognized,
                                     "check")) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "vfprintf", &tail)) {
      _vfprintf_hook = CAST_TO_FN_PTR(vfprintf_hook_t, option->extraInfo);
    } else if (match_option(option, "exit", &tail)) {
      _exit_hook = CAST_TO_FN_PTR(exit_hook_t, option->extraInfo);
    } else if (match_option(option, "abort", &tail)) {
      _abort_hook = CAST_TO_FN_PTR(abort_hook_t, option->extraInfo);
    // -XX:+AggressiveHeap
    } else if (match_option(option, "-XX:+AggressiveHeap", &tail)) {

      // This option inspects the machine and attempts to set various
      // parameters to be optimal for long-running, memory allocation
      // intensive jobs.  It is intended for machines with large
      // amounts of cpu and memory.

      // initHeapSize is needed since _initial_heap_size is 4 bytes on a 32 bit
      // VM, but we may not be able to represent the total physical memory
      // available (like having 8gb of memory on a box but using a 32bit VM).
      // Thus, we need to make sure we're using a julong for intermediate
      // calculations.
      julong initHeapSize;
      julong total_memory = os::physical_memory();

      if (total_memory < (julong)256*M) {
        jio_fprintf(defaultStream::error_stream(),
                    "You need at least 256mb of memory to use -XX:+AggressiveHeap\n");
        vm_exit(1);
      }

      // The heap size is half of available memory, or (at most)
      // all of possible memory less 160mb (leaving room for the OS
      // when using ISM).  This is the maximum; because adaptive sizing
      // is turned on below, the actual space used may be smaller.

      initHeapSize = MIN2(total_memory / (julong)2,
                          total_memory - (julong)160*M);

      // Make sure that if we have a lot of memory we cap the 32 bit
      // process space.  The 64bit VM version of this function is a nop.
      initHeapSize = os::allocatable_physical_memory(initHeapSize);

      // The perm gen is separate but contiguous with the
      // object heap (and is reserved with it) so subtract it
      // from the heap size.
      if (initHeapSize > MaxPermSize) {
        initHeapSize = initHeapSize - MaxPermSize;
      } else {
        warning("AggressiveHeap and MaxPermSize values may conflict");
      }

      if (FLAG_IS_DEFAULT(MaxHeapSize)) {
         FLAG_SET_CMDLINE(uintx, MaxHeapSize, initHeapSize);
         FLAG_SET_CMDLINE(uintx, InitialHeapSize, initHeapSize);
         // Currently the minimum size and the initial heap sizes are the same.
         set_min_heap_size(initHeapSize);
      }
      if (FLAG_IS_DEFAULT(NewSize)) {
         // Make the young generation 3/8ths of the total heap.
         FLAG_SET_CMDLINE(uintx, NewSize,
                                ((julong)MaxHeapSize / (julong)8) * (julong)3);
         FLAG_SET_CMDLINE(uintx, MaxNewSize, NewSize);
      }

#ifndef _ALLBSD_SOURCE  // UseLargePages is not yet supported on BSD.
      FLAG_SET_DEFAULT(UseLargePages, true);
#endif

      // Increase some data structure sizes for efficiency
      FLAG_SET_CMDLINE(uintx, BaseFootPrintEstimate, MaxHeapSize);
      FLAG_SET_CMDLINE(bool, ResizeTLAB, false);
      FLAG_SET_CMDLINE(uintx, TLABSize, 256*K);

      // See the OldPLABSize comment below, but replace 'after promotion'
      // with 'after copying'.  YoungPLABSize is the size of the survivor
      // space per-gc-thread buffers.  The default is 4kw.
      FLAG_SET_CMDLINE(uintx, YoungPLABSize, 256*K);      // Note: this is in words

      // OldPLABSize is the size of the buffers in the old gen that
      // UseParallelGC uses to promote live data that doesn't fit in the
      // survivor spaces.  At any given time, there's one for each gc thread.
      // The default size is 1kw. These buffers are rarely used, since the
      // survivor spaces are usually big enough.  For specjbb, however, there
      // are occasions when there's lots of live data in the young gen
      // and we end up promoting some of it.  We don't have a definite
      // explanation for why bumping OldPLABSize helps, but the theory
      // is that a bigger PLAB results in retaining something like the
      // original allocation order after promotion, which improves mutator
      // locality.  A minor effect may be that larger PLABs reduce the
      // number of PLAB allocation events during gc.  The value of 8kw
      // was arrived at by experimenting with specjbb.
      FLAG_SET_CMDLINE(uintx, OldPLABSize, 8*K);  // Note: this is in words

      // Enable parallel GC and adaptive generation sizing
      FLAG_SET_CMDLINE(bool, UseParallelGC, true);
      FLAG_SET_DEFAULT(ParallelGCThreads,
                       Abstract_VM_Version::parallel_worker_threads());

      // Encourage steady state memory management
      FLAG_SET_CMDLINE(uintx, ThresholdTolerance, 100);

      // This appears to improve mutator locality
      FLAG_SET_CMDLINE(bool, ScavengeBeforeFullGC, false);

      // Get around early Solaris scheduling bug
      // (affinity vs other jobs on system)
      // but disallow DR and offlining (5008695).
      FLAG_SET_CMDLINE(bool, BindGCTaskThreadsToCPUs, true);

    } else if (match_option(option, "-XX:+NeverTenure", &tail)) {
      // The last option must always win.
      FLAG_SET_CMDLINE(bool, AlwaysTenure, false);
      FLAG_SET_CMDLINE(bool, NeverTenure, true);
    } else if (match_option(option, "-XX:+AlwaysTenure", &tail)) {
      // The last option must always win.
      FLAG_SET_CMDLINE(bool, NeverTenure, false);
      FLAG_SET_CMDLINE(bool, AlwaysTenure, true);
    } else if (match_option(option, "-XX:+CMSPermGenSweepingEnabled", &tail) ||
               match_option(option, "-XX:-CMSPermGenSweepingEnabled", &tail)) {
      jio_fprintf(defaultStream::error_stream(),
        "Please use CMSClassUnloadingEnabled in place of "
        "CMSPermGenSweepingEnabled in the future\n");
    } else if (match_option(option, "-XX:+UseGCTimeLimit", &tail)) {
      FLAG_SET_CMDLINE(bool, UseGCOverheadLimit, true);
      jio_fprintf(defaultStream::error_stream(),
        "Please use -XX:+UseGCOverheadLimit in place of "
        "-XX:+UseGCTimeLimit in the future\n");
    } else if (match_option(option, "-XX:-UseGCTimeLimit", &tail)) {
      FLAG_SET_CMDLINE(bool, UseGCOverheadLimit, false);
      jio_fprintf(defaultStream::error_stream(),
        "Please use -XX:-UseGCOverheadLimit in place of "
        "-XX:-UseGCTimeLimit in the future\n");
    // The TLE options are for compatibility with 1.3 and will be
    // removed without notice in a future release.  These options
    // are not to be documented.
    } else if (match_option(option, "-XX:MaxTLERatio=", &tail)) {
      // No longer used.
    } else if (match_option(option, "-XX:+ResizeTLE", &tail)) {
      FLAG_SET_CMDLINE(bool, ResizeTLAB, true);
    } else if (match_option(option, "-XX:-ResizeTLE", &tail)) {
      FLAG_SET_CMDLINE(bool, ResizeTLAB, false);
    } else if (match_option(option, "-XX:+PrintTLE", &tail)) {
      FLAG_SET_CMDLINE(bool, PrintTLAB, true);
    } else if (match_option(option, "-XX:-PrintTLE", &tail)) {
      FLAG_SET_CMDLINE(bool, PrintTLAB, false);
    } else if (match_option(option, "-XX:TLEFragmentationRatio=", &tail)) {
      // No longer used.
    } else if (match_option(option, "-XX:TLESize=", &tail)) {
      julong long_tlab_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_tlab_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid TLAB size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, TLABSize, long_tlab_size);
    } else if (match_option(option, "-XX:TLEThreadRatio=", &tail)) {
      // No longer used.
    } else if (match_option(option, "-XX:+UseTLE", &tail)) {
      FLAG_SET_CMDLINE(bool, UseTLAB, true);
    } else if (match_option(option, "-XX:-UseTLE", &tail)) {
      FLAG_SET_CMDLINE(bool, UseTLAB, false);
SOLARIS_ONLY(
    } else if (match_option(option, "-XX:+UsePermISM", &tail)) {
      warning("-XX:+UsePermISM is obsolete.");
      FLAG_SET_CMDLINE(bool, UseISM, true);
    } else if (match_option(option, "-XX:-UsePermISM", &tail)) {
      FLAG_SET_CMDLINE(bool, UseISM, false);
)
    } else if (match_option(option, "-XX:+DisplayVMOutputToStderr", &tail)) {
      FLAG_SET_CMDLINE(bool, DisplayVMOutputToStdout, false);
      FLAG_SET_CMDLINE(bool, DisplayVMOutputToStderr, true);
    } else if (match_option(option, "-XX:+DisplayVMOutputToStdout", &tail)) {
      FLAG_SET_CMDLINE(bool, DisplayVMOutputToStderr, false);
      FLAG_SET_CMDLINE(bool, DisplayVMOutputToStdout, true);
    } else if (match_option(option, "-XX:+ExtendedDTraceProbes", &tail)) {
#if defined(DTRACE_ENABLED)
      FLAG_SET_CMDLINE(bool, ExtendedDTraceProbes, true);
      FLAG_SET_CMDLINE(bool, DTraceMethodProbes, true);
      FLAG_SET_CMDLINE(bool, DTraceAllocProbes, true);
      FLAG_SET_CMDLINE(bool, DTraceMonitorProbes, true);
#else // defined(DTRACE_ENABLED)
      jio_fprintf(defaultStream::error_stream(),
                  "ExtendedDTraceProbes flag is not applicable for this configuration\n");
      return JNI_EINVAL;
#endif // defined(DTRACE_ENABLED)
#ifdef ASSERT
    } else if (match_option(option, "-XX:+FullGCALot", &tail)) {
      FLAG_SET_CMDLINE(bool, FullGCALot, true);
      // disable scavenge before parallel mark-compact
      FLAG_SET_CMDLINE(bool, ScavengeBeforeFullGC, false);
#endif
    } else if (match_option(option, "-XX:CMSParPromoteBlocksToClaim=", &tail)) {
      julong cms_blocks_to_claim = (julong)atol(tail);
      FLAG_SET_CMDLINE(uintx, CMSParPromoteBlocksToClaim, cms_blocks_to_claim);
      jio_fprintf(defaultStream::error_stream(),
        "Please use -XX:OldPLABSize in place of "
        "-XX:CMSParPromoteBlocksToClaim in the future\n");
    } else if (match_option(option, "-XX:ParCMSPromoteBlocksToClaim=", &tail)) {
      julong cms_blocks_to_claim = (julong)atol(tail);
      FLAG_SET_CMDLINE(uintx, CMSParPromoteBlocksToClaim, cms_blocks_to_claim);
      jio_fprintf(defaultStream::error_stream(),
        "Please use -XX:OldPLABSize in place of "
        "-XX:ParCMSPromoteBlocksToClaim in the future\n");
    } else if (match_option(option, "-XX:ParallelGCOldGenAllocBufferSize=", &tail)) {
      julong old_plab_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &old_plab_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid old PLAB size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, OldPLABSize, old_plab_size);
      jio_fprintf(defaultStream::error_stream(),
                  "Please use -XX:OldPLABSize in place of "
                  "-XX:ParallelGCOldGenAllocBufferSize in the future\n");
    } else if (match_option(option, "-XX:ParallelGCToSpaceAllocBufferSize=", &tail)) {
      julong young_plab_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &young_plab_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid young PLAB size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, YoungPLABSize, young_plab_size);
      jio_fprintf(defaultStream::error_stream(),
                  "Please use -XX:YoungPLABSize in place of "
                  "-XX:ParallelGCToSpaceAllocBufferSize in the future\n");
    } else if (match_option(option, "-XX:CMSMarkStackSize=", &tail) ||
               match_option(option, "-XX:G1MarkStackSize=", &tail)) {
      julong stack_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &stack_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid mark stack size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, MarkStackSize, stack_size);
    } else if (match_option(option, "-XX:CMSMarkStackSizeMax=", &tail)) {
      julong max_stack_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &max_stack_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum mark stack size: %s\n",
                    option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, MarkStackSizeMax, max_stack_size);
    } else if (match_option(option, "-XX:ParallelMarkingThreads=", &tail) ||
               match_option(option, "-XX:ParallelCMSThreads=", &tail)) {
      uintx conc_threads = 0;
      if (!parse_uintx(tail, &conc_threads, 1)) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid concurrent threads: %s\n", option->optionString);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, ConcGCThreads, conc_threads);
    } else if (match_option(option, "-XX:MaxDirectMemorySize=", &tail)) {
      julong max_direct_memory_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &max_direct_memory_size, 0);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum direct memory size: %s\n",
                    option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      FLAG_SET_CMDLINE(uintx, MaxDirectMemorySize, max_direct_memory_size);
    } else if (match_option(option, "-XX:", &tail)) { // -XX:xxxx
      // Skip -XX:Flags= since that case has already been handled
      if (strncmp(tail, "Flags=", strlen("Flags=")) != 0) {
        if (!process_argument(tail, args->ignoreUnrecognized, origin)) {
          return JNI_EINVAL;
        }
      }
    // Unknown option
    } else if (is_bad_option(option, args->ignoreUnrecognized)) {
      return JNI_ERR;
    }
  }

  // Change the default value for flags  which have different default values
  // when working with older JDKs.
  if (JDK_Version::current().compare_major(6) <= 0 &&
      FLAG_IS_DEFAULT(UseVMInterruptibleIO)) {
    FLAG_SET_DEFAULT(UseVMInterruptibleIO, true);
  }
#ifdef LINUX
 if (JDK_Version::current().compare_major(6) <= 0 &&
      FLAG_IS_DEFAULT(UseLinuxPosixThreadCPUClocks)) {
    FLAG_SET_DEFAULT(UseLinuxPosixThreadCPUClocks, false);
  }
#endif // LINUX
  return JNI_OK;
}

jint Arguments::finalize_vm_init_args(SysClassPath* scp_p, bool scp_assembly_required) {
  // This must be done after all -D arguments have been processed.
  scp_p->expand_endorsed();

  if (scp_assembly_required || scp_p->get_endorsed() != NULL) {
    // Assemble the bootclasspath elements into the final path.
    Arguments::set_sysclasspath(scp_p->combined_path());
  }

  // This must be done after all arguments have been processed.
  // java_compiler() true means set to "NONE" or empty.
  if (java_compiler() && !xdebug_mode()) {
    // For backwards compatibility, we switch to interpreted mode if
    // -Djava.compiler="NONE" or "" is specified AND "-Xdebug" was
    // not specified.
    set_mode_flags(_int);
  }
  if (CompileThreshold == 0) {
    set_mode_flags(_int);
  }

#ifndef COMPILER2
  // Don't degrade server performance for footprint
  if (FLAG_IS_DEFAULT(UseLargePages) &&
      MaxHeapSize < LargePageHeapSizeThreshold) {
    // No need for large granularity pages w/small heaps.
    // Note that large pages are enabled/disabled for both the
    // Java heap and the code cache.
    FLAG_SET_DEFAULT(UseLargePages, false);
    SOLARIS_ONLY(FLAG_SET_DEFAULT(UseMPSS, false));
    SOLARIS_ONLY(FLAG_SET_DEFAULT(UseISM, false));
  }

  // Tiered compilation is undefined with C1.
  TieredCompilation = false;
#else
  if (!FLAG_IS_DEFAULT(OptoLoopAlignment) && FLAG_IS_DEFAULT(MaxLoopPad)) {
    FLAG_SET_DEFAULT(MaxLoopPad, OptoLoopAlignment-1);
  }
#endif

  // If we are running in a headless jre, force java.awt.headless property
  // to be true unless the property has already been set.
  // Also allow the OS environment variable JAVA_AWT_HEADLESS to set headless state.
  if (os::is_headless_jre()) {
    const char* headless = Arguments::get_property("java.awt.headless");
    if (headless == NULL) {
      char envbuffer[128];
      if (!os::getenv("JAVA_AWT_HEADLESS", envbuffer, sizeof(envbuffer))) {
        if (!add_property("java.awt.headless=true")) {
          return JNI_ENOMEM;
        }
      } else {
        char buffer[256];
        strcpy(buffer, "java.awt.headless=");
        strcat(buffer, envbuffer);
        if (!add_property(buffer)) {
          return JNI_ENOMEM;
        }
      }
    }
  }

  if (!check_vm_args_consistency()) {
    return JNI_ERR;
  }

  return JNI_OK;
}

jint Arguments::parse_java_options_environment_variable(SysClassPath* scp_p, bool* scp_assembly_required_p) {
  return parse_options_environment_variable("_JAVA_OPTIONS", scp_p,
                                            scp_assembly_required_p);
}

jint Arguments::parse_java_tool_options_environment_variable(SysClassPath* scp_p, bool* scp_assembly_required_p) {
  return parse_options_environment_variable("JAVA_TOOL_OPTIONS", scp_p,
                                            scp_assembly_required_p);
}

jint Arguments::parse_options_environment_variable(const char* name, SysClassPath* scp_p, bool* scp_assembly_required_p) {
  const int N_MAX_OPTIONS = 64;
  const int OPTION_BUFFER_SIZE = 1024;
  char buffer[OPTION_BUFFER_SIZE];

  // The variable will be ignored if it exceeds the length of the buffer.
  // Don't check this variable if user has special privileges
  // (e.g. unix su command).
  if (os::getenv(name, buffer, sizeof(buffer)) &&
      !os::have_special_privileges()) {
    JavaVMOption options[N_MAX_OPTIONS];      // Construct option array
    jio_fprintf(defaultStream::error_stream(),
                "Picked up %s: %s\n", name, buffer);
    char* rd = buffer;                        // pointer to the input string (rd)
    int i;
    for (i = 0; i < N_MAX_OPTIONS;) {         // repeat for all options in the input string
      while (isspace(*rd)) rd++;              // skip whitespace
      if (*rd == 0) break;                    // we re done when the input string is read completely

      // The output, option string, overwrites the input string.
      // Because of quoting, the pointer to the option string (wrt) may lag the pointer to
      // input string (rd).
      char* wrt = rd;

      options[i++].optionString = wrt;        // Fill in option
      while (*rd != 0 && !isspace(*rd)) {     // unquoted strings terminate with a space or NULL
        if (*rd == '\'' || *rd == '"') {      // handle a quoted string
          int quote = *rd;                    // matching quote to look for
          rd++;                               // don't copy open quote
          while (*rd != quote) {              // include everything (even spaces) up until quote
            if (*rd == 0) {                   // string termination means unmatched string
              jio_fprintf(defaultStream::error_stream(),
                          "Unmatched quote in %s\n", name);
              return JNI_ERR;
            }
            *wrt++ = *rd++;                   // copy to option string
          }
          rd++;                               // don't copy close quote
        } else {
          *wrt++ = *rd++;                     // copy to option string
        }
      }
      // Need to check if we're done before writing a NULL,
      // because the write could be to the byte that rd is pointing to.
      if (*rd++ == 0) {
        *wrt = 0;
        break;
      }
      *wrt = 0;                               // Zero terminate option
    }
    // Construct JavaVMInitArgs structure and parse as if it was part of the command line
    JavaVMInitArgs vm_args;
    vm_args.version = JNI_VERSION_1_2;
    vm_args.options = options;
    vm_args.nOptions = i;
    vm_args.ignoreUnrecognized = IgnoreUnrecognizedVMOptions;

    if (PrintVMOptions) {
      const char* tail;
      for (int i = 0; i < vm_args.nOptions; i++) {
        const JavaVMOption *option = vm_args.options + i;
        if (match_option(option, "-XX:", &tail)) {
          logOption(tail);
        }
      }
    }

    return(parse_each_vm_init_arg(&vm_args, scp_p, scp_assembly_required_p, ENVIRON_VAR));
  }
  return JNI_OK;
}

void Arguments::set_shared_spaces_flags() {
  const bool must_share = DumpSharedSpaces || RequireSharedSpaces;
  const bool might_share = must_share || UseSharedSpaces;

  // The string table is part of the shared archive so the size must match.
  if (!FLAG_IS_DEFAULT(StringTableSize)) {
    // Disable sharing.
    if (must_share) {
      warning("disabling shared archive %s because of non-default "
              "StringTableSize", DumpSharedSpaces ? "creation" : "use");
    }
    if (might_share) {
      FLAG_SET_DEFAULT(DumpSharedSpaces, false);
      FLAG_SET_DEFAULT(RequireSharedSpaces, false);
      FLAG_SET_DEFAULT(UseSharedSpaces, false);
    }
    return;
  }

  // Check whether class data sharing settings conflict with GC, compressed oops
  // or page size, and fix them up.  Explicit sharing options override other
  // settings.
  const bool cannot_share = UseConcMarkSweepGC || CMSIncrementalMode ||
    UseG1GC || UseParNewGC || UseParallelGC || UseParallelOldGC ||
    UseCompressedOops || UseLargePages && FLAG_IS_CMDLINE(UseLargePages);
  if (cannot_share) {
    if (must_share) {
        warning("selecting serial gc and disabling large pages %s"
                "because of %s", "" LP64_ONLY("and compressed oops "),
                DumpSharedSpaces ? "-Xshare:dump" : "-Xshare:on");
        force_serial_gc();
        FLAG_SET_CMDLINE(bool, UseLargePages, false);
        LP64_ONLY(FLAG_SET_CMDLINE(bool, UseCompressedOops, false));
    } else {
      if (UseSharedSpaces && Verbose) {
        warning("turning off use of shared archive because of "
                "choice of garbage collector or large pages");
      }
      no_shared_spaces();
    }
  } else if (UseLargePages && might_share) {
    // Disable large pages to allow shared spaces.  This is sub-optimal, since
    // there may not even be a shared archive to use.
    FLAG_SET_DEFAULT(UseLargePages, false);
  }
}

// Disable options not supported in this release, with a warning if they
// were explicitly requested on the command-line
#define UNSUPPORTED_OPTION(opt, description)                    \
do {                                                            \
  if (opt) {                                                    \
    if (FLAG_IS_CMDLINE(opt)) {                                 \
      warning(description " is disabled in this release.");     \
    }                                                           \
    FLAG_SET_DEFAULT(opt, false);                               \
  }                                                             \
} while(0)

// Parse entry point called from JNI_CreateJavaVM

jint Arguments::parse(const JavaVMInitArgs* args) {

  // Sharing support
  // Construct the path to the archive
  char jvm_path[JVM_MAXPATHLEN];
  os::jvm_path(jvm_path, sizeof(jvm_path));
  char *end = strrchr(jvm_path, *os::file_separator());
  if (end != NULL) *end = '\0';
  char *shared_archive_path = NEW_C_HEAP_ARRAY(char, strlen(jvm_path) +
      strlen(os::file_separator()) + 20, mtInternal);
  if (shared_archive_path == NULL) return JNI_ENOMEM;
  strcpy(shared_archive_path, jvm_path);
  strcat(shared_archive_path, os::file_separator());
  strcat(shared_archive_path, "classes");
  DEBUG_ONLY(strcat(shared_archive_path, "_g");)
  strcat(shared_archive_path, ".jsa");
  SharedArchivePath = shared_archive_path;

  // Remaining part of option string
  const char* tail;

  // If flag "-XX:Flags=flags-file" is used it will be the first option to be processed.
  const char* hotspotrc = ".hotspotrc";
  bool settings_file_specified = false;
  bool needs_hotspotrc_warning = false;

  const char* flags_file;
  int index;
  for (index = 0; index < args->nOptions; index++) {
    const JavaVMOption *option = args->options + index;
    if (match_option(option, "-XX:Flags=", &tail)) {
      flags_file = tail;
      settings_file_specified = true;
    }
    if (match_option(option, "-XX:+PrintVMOptions", &tail)) {
      PrintVMOptions = true;
    }
    if (match_option(option, "-XX:-PrintVMOptions", &tail)) {
      PrintVMOptions = false;
    }
    if (match_option(option, "-XX:+IgnoreUnrecognizedVMOptions", &tail)) {
      IgnoreUnrecognizedVMOptions = true;
    }
    if (match_option(option, "-XX:-IgnoreUnrecognizedVMOptions", &tail)) {
      IgnoreUnrecognizedVMOptions = false;
    }
    if (match_option(option, "-XX:+PrintFlagsInitial", &tail)) {
      CommandLineFlags::printFlags(tty, false);
      vm_exit(0);
    }
    if (match_option(option, "-XX:NativeMemoryTracking", &tail)) {
      MemTracker::init_tracking_options(tail);
    }


#ifndef PRODUCT
    if (match_option(option, "-XX:+PrintFlagsWithComments", &tail)) {
      CommandLineFlags::printFlags(tty, true);
      vm_exit(0);
    }
#endif
  }

  if (IgnoreUnrecognizedVMOptions) {
    // uncast const to modify the flag args->ignoreUnrecognized
    *(jboolean*)(&args->ignoreUnrecognized) = true;
  }

  // Parse specified settings file
  if (settings_file_specified) {
    if (!process_settings_file(flags_file, true, args->ignoreUnrecognized)) {
      return JNI_EINVAL;
    }
  } else {
#ifdef ASSERT
    // Parse default .hotspotrc settings file
    if (!process_settings_file(".hotspotrc", false, args->ignoreUnrecognized)) {
      return JNI_EINVAL;
    }
#else
    struct stat buf;
    if (os::stat(hotspotrc, &buf) == 0) {
      needs_hotspotrc_warning = true;
    }
#endif
  }

  if (PrintVMOptions) {
    for (index = 0; index < args->nOptions; index++) {
      const JavaVMOption *option = args->options + index;
      if (match_option(option, "-XX:", &tail)) {
        logOption(tail);
      }
    }
  }

  // Parse JavaVMInitArgs structure passed in, as well as JAVA_TOOL_OPTIONS and _JAVA_OPTIONS
  jint result = parse_vm_init_args(args);
  if (result != JNI_OK) {
    return result;
  }

  // Delay warning until here so that we've had a chance to process
  // the -XX:-PrintWarnings flag
  if (needs_hotspotrc_warning) {
    warning("%s file is present but has been ignored.  "
            "Run with -XX:Flags=%s to load the file.",
            hotspotrc, hotspotrc);
  }

#if (defined JAVASE_EMBEDDED || defined ARM)
  UNSUPPORTED_OPTION(UseG1GC, "G1 GC");
#endif

#ifdef _ALLBSD_SOURCE  // UseLargePages is not yet supported on BSD.
  UNSUPPORTED_OPTION(UseLargePages, "-XX:+UseLargePages");
#endif

#ifndef PRODUCT
  if (TraceBytecodesAt != 0) {
    TraceBytecodes = true;
  }
  if (CountCompiledCalls) {
    if (UseCounterDecay) {
      warning("UseCounterDecay disabled because CountCalls is set");
      UseCounterDecay = false;
    }
  }
#endif // PRODUCT

  // JSR 292 is not supported before 1.7
  if (!JDK_Version::is_gte_jdk17x_version()) {
    if (EnableInvokeDynamic) {
      if (!FLAG_IS_DEFAULT(EnableInvokeDynamic)) {
        warning("JSR 292 is not supported before 1.7.  Disabling support.");
      }
      EnableInvokeDynamic = false;
    }
  }

  if (EnableInvokeDynamic && ScavengeRootsInCode == 0) {
    if (!FLAG_IS_DEFAULT(ScavengeRootsInCode)) {
      warning("forcing ScavengeRootsInCode non-zero because EnableInvokeDynamic is true");
    }
    ScavengeRootsInCode = 1;
  }
  if (!JavaObjectsInPerm && ScavengeRootsInCode == 0) {
    if (!FLAG_IS_DEFAULT(ScavengeRootsInCode)) {
      warning("forcing ScavengeRootsInCode non-zero because JavaObjectsInPerm is false");
    }
    ScavengeRootsInCode = 1;
  }

  if (PrintGCDetails) {
    // Turn on -verbose:gc options as well
    PrintGC = true;
  }

  if (!JDK_Version::is_gte_jdk18x_version()) {
    // To avoid changing the log format for 7 updates this flag is only
    // true by default in JDK8 and above.
    if (FLAG_IS_DEFAULT(PrintGCCause)) {
      FLAG_SET_DEFAULT(PrintGCCause, false);
    }
  }

  // Set object alignment values.
  set_object_alignment();

#ifdef SERIALGC
  force_serial_gc();
#endif // SERIALGC

  // Set flags based on ergonomics.
  set_ergonomics_flags();

  set_shared_spaces_flags();

  // Check the GC selections again.
  if (!check_gc_consistency()) {
    return JNI_EINVAL;
  }

  if (TieredCompilation) {
    set_tiered_flags();
  } else {
    // Check if the policy is valid. Policies 0 and 1 are valid for non-tiered setup.
    if (CompilationPolicyChoice >= 2) {
      vm_exit_during_initialization(
        "Incompatible compilation policy selected", NULL);
    }
  }
  set_heap_base_min_address();
  // Set heap size based on available physical memory
  set_heap_size();
  // Set per-collector flags
  if (UseParallelGC || UseParallelOldGC) {
    set_parallel_gc_flags();
  } else if (UseConcMarkSweepGC) { // should be done before ParNew check below
    set_cms_and_parnew_gc_flags();
  } else if (UseParNewGC) {  // skipped if CMS is set above
    set_parnew_gc_flags();
  } else if (UseG1GC) {
    set_g1_gc_flags();
  }

#ifdef SERIALGC
  assert(verify_serial_gc_flags(), "SerialGC unset");
#endif // SERIALGC

  // Set bytecode rewriting flags
  set_bytecode_flags();

  // Set flags if Aggressive optimization flags (-XX:+AggressiveOpts) enabled.
  set_aggressive_opts_flags();

  // Turn off biased locking for locking debug mode flags,
  // which are subtlely different from each other but neither works with
  // biased locking.
  if (UseHeavyMonitors
#ifdef COMPILER1
      || !UseFastLocking
#endif // COMPILER1
    ) {
    if (!FLAG_IS_DEFAULT(UseBiasedLocking) && UseBiasedLocking) {
      // flag set to true on command line; warn the user that they
      // can't enable biased locking here
      warning("Biased Locking is not supported with locking debug flags"
              "; ignoring UseBiasedLocking flag." );
    }
    UseBiasedLocking = false;
  }

#ifdef CC_INTERP
  // Clear flags not supported by the C++ interpreter
  FLAG_SET_DEFAULT(ProfileInterpreter, false);
  FLAG_SET_DEFAULT(UseBiasedLocking, false);
  LP64_ONLY(FLAG_SET_DEFAULT(UseCompressedOops, false));
#endif // CC_INTERP

#ifdef COMPILER2
  if (!EliminateLocks) {
    EliminateNestedLocks = false;
  }
  if (!Inline) {
    IncrementalInline = false;
  }
#ifndef PRODUCT
  if (!IncrementalInline) {
    AlwaysIncrementalInline = false;
  }
#endif
  if (IncrementalInline && FLAG_IS_DEFAULT(MaxNodeLimit)) {
    // incremental inlining: bump MaxNodeLimit
    FLAG_SET_DEFAULT(MaxNodeLimit, (intx)75000);
  }
#endif

  if (PrintAssembly && FLAG_IS_DEFAULT(DebugNonSafepoints)) {
    warning("PrintAssembly is enabled; turning on DebugNonSafepoints to gain additional output");
    DebugNonSafepoints = true;
  }

#ifndef PRODUCT
  if (CompileTheWorld) {
    // Force NmethodSweeper to sweep whole CodeCache each time.
    if (FLAG_IS_DEFAULT(NmethodSweepFraction)) {
      NmethodSweepFraction = 1;
    }
  }
#endif

  if (PrintCommandLineFlags) {
    CommandLineFlags::printSetFlags(tty);
  }

  // Apply CPU specific policy for the BiasedLocking
  if (UseBiasedLocking) {
    if (!VM_Version::use_biased_locking() &&
        !(FLAG_IS_CMDLINE(UseBiasedLocking))) {
      UseBiasedLocking = false;
    }
  }

#ifdef COMPILER2
  if (!UseBiasedLocking || EmitSync != 0) {
    UseOptoBiasInlining = false;
  }
#endif

  // set PauseAtExit if the gamma launcher was used and a debugger is attached
  // but only if not already set on the commandline
  if (Arguments::created_by_gamma_launcher() && os::is_debugger_attached()) {
    bool set = false;
    CommandLineFlags::wasSetOnCmdline("PauseAtExit", &set);
    if (!set) {
      FLAG_SET_DEFAULT(PauseAtExit, true);
    }
  }

  return JNI_OK;
}

int Arguments::PropertyList_count(SystemProperty* pl) {
  int count = 0;
  while(pl != NULL) {
    count++;
    pl = pl->next();
  }
  return count;
}

const char* Arguments::PropertyList_get_value(SystemProperty *pl, const char* key) {
  assert(key != NULL, "just checking");
  SystemProperty* prop;
  for (prop = pl; prop != NULL; prop = prop->next()) {
    if (strcmp(key, prop->key()) == 0) return prop->value();
  }
  return NULL;
}

const char* Arguments::PropertyList_get_key_at(SystemProperty *pl, int index) {
  int count = 0;
  const char* ret_val = NULL;

  while(pl != NULL) {
    if(count >= index) {
      ret_val = pl->key();
      break;
    }
    count++;
    pl = pl->next();
  }

  return ret_val;
}

char* Arguments::PropertyList_get_value_at(SystemProperty* pl, int index) {
  int count = 0;
  char* ret_val = NULL;

  while(pl != NULL) {
    if(count >= index) {
      ret_val = pl->value();
      break;
    }
    count++;
    pl = pl->next();
  }

  return ret_val;
}

void Arguments::PropertyList_add(SystemProperty** plist, SystemProperty *new_p) {
  SystemProperty* p = *plist;
  if (p == NULL) {
    *plist = new_p;
  } else {
    while (p->next() != NULL) {
      p = p->next();
    }
    p->set_next(new_p);
  }
}

void Arguments::PropertyList_add(SystemProperty** plist, const char* k, char* v) {
  if (plist == NULL)
    return;

  SystemProperty* new_p = new SystemProperty(k, v, true);
  PropertyList_add(plist, new_p);
}

// This add maintains unique property key in the list.
void Arguments::PropertyList_unique_add(SystemProperty** plist, const char* k, char* v, jboolean append) {
  if (plist == NULL)
    return;

  // If property key exist then update with new value.
  SystemProperty* prop;
  for (prop = *plist; prop != NULL; prop = prop->next()) {
    if (strcmp(k, prop->key()) == 0) {
      if (append) {
        prop->append_value(v);
      } else {
        prop->set_value(v);
      }
      return;
    }
  }

  PropertyList_add(plist, k, v);
}

// Copies src into buf, replacing "%%" with "%" and "%p" with pid
// Returns true if all of the source pointed by src has been copied over to
// the destination buffer pointed by buf. Otherwise, returns false.
// Notes:
// 1. If the length (buflen) of the destination buffer excluding the
// NULL terminator character is not long enough for holding the expanded
// pid characters, it also returns false instead of returning the partially
// expanded one.
// 2. The passed in "buflen" should be large enough to hold the null terminator.
bool Arguments::copy_expand_pid(const char* src, size_t srclen,
                                char* buf, size_t buflen) {
  const char* p = src;
  char* b = buf;
  const char* src_end = &src[srclen];
  char* buf_end = &buf[buflen - 1];

  while (p < src_end && b < buf_end) {
    if (*p == '%') {
      switch (*(++p)) {
      case '%':         // "%%" ==> "%"
        *b++ = *p++;
        break;
      case 'p':  {       //  "%p" ==> current process id
        // buf_end points to the character before the last character so
        // that we could write '\0' to the end of the buffer.
        size_t buf_sz = buf_end - b + 1;
        int ret = jio_snprintf(b, buf_sz, "%d", os::current_process_id());

        // if jio_snprintf fails or the buffer is not long enough to hold
        // the expanded pid, returns false.
        if (ret < 0 || ret >= (int)buf_sz) {
          return false;
        } else {
          b += ret;
          assert(*b == '\0', "fail in copy_expand_pid");
          if (p == src_end && b == buf_end + 1) {
            // reach the end of the buffer.
            return true;
          }
        }
        p++;
        break;
      }
      default :
        *b++ = '%';
      }
    } else {
      *b++ = *p++;
    }
  }
  *b = '\0';
  return (p == src_end); // return false if not all of the source was copied
}
