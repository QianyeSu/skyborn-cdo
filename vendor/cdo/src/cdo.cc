/*
  This file is part of CDO. CDO is a collection of Operators to manipulate and analyse Climate model Data.

  Author: Uwe Schulzweida

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <iostream>
#include <vector>

#include <sys/stat.h>

#include <unistd.h>  // sysconf
#include <cstring>
#include <cstdlib>
#include <csignal>

#include <cdi.h>

#include "cdo_timer.h"
#include "cdo_getopt.h"
#include "cdo_settings.h"
#include "cdo_rlimit.h"
#include "cdo_default_values.h"
#include "param_conversion.h"
#include "progress.h"
#include "module_info.h"
#include "util_wildcards.h"
#include "process_int.h"
#include "processManager.h"
#include "commandline.h"
#include "mpmo_color.h"
#include "cdo_output.h"
#include "cdo_features.h"
#include "cdo_pthread.h"
#include "parser.h"
#include "factory.h"
#include "cdo_def_options.h"
#include "fileStream.h"

static ProcessManager g_processManager;

void
cdo_exit(std::string msg = "")
{
  (void) msg;
  g_processManager.kill_processes();
  exit(EXIT_FAILURE);
}

static bool applyDryRun = false;

static void
cdo_display_syntax_help(std::string const &help, FILE *p_target)
{
  set_text_color(p_target, BRIGHT, BLUE);
  std::string pad = CLIOptions::pad_size_terminal('=');
  fprintf(p_target, "%s", pad.c_str());
  reset_text_color(p_target);
  fprintf(p_target, "%s", help.c_str());
  set_text_color(p_target, BRIGHT, BLUE);
  pad = CLIOptions::pad_size_terminal('=');
  fprintf(p_target, "%s", pad.c_str());
  reset_text_color(p_target);
}

static void
print_category(std::string const &p_category, FILE *p_target)
{
  const auto options = CLIOptions::print_options_help(p_category);
  if (!options.empty())
  {
    const auto pad = CLIOptions::pad_size_terminal('=', p_category);
    fprintf(p_target, "%s", pad.c_str());
    set_text_color(p_target, BLUE);
    fprintf(p_target, "%s", options.c_str());
    reset_text_color(p_target);
  }
}

static void
cdo_usage(FILE *target)
{
  auto pad = CLIOptions::pad_size_terminal('-');
  fprintf(target, "%s", pad.c_str());
  fprintf(target, "  Usage : cdo  [Options]  Operator1  [-Operator2  [-OperatorN]]\n");
  pad = CLIOptions::pad_size_terminal('-');
  fprintf(target, "%s\n", pad.c_str());

  print_category("Info", target);
  print_category("Output", target);
  print_category("Multi Threading", target);
  print_category("Search Methods", target);
  print_category("Format Specific", target);
  print_category("CGRIBEX", target);
  print_category("Numeric", target);
  print_category("History", target);
  print_category("Compression", target);
  print_category("Hirlam Extensions", target);
  print_category("Options", target);
  print_category("Help", target);

  pad = CLIOptions::pad_size_terminal('=', "Environment Variables");
  fprintf(target, "%s\n", pad.c_str());
  set_text_color(target, BLUE);
  fprintf(target, "%s", CLIOptions::print_envvar_help().c_str());
  reset_text_color(target);
  fprintf(target, "\n");
  /*
  pad = CLIOptions::pad_size_terminal('=', "Syntax Features");
  fprintf(target, "%s", pad.c_str());
  fprintf(target, "%s\n", "    Apply");
  fprintf(target, "%s\n", Parser::apply_help.c_str());
  pad = CLIOptions::pad_size_terminal('-');
  fprintf(target, "%s", pad.c_str());
  fprintf(target, "%s\n", "    Subgroups");
  fprintf(target, "%s\n", Parser::subgroup_help.c_str());
  */
  pad = CLIOptions::pad_size_terminal('=');
  fprintf(target, "%s\n", pad.c_str());
  fprintf(target, "    CDO version %s, Copyright (C) 2002-2025 MPI für Meteorologie\n", VERSION);
  fprintf(target, "    This is free software and comes with ABSOLUTELY NO WARRANTY\n");
  fprintf(target, "    Report bugs to <https://mpimet.mpg.de/cdo>\n\n");
  pad = CLIOptions::pad_size_terminal('=');
  fprintf(target, "%s", pad.c_str());
}

