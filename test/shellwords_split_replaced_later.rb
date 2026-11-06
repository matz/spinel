# String#shellsplit, which shellwords adds, calls Shellwords.split, an alias in
# `class << self` of a module_function copy made by name; the program
# replaces split after the first call, and the call in shellsplit follows.
require "shellwords"
p "a b 'c d'".shellsplit
module Shellwords
  def self.split(s) = ["mine"]
end
p "a b".shellsplit
p Shellwords.shellwords("x y")
