/******************************** -*- C -*- ****************************
 *
 *	Public library entry points
 *
 *
 ***********************************************************************/

/***********************************************************************
 *
 * Copyright 1988,89,90,91,92,94,95,99,2000,2001,2002,2003
 * Free Software Foundation, Inc.
 * Written by Steve Byrne.
 *
 * This file is part of GNU Smalltalk.
 *
 * GNU Smalltalk is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any later 
 * version.
 * 
 * GNU Smalltalk is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * GNU Smalltalk; see the file COPYING.  If not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.  
 *
 ***********************************************************************/



#include "gstpriv.h"
#include "getopt.h"

#ifndef MAXPATHLEN
#define MAXPATHLEN		1024	/* max length of a file and path */
#endif


/* Define to debug the getopt code. */
/* #define DEBUG_GETOPT */

#ifdef atarist
#define INIT_FILE_NAME		".gstinit"
#define PRE_IMAGE_FILE_NAME	".gstpre"	/* hope this is ok */
#else
#ifdef MSDOS
#define INIT_FILE_NAME		"_stinit"
#define PRE_IMAGE_FILE_NAME	"_stpre"
#else
#define INIT_FILE_NAME		".stinit"
#define PRE_IMAGE_FILE_NAME	".stpre"
#endif
#endif


extern int _gst_yydebug;


static const char help_text[] =
  "GNU Smalltalk usage:"
  "\n"
  "\n    gst [ flag ... ] [ file ...]"
  "\n"
  "\nShort flags can appear either as -xyz or as -x -y -z.  If an option is"
  "\nmandatory for a long option, it is also mandatory for a short one. The"
  "\ncurrently defined set of flags is:"
  "\n   -a --smalltalk\t\t Pass the remaining arguments to Smalltalk"
  "\n   -c --core-dump\t\t Dump core on fatal signal"
  "\n   -d --declaration-trace-user\t Trace compilation of files on the command line"
  "\n   -D --declaration-trace-all\t Trace compilation of all loaded files"
#ifndef USE_JIT_TRANSLATION
  "\n   -e --execution-trace-user\t Trace execution of files on the command line"
  "\n   -E --execution-trace-all\t Trace execution of all loaded files"
#endif
  "\n   -g --no-gc-message\t\t Do not print garbage collection messages"
  "\n   -H -h -? --help\t\t Print this message and exit"
  "\n   -i --rebuild-image\t\t Ignore the image file; rebuild it from scratch"
  "\n   -I --image-file file\t\t Instead of `gst.im', use `file' as the image\n\t\t\t\t file, and ignore the kernel files' timestamps\n"
  "\n   -K --kernel-file file\t Make file's path relative to the image path."
  "\n   -l --log-changes\t\t Log changes in `change-log.st'"
  "\n   -L --log-file file\t\t Log changes in file `file'"
  "\n   -p --emacs-mode\t\t Execute as a `process' (from within Emacs)"
  "\n   -q --quiet --silent\t\t Do not print execution information"
  "\n   -Q --no-messages\t\t Run Smalltalk with -q -g and no startup banner"
  "\n   -r --regression-test\t\t Run in regression test mode, i.e. make\n\t\t\t\t printed messages constant\n"
  "\n   -S --snapshot\t\t Save a snapshot just before exiting"
  "\n   -v --version\t\t\t Print the Smalltalk version number and exit"
  "\n   -V --verbose\t\t\t Print names of loaded files and execution stats"
  "\n   -y --yacc-debug\t\t Turn on debugging in the parser"
  "\n   -\t\t\t\t Read input from standard input explicitly"
  "\n"
  "\nFiles are loaded one after the other.  After the last one is loaded,"
  "\nSmalltalk will exit.  If no files are specified, Smalltalk reads from"
  "\nthe terminal, with prompts."
  "\n" "\nReport bugs to help-smalltalk@gnu.org\n";

static const char copyright_and_legal_stuff_text[] =
  "GNU Smalltalk version %s"
  "\nCopyright 2003 Free Software Foundation, Inc."
  "\nWritten by Steve Byrne (sbb@gnu.org) and Paolo Bonzini (bonzini@gnu.org)"
  "\n"
  "\nGNU Smalltalk comes with NO WARRANTY, to the extent permitted by law."
  "\nYou may redistribute copies of GNU Smalltalk under the terms of the"
  "\nGNU General Public License.  For more information, see the file named"
  "\nCOPYING."
  "\n"
  "\nUsing default kernel path: %s" "\nUsing default image path: %s"
  "\n";