static void
cdo_init_is_tty()
{
  struct stat statbuf;
  fstat(0, &statbuf);
  if (S_ISCHR(statbuf.st_mode)) { cdo::stdinIsTerminal = true; }
  fstat(1, &statbuf);
  if (S_ISCHR(statbuf.st_mode)) { cdo::stdoutIsTerminal = true; }
  fstat(2, &statbuf);
  if (S_ISCHR(statbuf.st_mode)) { cdo::stderrIsTerminal = true; }
}

static void
get_env_vars()
{
  CLIOptions::envvar("CDO_TEST")
      ->add_effect([&](std::string const &envstr) { Options::test = parameter_to_bool(envstr); })
      ->describe_argument("true|false")
      ->add_default("false")
      ->add_help("'true' test new features [default: false].");

  CLIOptions::envvar("CDO_ASYNC_READ")
      ->add_effect(
          [&](std::string const &envstr)
          {
            Options::CDO_Async_Read = parameter_to_bool(envstr);
            Options::CDO_task = Options::CDO_Async_Read;
          })
      ->describe_argument("true|false")
      ->add_default("false")
      ->add_help("'true' asyncronous read of input files [default: true].");

  CLIOptions::envvar("CDO_CORESIZE")
      ->add_effect([&](std::string const &envstr) { Options::coresize = parameter_to_long(envstr); })
      ->describe_argument("max. core dump size")
      ->add_help("The largest size (in bytes) core file that may be created.");

  CLIOptions::envvar("CDO_DOWNLOAD_PATH")
      ->add_effect([&](std::string const &downloadPath) { DownloadPath = downloadPath; })
      ->describe_argument("path")
      ->add_help("Path where CDO can store downloads.");

  CLIOptions::envvar("CDO_ICON_GRIDS")
      ->add_effect([&](std::string const &iconGrid) { IconGrids = iconGrid; })
      ->describe_argument("path")
      ->add_help("Root directory of the installed ICON grids (e.g. /pool/data/ICON).");

  CLIOptions::envvar("CDO_DISABLE_HISTORY")
      ->add_effect(
          [&](std::string const &envstr)
          {
            if (parameter_to_bool(envstr) == true)
            {
              Options::CDO_Reset_History = true;
              Options::CDO_Append_History = false;
            }
          })
      ->describe_argument("true|false")
      ->add_help("'true' disables history attribute.");

  CLIOptions::envvar("CDO_RESET_HISTORY")
      ->add_effect([&](std::string const &envstr) { Options::CDO_Reset_History = parameter_to_bool(envstr); })
      ->describe_argument("true|false")
      ->add_default("false")
      ->add_help("'true' resets the global history attribute [default: false].");

  CLIOptions::envvar("CDO_PRINT_FILENAME")
      ->add_effect([&](std::string const &envstr) { Options::PrintFilename = parameter_to_bool(envstr); })
      ->describe_argument("true|false")
      ->add_default("false")
      ->add_help("'true' prints name of all output files [default: false].");

  CLIOptions::envvar("CDO_HISTORY_INFO")
      ->add_effect([&](std::string const &envstr) { Options::CDO_Append_History = parameter_to_bool(envstr); })
      ->describe_argument("true|false")
      ->add_default("true")
      ->add_help("'false' don't write information to the global history attribute [default: true].");

  CLIOptions::envvar("CDO_FILE_SUFFIX")
      ->add_effect(
          [&](std::string const &envstr)
          {
            if (envstr.size()) cdo::FileSuffix = envstr;
          })
      ->describe_argument("suffix")
      ->add_help("Default filename suffix.");

  CLIOptions::envvar("CDO_DISABLE_FILE_SUFFIX")
      ->add_effect(
          [&](std::string const &envstr)
          {
            if (parameter_to_bool(envstr)) cdo::FileSuffix = "NULL";
          })
      ->describe_argument("true|false")
      ->add_help("'true' disables file suffix.");

  CLIOptions::envvar("CDO_VERSION_INFO")
      ->add_effect([&](std::string const &envstr) { Options::VersionInfo = parameter_to_bool(envstr); })
      ->describe_argument("true|false")
      ->add_default("true")
      ->add_help("'false' disables the global NetCDF attribute CDO [default: true].");
}

static const char *
get_progname(char *string)
{
#ifdef _WIN32
  //  progname = strrchr(string, '\\');
  char *progname = " cdo";
#else
  char *progname = strrchr(string, '/');
#endif

  return (progname == nullptr) ? string : ++progname;
}

#ifdef HAVE_H5DONT_ATEXIT
extern "C" void H5dont_atexit(void);
#endif

