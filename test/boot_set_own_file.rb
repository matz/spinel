# A program's own file named set/set.rb is not the Set CRuby loads at boot:
# it reopens Set, and the program's `require "set"` after it loads nothing.
require_relative "boot_set_own_file_lib/set/set"
require "set"
p Set[1, 2].size