static const struct option long_options[] = {
  {"smalltalk", 0, 0, 'a'},
  {"core-dump", 0, 0, 'c'},
  {"declaration-trace-user", 0, 0, 'd'},
  {"declaration-trace-all", 0, 0, 'D'},
#ifndef USE_JIT_TRANSLATION
  {"execution-trace-user", 0, 0, 'e'},
  {"execution-trace-all", 0, 0, 'E'},
#endif
  {"no-gc-message", 0, 0, 'g'},
  {"help", 0, 0, 'H'},
  {"rebuild-image", 0, 0, 'i'},
  {"image-file", 1, 0, 'I'},
  {"kernel-file", 1, 0, 'K'},
  {"log-changes", 0, 0, 'l'},
  {"log-file", 1, 0, 'L'},
  {"emacs-mode", 0, 0, 'p'},
  {"quiet", 0, 0, 'q'},
  {"no-messages", 0, 0, 'Q'},
  {"silent", 0, 0, 'q'},
  {"regression-test", 0, 0, 'r'},
  {"store-no-source", 0, 0, 's'},
  {"snapshot", 0, 0, 'S'},
  {"version", 0, 0, 'v'},
  {"verbose", 0, 0, 'V'},
  {"yacc-debug", 0, 0, 'y'},
  {NULL, 0, 0, 0}
};


/* When true, this flag suppresses the printing of execution-related
 * messages, such as the number of byte codes executed by the
 * last expression, etc.
 */
mst_Boolean _gst_quiet_execution;

/* Set by cmd line flag.  If true, Smalltalk is more verbose about
   what it's doing. */
mst_Boolean _gst_verbose = false;

/* These contain the default path that was picked (after looking at the
   environment variables) for the kernel files and the image. */
char *_gst_kernel_file_default_path, *_gst_image_file_default_path;

/* This is the name of the binary image to load.  If it is not NULL after the
   command line is parsed, the checking of the dates of the kernel source files
   against the image file date is overridden.  If it is NULL, it is set to
   default_image_name. */
char *_gst_binary_image_name = NULL;

/* This is used by the callin functions to auto-initialize Smalltalk.
   When it's not true, initialization needs to be performed.  It's set
   to true by gst_init_smalltalk(). */
mst_Boolean _gst_smalltalk_initialized = false;

/* This is used to avoid doing complicated things (currently, this
   includes call-ins before and after _gst_execute_statements) before
   the system is ready to do them. */
mst_Boolean _gst_kernel_initialized = false;

/* If true, even both kernel and user method definitions are shown as
   they are compiled.  */
mst_Boolean _gst_trace_kernel_declarations;

/* If true, execution tracing is performed when loading kernel method
   definitions. */
mst_Boolean _gst_trace_kernel_execution;

/* This is TRUE if we are doing regression testing, and causes
   whatever sources of variance to be suppressed (such as printing out
   execution statistics).  */
mst_Boolean _gst_regression_testing;



/***********************************************************************
 *
 *	Private declarations
 *
 ***********************************************************************/

/* Parse the Smalltalk source code contained in FILENAME.  If QUIET
   is true, for no reason show messages about the execution state (this
   is turned on while loading kernel files).  */
static mst_Boolean process_file (char *fileName);

/* Parse the Smalltalk source code read from stdin.  */
static void process_stdin ();

/* Answer whether it is ok to load the binary image pointed to by
   _gst_binary_image_name.  This is good is the image file is local
   and newer than all of the kernel files, or if the image file is
   global, newer than all of the global kernel files, and no local
   kernel file is found. */
static mst_Boolean ok_to_load_binary (void);