static void
print_operator_attributes(std::string const &argument)
{
  ModListOptions local_modListOpt;
  local_modListOpt.parse_request(argument);
  operator_print_list(local_modListOpt);
}

static void
cdo_print_debug_info()
{
  fprintf(stderr, "stdinIsTerminal:   %d\n", cdo::stdinIsTerminal);
  fprintf(stderr, "stdoutIsTerminal:  %d\n", cdo::stdoutIsTerminal);
  fprintf(stderr, "stderrIsTerminal:  %d\n", cdo::stderrIsTerminal);
  cdo::features::print_system_info();
  print_pthread_info();
}

static void
create_options_from_envvars()
{
  CLIOptions::option_from_envvar("CDO_VERSION_INFO");
  CLIOptions::option_from_envvar("CDO_DISABLE_FILE_SUFFIX");
  CLIOptions::option_from_envvar("CDO_FILE_SUFFIX");
  CLIOptions::option_from_envvar("CDO_DISABLE_HISTORY")->set_category("History");
  CLIOptions::option_from_envvar("CDO_HISTORY_INFO")->set_category("History");
  CLIOptions::option_from_envvar("CDO_RESET_HISTORY")->set_category("History");
  CLIOptions::option_from_envvar("CDO_DOWNLOAD_PATH");
  CLIOptions::option_from_envvar("CDO_ICON_GRIDS");
  CLIOptions::option_from_envvar("CDO_TEST");
}

static void
setup_cli_options()
{
  CLIOptions::option("attribs")
      ->describe_argument("arbitrary|filesOnly|onlyFirst|noOutput|obase")
      ->aborts_program(true)
      ->set_category("Info")
      ->add_effect([&](std::string const &argument) { print_operator_attributes(argument); })
      ->add_help("Lists all operators with choosen features or the attributes of given operator(s)",
                 "operator name or a combination of [arbitrary,filesOnly,onlyFirst,noOutput,obase].");

  CLIOptions::option("operators")
      ->aborts_program(true)
      ->add_effect([&]() { print_operator_attributes(std::string()); })
      ->set_category("Info")
      ->add_help("Prints list of operators.");

  CLIOptions::option("module_info")
      ->aborts_program(true)
      ->describe_argument("module name")
      ->set_category("Info")
      ->add_effect(
          [&](std::string const &argument)
          {
            auto names = Factory::get_module_operator_names(argument);
            if (names.empty())
            {
              std::string errstr = "Module " + argument + " not found\n";
              std::cerr << errstr;
            }
            else
            {
              std::string info_string = "\n" + argument + ":\n";
              for (auto const &name : names) { info_string += std::string(4, ' ') + name + "\n"; }
              std::cerr << info_string + "\n";
            }
          })
      ->add_help("Prints list of operators.");

  CLIOptions::option("operators_no_output")
      ->aborts_program(true)
      ->add_effect([&]() { print_operator_attributes("noOutput"); })
      ->set_category("Info")
      ->add_help("Prints all operators which produce no output.");

  CLIOptions::option("color")
      ->describe_argument("auto|no|all")
      ->add_effect([&](std::string const &argument) { cdo::evaluate_color_options(argument); })
      ->set_category("Output")
      ->add_help("Set behaviour of colorized output messages.")
      ->shortform('C');

  CLIOptions::option("help")
      ->describe_argument("operator")
      ->add_effect([&](std::string const &operator_name) { cdo_print_help(operator_name); })
      ->on_empty_argument([]() { cdo_usage(stdout); })
      ->aborts_program(true)
      ->set_category("Help")
      ->add_help("Shows either help information for the given operator or the usage of CDO.")
      ->shortform('h');

  CLIOptions::option("overwrite")
      ->add_effect([&]() { Options::cdoOverwriteMode = true; })
      ->add_help("Overwrite existing output file, if checked.")
      ->shortform('O');

  CLIOptions::option("interactive")
      ->add_effect([&]() { Options::cdoInteractive = true; })
      ->add_help("Enable CDO interactive mode.")
      ->shortform('u');

  CLIOptions::option("argument_groups")
      ->aborts_program(true)
      ->add_help("Explanation and Examples for subgrouping operators with [ ] syntax")
      ->add_effect([&]() { cdo_display_syntax_help(Parser::subgroup_help, stderr); })
      ->set_category("Help");

  CLIOptions::option("apply")
      ->aborts_program(true)
      ->add_help("Explanation and Examples for -apply syntax")
      ->add_effect([&]() { cdo_display_syntax_help(Parser::apply_help, stderr); })
      ->set_category("Help");

  CLIOptions::option("dryrun")
      ->add_effect([&]() { applyDryRun = true; })
      ->add_help("Dry run that shows processed CDO call.")
      ->shortform('A');
}

