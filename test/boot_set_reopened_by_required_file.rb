# A required file that reopens Set is read before the program's own
# `require "set"`, which loads nothing: Set was there from the start.
require_relative "boot_set_reopened_by_required_file_lib/ext"
require "set"
p Set[1, 2].size