/* Attempts to find a viable kernel Smalltalk file (.st file).  First
   tries the current directory to allow for overriding installed kernel
   files.  If that isn't found, the full path name of the installed kernel
   file is stored in fullFileName.  Note that the directory part of the
   kernel file name in this second case can be overridden by defining the
   SMALLTALK_KERNEL environment variable to be the directory that should
   serve as the kernel directory instead of the installed one.

   FILENAME is a simple file name, sans directory; the file name to use 
   for the particular kernel file is returned in the FULLFILENAME variable
   in this variable (which must be a string large enough for any file name).
   If there is a file in the current directory with name FILENAME, that is
   returned; otherwise the kernel path is prepended to FILENAME (separated
   by a slash, of course) and that is stored in the string pointed to by
   FULLFILENAME.

   Returns true if the kernel file is found in the local directory,
   and false if the file was found using the default path.
 */
static mst_Boolean find_kernel_file (const char *fileName,
				     char *fullFileName);

/* Loads the kernel Smalltalk files.  It uses a vector of file names,
   and loads each file individually.  To provide for greater
   flexibility, if a one of the files exists in the current directory,
   that is used in preference to one in the default location.  The
   default location can be overridden at runtime by setting the
   SMALLTALK_KERNEL environment variable.  */
static int load_standard_files (void);

/* Sets up the paths for the kernel source directory and for where the
   saved Smalltalk binary image lives.  Uses environment variables
   SMALLTALK_KERNEL and SMALLTALK_IMAGE if they are set, otherwise
   uses the paths passed by the makefiles. */
static void init_paths (void);

/* This routine scans the command line arguments, accumulating
   information and setting flags. */
static int parse_args (int argc, char **argv);

/* These functions load the per-user customization files, respectively
   .stinit (loaded at every startup) and .stpre (loaded before a local
   image is saved. */
static void load_user_init_file (void);
static void load_user_pre_image_file (void);

/* Set by command line flag.  When true, Smalltalk saves a snapshot after
   loading the files on the command line, before exiting. */
static mst_Boolean snapshot_after_load = false;

/* Whether SMALLTALK_IMAGE is set (only if it is set, .stpre is loaded) */
static mst_Boolean is_local_image;

/* The _gst_image_file_default_path followed by /gst.im */
static char *default_image_name;

/* Set by command line flag.  When true, Smalltalk does not print any
   sort of version banner at the startup of the interpreter.  This
   makes Smalltalk usable as a filter, or as a pure producer of
   content (such as the main program in a cgi-bin script). */
static mst_Boolean no_errors = false;

/* If true, skip date checking of kernel files vs. binary image; pretend
   that binary image does not exist. */
static mst_Boolean ignore_image;

/* The complete list of "kernel" class and method definitions.  Each
   of these files is loaded, in the order given below.  Their last
   modification dates are compared against that of the image file; if
   any are newer, the image file is ignored, these files are loaded,
   and a new image file is created. */
static const char *standard_files[] = {
  "Builtins.st",
  "Object.st",
  "Message.st",
  "DirMessage.st",
  "Boolean.st",
  "False.st",
  "True.st",
  "Magnitude.st",
  "Integer.st",
  "Date.st",
  "Time.st",
  "Number.st",
  "Float.st",
  "FloatD.st",
  "FloatE.st",
  "FloatQ.st",
  "Fraction.st",
  "LargeInt.st",
  "SmallInt.st",
  "Character.st",
  "LookupKey.st",
  "Association.st",
  "HomedAssoc.st",
  "VarBinding.st",
  "Link.st",
  "Process.st",
  "CallinProcess.st",
  "CompildCode.st",
  "CompildMeth.st",
  "CompiledBlk.st",
  "Collection.st",
  "SeqCollect.st",
  "LinkedList.st",
  "Semaphore.st",
  "ArrayColl.st",
  "Array.st",
  "ByteArray.st",
  "CharArray.st",
  "String.st",
  "Symbol.st",
  "Interval.st",
  "OrderColl.st",
  "SortCollect.st",
  "Bag.st",
  "MappedColl.st",
  "HashedColl.st",
  "Set.st",
  "IdentitySet.st",
  "Dictionary.st",
  "LookupTable.st",
  "IdentDict.st",
  "MethodDict.st",
  "BindingDict.st",
  "AbstNamespc.st",
  "RootNamespc.st",
  "Namespace.st",
  "SysDict.st",
  "Stream.st",
  "PosStream.st",
  "ReadStream.st",
  "WriteStream.st",
  "RWStream.st",
  "ByteStream.st",
  "TokenStream.st",
  "Random.st",
  "UndefObject.st",
  "ProcSched.st",
  "Delay.st",
  "SharedQueue.st",
  "Behavior.st",
  "ClassDesc.st",
  "Class.st",
  "Metaclass.st",
  "ContextPart.st",
  "MthContext.st",
  "BlkContext.st",
  "BlkClosure.st",
  "Memory.st",
  "MethodInfo.st",
  "FileSegment.st",
  "FileDescr.st",
  "SymLink.st",
  "ObjMemory.st",

  /* More core classes */
  "WeakObjects.st",
  "RecursionLock.st",
  "Point.st",
  "Rectangle.st",
  "RunArray.st",
  "OtherArrays.st",
  "AnsiDates.st",
  "ScaledDec.st",
  "ValueAdapt.st",

  /* C call-out facilities */
  "CObject.st",
  "CType.st",
  "CFuncs.st",
  "CStruct.st",

  /* Exception handling */
  "ExcHandling.st",
  "AnsiExcept.st",

  /* Virtual filesystem layer */
  "File.st",
  "Directory.st",
  "VFS.st",
  "URL.st",
  "FileStream.st",
  "Transcript.st",

  /* Goodies */
  "Autoload.st",
  "ObjDumper.st",
  "PkgLoader.st",
  "DLD.st",
  NULL
};