static void
timer_report(std::vector<cdo::iTimer *> &timers)
{
  FILE *fp = stdout;
  if (Options::cdoVerbose) fprintf(fp, "\nTimer report:  shift = %g\n", cdo::timerShift);
  fprintf(fp, "    Name   Calls          Min      Average          Max        Total\n");

  for (auto &timer : timers)
  {
    if (timer->calls > 0)
    {
      auto total = timer->elapsed();
      auto avg = timer->sum;
      avg /= timer->calls;

      // if (timer.stat != rt_stat_undef)
      fprintf(fp, "%8s %7d %12.4g %12.4g %12.4g %12.4g\n", timer->name.c_str(), timer->calls, timer->min, avg, timer->max, total);
    }
  }
}

int
main(int argc, char *argv[])
{
  cdo::set_exit_function(cdo_exit);
  cdo::set_context_function(process_inq_prompt);
  progress::set_context_function(process_inq_prompt);

  mpmo_color_set(Auto);

  cdo_init_is_tty();

  Options::CDO_Reduce_Dim = 0;

  // mallopt(M_MMAP_MAX, 0);

  cdo::set_command_line(argc, argv);

  cdo::progname = get_progname(argv[0]);

  get_env_vars();
  create_options_from_envvars();
  CLIOptions::get_env_vars();

  setup_options();
  setup_cli_options();

  auto CDO_optind = CLIOptions::parse(std::vector<std::string>(argv, argv + argc));

  if (CDO_optind == CLIOptions::ABORT_REQUESTED) exit(EXIT_FAILURE);
  if (CDO_optind == CLIOptions::EXIT_REQUESTED) exit(EXIT_SUCCESS);

  if (CDO_optind >= argc)
  {
    cdo_usage(stderr);
    fprintf(stderr, "\nNo operator given!\n\n");
    exit(EXIT_FAILURE);
  }
  else
  {
    cdo::set_cdi_options();
    cdo::set_external_proj_func();
    cdo::set_stacksize(67108864);  // 64MB
    cdo::set_coresize(Options::coresize);
    cdo::setup_openMP(Threading::ompNumUserRequestedThreads);

    if (cdo::dbg()) cdo_print_debug_info();

    std::vector<std::string> new_argv(&argv[CDO_optind], argv + argc);

    new_argv = expand_wild_cards(new_argv);

    if (CdoDefault::TableID != CDI_UNDEFID) cdo_def_table_id(CdoDefault::TableID);

#ifdef HAVE_H5DONT_ATEXIT
    H5dont_atexit();  // don't call H5close on exit
#endif
#ifdef CUSTOM_MODULES
    load_custom_modules("custom_modules");
    close_library_handles();
#endif

    auto processStructure = Parser::parse(new_argv, process_inq_prompt);
    if (applyDryRun == true)
    {
      std::cerr << processStructure[0]->to_string() << std::endl;
      exit(applyDryRun ? 0 : -1);
    }

    std::vector<cdo::iTimer *> allTimers;
    auto totalTimer = cdo::iTimer("total");
    cdo::readTimer = cdo::iTimer("read");
    cdo::writeTimer = cdo::iTimer("write");
    allTimers.push_back(&totalTimer);
    allTimers.push_back(&cdo::readTimer);
    allTimers.push_back(&cdo::writeTimer);

    g_processManager.buildProcessTree(processStructure);

    FileStream::enableTimers(g_processManager.get_num_processes() == 1 && Threading::ompNumMaxThreads == 1);

    // if (g_processManager.get_num_processes() == 1) { cdiDefGlobal("NETCDF_LAZY_GRID_LOAD", true); }
    totalTimer.start();
    g_processManager.run_processes();
    totalTimer.stop();

    // Flush stdout before cleanup — on Windows with PIPE-connected stdout,
    // cleanup may hang (e.g. atexit handlers in HDF5/NetCDF libraries).
    // Flushing here ensures all operator output (showname, sinfo, etc.)
    // reaches the pipe before any potential hang.
    fflush(stdout);

    g_processManager.clear_processes();

    if (Options::Timer) timer_report(allTimers);
  }

  if (Options::CDO_Rusage) cdo::features::print_rusage();

  return Options::cdoExitStatus;
}
