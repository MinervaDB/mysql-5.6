/*
   Copyright (c) 2014, SkySQL Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

/* This C++ files header file */
#include "./rdb_cf_options.h"

/* C++ system header files */
#include <string>

/* MySQL header files */
#include "./log.h"

/* RocksDB header files */
#include "rocksdb/utilities/convenience.h"

void Cf_options::Get(const std::string &cf_name,
                     rocksdb::ColumnFamilyOptions *opts) {

  // set defaults
  rocksdb::GetColumnFamilyOptionsFromString(*opts,
                                            default_config_,
                                            opts);

  // set per-cf config if we have one
  NameToConfig::iterator it = name_map_.find(cf_name);
  if (it != name_map_.end()) {
    rocksdb::GetColumnFamilyOptionsFromString(*opts,
                                              it->second,
                                              opts);
  }
}

bool Cf_options::SetDefault(const std::string &default_config) {
  rocksdb::ColumnFamilyOptions options;

  if (!default_config.empty() &&
      !rocksdb::GetColumnFamilyOptionsFromString(options,
                                                 default_config,
                                                 &options).ok()) {
    fprintf(stderr,
            "Invalid default column family config: %s\n",
            default_config.c_str());
    return false;
  }

  default_config_ = default_config;
  return true;
}

// Skip over any spaces in the input string.
static void skip_spaces(const std::string& input, size_t* pos)
{
  while (*pos < input.size() && isspace(input[*pos]))
    ++(*pos);
}

// Find a valid column family name.  Note that all characters except a
// semicolon are valid (should this change?) and all spaces are trimmed from
// the beginning and end but are not removed between other characters.
static bool find_column_family(const std::string& input, size_t* pos,
                              std::string* key)
{
  size_t beg_pos = *pos;
  size_t end_pos = *pos - 1;

  // Loop through the characters in the string until we see a '='.
  for ( ; *pos < input.size() && input[*pos] != '='; ++(*pos))
  {
    // If this is not a space, move the end position to the current position.
    if (input[*pos] != ' ')
      end_pos = *pos;
  }

  if (end_pos == beg_pos - 1)
  {
    // NO_LINT_DEBUG
    sql_print_warning("No column family found (options: %s)", input.c_str());
    return false;
  }

  *key = input.substr(beg_pos, end_pos - beg_pos + 1);
  return true;
}

// Find a valid options portion.  Everything is deemed valid within the options
// portion until we hit as many close curly braces as we have seen open curly
// braces.
static bool find_options(const std::string& input, size_t* pos,
                         std::string* options)
{
  // Make sure we have an open curly brace at the current position.
  if (*pos < input.size() && input[*pos] != '{')
  {
    // NO_LINT_DEBUG
    sql_print_warning("Invalid cf options, '{' expected (options: %s)",
        input.c_str());
    return false;
  }

  // Skip the open curly brace and any spaces.
  ++(*pos);
  skip_spaces(input, pos);

  // Set up our brace_count, the begin position and current end position.
  size_t brace_count = 1;
  size_t beg_pos = *pos;

  // Loop through the characters in the string until we find the appropriate
  // number of closing curly braces.
  while (*pos < input.size())
  {
    switch (input[*pos])
    {
      case '}':
        // If this is a closing curly brace and we bring the count down to zero
        // we can exit the loop with a valid options string.
        if (--brace_count == 0)
        {
          *options = input.substr(beg_pos, *pos - beg_pos);
          ++(*pos);  // Move past the last closing curly brace
          return true;
        }

        break;

      case '{':
        // If this is an open curly brace increment the count.
        ++brace_count;
        break;

      default:
        break;
    }

    // Move to the next character.
    ++(*pos);
  }

  // We never found the correct number of closing curly braces.
  // Generate an error.
  // NO_LINT_DEBUG
  sql_print_warning("Mismatched cf options, '}' expected (options: %s)",
      input.c_str());
  return false;
}

static bool find_cf_options_pair(const std::string& input, size_t* pos,
                                 std::string* cf, std::string* opt_str)
{
  // Skip any spaces.
  skip_spaces(input, pos);

  // We should now have a column family name.
  if (!find_column_family(input, pos, cf))
    return false;

  // If we are at the end of the input then we generate an error.
  if (*pos == input.size())
  {
    // NO_LINT_DEBUG
    sql_print_warning("Invalid cf options, '=' expected (options: %s)",
        input.c_str());
    return false;
  }

  // Skip equal sign and any spaces after it
  ++(*pos);
  skip_spaces(input, pos);

  // Find the options for this column family.  This should be in the format
  // {<options>} where <options> may contain embedded pairs of curly braces.
  if (!find_options(input, pos, opt_str))
    return false;

  // Skip any trailing spaces after the option string.
  skip_spaces(input, pos);

  // We should either be at the end of the input string or at a semicolon.
  if (*pos < input.size())
  {
    if (input[*pos] != ';')
    {
      // NO_LINT_DEBUG
      sql_print_warning("Invalid cf options, ';' expected (options: %s)",
          input.c_str());
      return false;
    }

    ++(*pos);
  }

  return true;
}

bool Cf_options::SetOverride(const std::string &override_config)
{
  // TODO(???): support updates?

  std::string cf;
  std::string opt_str;
  rocksdb::ColumnFamilyOptions options;
  NameToConfig configs;

  // Loop through the characters of the string until we reach the end.
  size_t pos = 0;
  while (pos < override_config.size())
  {
    // Attempt to find <cf>={<opt_str>}.
    if (!find_cf_options_pair(override_config, &pos, &cf, &opt_str))
      return false;

    // Generate an error if we have already seen this column family.
    if (configs.find(cf) != configs.end())
    {
      // NO_LINT_DEBUG
      sql_print_warning(
          "Duplicate entry for %s in override options (options: %s)",
          cf.c_str(), override_config.c_str());
      return false;
    }

    // Generate an error if the <opt_str> is not valid according to RocksDB.
    if (!rocksdb::GetColumnFamilyOptionsFromString(
          options, opt_str, &options).ok())
    {
      // NO_LINT_DEBUG
      sql_print_warning(
          "Invalid cf config for %s in override options (options: %s)",
          cf.c_str(), override_config.c_str());
      return false;
    }

    // If everything is good, add this cf/opt_str pair to the map.
    configs[cf] = opt_str;
  }

  // Everything checked out - make the map live
  name_map_ = configs;

  return true;
}