/* The argc and argv that are passed to libgst via gst_smalltalk_args. 
   The default is passing no parameters. */
static char *smalltalk_arg_vec[] = { "gst", NULL };
static int smalltalk_argc = 0;
static char **smalltalk_argv = smalltalk_arg_vec;

/* The argc and argv that are made available to Smalltalk programs
   through the -a option. */
int _gst_smalltalk_passed_argc = 0;
char **_gst_smalltalk_passed_argv = NULL;



void
gst_smalltalk_args (int argc,
		    char **argv)
{
  smalltalk_argc = argc;
  smalltalk_argv = argv;
}


int
gst_init_smalltalk (void)
{
  mst_Boolean loadBinary, abortOnFailure;
  mst_Boolean traceUserDeclarations, traceUserExecution;
  int result;
  char *p;

  _gst_yydebug = 0;
  _gst_trace_kernel_declarations = _gst_declare_tracing = false;
  _gst_trace_kernel_execution = _gst_execution_tracing = false;
  _gst_regression_testing = false;
  _gst_verbose = false;
  ignore_image = false;

  /* Even though we're nowhere near through initialization, we set this
     to make sure we don't invoke a callin function which would recursively
     invoke us.  */
  _gst_smalltalk_initialized = true;

  init_paths ();
  _gst_init_snprintfv ();

  result = parse_args (smalltalk_argc, smalltalk_argv);
  if (result)
    return result;

  _gst_init_sysdep ();
  _gst_init_signals ();
  _gst_init_cfuncs ();
  _gst_init_primitives ();

  if (_gst_regression_testing)
    {
      _gst_trace_kernel_declarations = _gst_declare_tracing = false;
      _gst_trace_kernel_execution = _gst_execution_tracing = false;
      _gst_verbose = false;
      setvbuf (stdout, NULL, _IOLBF, 1024);
    }

  traceUserDeclarations = _gst_declare_tracing;
  traceUserExecution = _gst_execution_tracing;
  _gst_declare_tracing = _gst_trace_kernel_declarations;
  _gst_execution_tracing = _gst_trace_kernel_execution;

  if (_gst_binary_image_name)
    loadBinary = abortOnFailure = true;
  else
    {
      _gst_binary_image_name = default_image_name;
      loadBinary = !ignore_image && ok_to_load_binary();
      abortOnFailure = false;

      /* If we must create a new non-local image, but the directory is
         not writeable, we must resort to the current directory.  In
         practice this is what is used when working in the build directory.  */

      if (!loadBinary
          && !is_local_image
          && !_gst_file_is_writeable (_gst_image_file_default_path))
        {
          xfree (_gst_binary_image_name);

          is_local_image = true;
          xfree (_gst_image_file_default_path);
          _gst_image_file_default_path = _gst_get_cur_dir_name ();
          _gst_binary_image_name = "gst.im";
          loadBinary = !ignore_image && ok_to_load_binary();
        }
    }

  /* Compute the actual path of the image file */
  xfree (_gst_image_file_default_path);

  p = _gst_image_file_default_path =
    _gst_get_full_file_name (_gst_binary_image_name);
  p += strlen (_gst_image_file_default_path);
#if defined(MSDOS) || defined(WIN32) || defined(__OS2__)
  while (*--p != '/' && *p != '\\');
#else
  while (*--p != '/');
#endif
  *p = 0;

  if (loadBinary && _gst_load_from_file (_gst_binary_image_name))
    {
      _gst_init_changes_stream ();
      _gst_init_compiler ();
      _gst_init_interpreter ();
      _gst_init_vmproxy ();
    }
  else
    {
      mst_Boolean willRegressTest = _gst_regression_testing;
      _gst_regression_testing = false;

      if (abortOnFailure)
	{
	  _gst_errorf ("Couldn't load image file %s", _gst_binary_image_name);
	  return 1;
	}

      _gst_init_oop_table (INITIAL_OOP_TABLE_SIZE);
      _gst_init_mem_default ();
      _gst_init_dictionary ();
      _gst_init_compiler ();
      _gst_init_interpreter ();
      _gst_init_vmproxy ();

      _gst_reset_changes_file ();
      _gst_install_initial_methods ();
      _gst_init_changes_stream ();

      result = load_standard_files ();
      if (!result)
	{
          if (is_local_image)
	    load_user_pre_image_file ();
	}

      _gst_regression_testing = willRegressTest;
      if (result)
	return result;

      if (!_gst_save_to_file (_gst_binary_image_name))
	_gst_errorf ("Couldn't open file %s", _gst_binary_image_name);
    }

  _gst_kernel_initialized = true;
  _gst_invoke_hook ("returnFromSnapshot");
  load_user_init_file ();

  _gst_declare_tracing = traceUserDeclarations;
  _gst_execution_tracing = traceUserExecution;

#ifdef HAVE_READLINE
  _gst_initialize_readline ();
#endif /* HAVE_READLINE */

  return 0;
}

void
gst_top_level_loop (void)
{
  int filesProcessed;

  for (filesProcessed = 0; *++smalltalk_argv;)
    {
      if (smalltalk_argv[0][0] == '-')
        /* - by itself indicates standard input */
        process_stdin ();
      else
	{
	  _gst_use_undeclared++;
	  if (!process_file (smalltalk_argv[0]) && !no_errors)
	    _gst_errorf ("Couldn't open file %s", smalltalk_argv[0]);

	  _gst_use_undeclared--;
	}
      filesProcessed++;
    }

  if (filesProcessed == 0)
    process_stdin ();

  if (snapshot_after_load)
    _gst_save_to_file (_gst_binary_image_name);

  _gst_invoke_hook ("aboutToQuit");
}



void
init_paths (void)
{
  char *currentDirectory = _gst_get_cur_dir_name ();

  _gst_kernel_file_default_path = (char *) getenv ("SMALLTALK_KERNEL");
  _gst_image_file_default_path = (char *) getenv ("SMALLTALK_IMAGE");
  is_local_image = true;

  if (!_gst_kernel_file_default_path
      || !_gst_file_is_readable (_gst_kernel_file_default_path))
    asprintf (&_gst_kernel_file_default_path, "%s/kernel",
	      currentDirectory);

  if (!_gst_image_file_default_path
      || !_gst_file_is_readable (_gst_image_file_default_path))
    {
      _gst_image_file_default_path = IMAGE_PATH;

      if (_gst_file_is_readable (_gst_image_file_default_path))
	/* Found in the standard image path.  Apply this kludge so
	   that OSes such as Windows and MS-DOS which have no concept 
	   of home directories always load the .stpre file. */
	is_local_image = (((char *) getenv ("HOME")) == NULL);
      else
	_gst_image_file_default_path = currentDirectory;
    }

  asprintf (&default_image_name, "%s/gst.im",
	    _gst_image_file_default_path);

  _gst_image_file_default_path = xstrdup (_gst_image_file_default_path);
  xfree (currentDirectory);
}

mst_Boolean
ok_to_load_binary (void)
{
  long imageFileTime;
  const char **fileNames;
  char fullFileName[MAXPATHLEN], *home, preImageFileName[MAXPATHLEN];

  imageFileTime = _gst_get_file_modify_time (_gst_binary_image_name);

  if (imageFileTime == 0)	/* not found */
    return (false);

  for (fileNames = standard_files; *fileNames; fileNames++)
    {
      if (find_kernel_file (*fileNames, fullFileName)
	  && !is_local_image)
	{
	  /* file lives locally but the image doesn't -- bad semantics.
	     Note that if SOME of the files are local and the image file
	     is local, it is good. */
	  return (false);
	}
      if (imageFileTime < _gst_get_file_modify_time (fullFileName))
	return (false);
    }

  if (is_local_image)
    {
      if ((home = (char *) getenv ("HOME")) != NULL)
	sprintf (preImageFileName, "%s/%s", home, PRE_IMAGE_FILE_NAME);
      else
	strcpy (preImageFileName, PRE_IMAGE_FILE_NAME);

      if (imageFileTime < _gst_get_file_modify_time (preImageFileName))
	return (false);
    }

  return (true);
}

int
load_standard_files (void)
{
  const char **fileNames;
  char fullFileName[MAXPATHLEN];

  _gst_use_undeclared++;
  for (fileNames = standard_files; *fileNames; fileNames++)
    {
      find_kernel_file (*fileNames, fullFileName);
      if (!process_file (fullFileName))
	{
	  _gst_errorf ("can't find system file '%s'", fullFileName);
	  _gst_errorf
	    ("image bootstrap failed, set SMALLTALK_KERNEL to the directory name");
	  return 1;
	}
    }
  _gst_use_undeclared--;
  return 0;
}


mst_Boolean
find_kernel_file (const char *fileName,
		  char *fullFileName)
{
  if (_gst_file_is_readable (fileName))
    {
      strcpy (fullFileName, fileName);
      return (true);
    }

  sprintf (fullFileName, "%s/%s", _gst_kernel_file_default_path,
	   fileName);
  if (_gst_file_is_readable (fullFileName))
    {
      char systemFileName[256];
      sprintf (systemFileName, KERNEL_PATH "/%s", fileName);
      /* If this file and the system file are the same, consider the
         file as a system file instead. */
      return (!_gst_file_is_readable (systemFileName) ||
	      _gst_get_file_modify_time (fullFileName) !=
	      _gst_get_file_modify_time (systemFileName));
    }

  sprintf (fullFileName, KERNEL_PATH "/%s", fileName);
  return (false);
}


void
load_user_pre_image_file (void)
{
  char fileName[MAXPATHLEN], *home;

  if ((home = (char *) getenv ("HOME")) != NULL)
    sprintf (fileName, "%s/%s", home, PRE_IMAGE_FILE_NAME);
  else
    strcpy (fileName, PRE_IMAGE_FILE_NAME);

  process_file (fileName);
}

void
load_user_init_file (void)
{
  char fileName[MAXPATHLEN], *home;

  if (_gst_regression_testing)
    return;

  if ((home = (char *) getenv ("HOME")) != NULL)
    sprintf (fileName, "%s/%s", home, INIT_FILE_NAME);
  else
    strcpy (fileName, INIT_FILE_NAME);

  process_file (fileName);
}

void
process_stdin ()
{
 if (!no_errors)
    {
      printf ("GNU Smalltalk ready\n\n");
      fflush (stdout);
    }

  _gst_non_interactive = false;
  _gst_push_stdin_string ();
  _gst_parse_stream ();
  _gst_pop_stream (true);
  _gst_non_interactive = true;
}

mst_Boolean
process_file (char *fileName)
{
  int fd;

  fd = _gst_open_file (fileName, "r");
  if (fd == -1)
    return (false);

  if (_gst_verbose)
    printf ("Processing %s\n", fileName);

  _gst_push_unix_file (fd, fileName);
  _gst_parse_stream ();
  _gst_pop_stream (true);
  return (true);
}


int
parse_args (int argc,
	    char **argv)
{
  char **av = argv;
  int ch, prev_optind = 0, minus_a_optind = -1;

#ifndef USE_DYNAMIC_TRANSLATION
# define OPTIONS "-acdDeEghHiI:K:lL:pQqrsSvVy"
#else
# define OPTIONS "-acdDghHiI:K:lL:pQqrsSvVy"
#endif

  /* get rid of getopt's own error reporting for invalid options */
  opterr = 1;

  while ((ch =
	  getopt_long (argc, (char **) argv, OPTIONS, long_options, NULL)) != -1)
    {
#undef OPTIONS

#if DEBUG_GETOPT
      printf ("%c \"%s\" %d     %d\n", ch, optarg ? optarg : "",
	      optind, minus_a_optind);
#endif

      switch (ch)
	{
	case 'c':
	  _gst_make_core_file = true;
	  break;
	case 'D':
	  _gst_trace_kernel_declarations = true;	/* fall thru */
	case 'd':
	  _gst_declare_tracing = true;
	  break;
#ifndef USE_JIT_TRANSLATION
	case 'E':
	  _gst_trace_kernel_execution = true;		/* fall thru */
	case 'e':
	  _gst_execution_tracing = true;
	  break;
#endif
	case 'g':
	  _gst_gc_message = false;
	  break;
	case 'i':
	  ignore_image = true;
	  break;
	case 'p':
	  _gst_emacs_process = true;
	  break;
	case 'Q':
	  _gst_gc_message = false;
	  no_errors = true;	/* fall thru */
	case 'q':
          _gst_quiet_execution = true;
	  break;
	case 'r':
	  _gst_regression_testing = true;
	  break;
	case 's':
	  /* _gst_store_no_source = true;  always enabled, will die in 2.2 */
	  break;
	case 'S':
	  snapshot_after_load = true;
	  break;
	case 'V':
	  _gst_verbose = true;
	  break;
	case 'y':
	  _gst_yydebug = 1;
	  break;

	  /* ??? Is there some better place for this to be? Canonical
	     places seem to lose in that canonical places are often
	     write-protected, which is ok for extant changes (like in a 
	     saved system), but not for new changes by the user.
	     Again, the notion of "the" user's home area is kind of
	     random if there are multiple snapshots, since each
	     snapshot should be associated with a particular changes
	     file for which it is relevant. We use the current
	     directory and allow to override with the -L option. */
	case 'l':
	  _gst_change_file_name = "change-log.st";
	  break;

	case 'a':
	  /* "Officially", the C command line ends here.  The Smalltalk 
	     command line, instead, starts right after the parameter
	     containing -a. -a is handled specially by the code that
	     tests the minus_a_optind variable, so that ./gst -aI 
	     xxx yyy for example interprets xxx as the image to be
	     loaded.  */
	  minus_a_optind = optind;
	  break;

	case 'I':
	  _gst_binary_image_name = optarg;
          if (!_gst_file_is_readable (_gst_binary_image_name))
	    {
	      _gst_errorf ("Couldn't open image file %s", _gst_binary_image_name);
	      return 1;
	    }
	  break;

	case 'K':
	  {
	    char *file;
	    asprintf (&file, "%s/%s", _gst_image_file_default_path, optarg);
	    *++av = file;
	    break;
	  }

	case 'L':
	  _gst_change_file_name = optarg;
	  break;

	case 'v':
	  printf (copyright_and_legal_stuff_text, VERSION, KERNEL_PATH,
		  IMAGE_PATH);
	  return -1;

	case '\1':
	  *++av = optarg;
	  break;

	default:
	  printf (help_text);
	  return 1;

	case 'h':
	case 'H':
	case '?':
	  printf (help_text);
	  return -1;
	}

      if (minus_a_optind > -1)
	{
	  /* If the first argument was not an option, undo and leave.  */
	  if (ch == '\1')
	    optind--;

	  /* If the first argument after -a was not an option, or if there
	     is nothing after -a, or if we finished processing the argument
	     which included -a, leave.  */
	  if (ch == '\1' || optind > prev_optind || optind > minus_a_optind)
	    break;
	}
      prev_optind = optind;
    }

  *++av = NULL;
  if (minus_a_optind > -1)
    {
      _gst_smalltalk_passed_argc = argc - optind;
      _gst_smalltalk_passed_argv = argv + optind;
    }

  return 0;
}