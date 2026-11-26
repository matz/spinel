# A call that a class with method_missing may take is split on a test of
# the receiver's class. Date's String calls run before the file that
# defines those classes loads, and a test of a class not yet defined is
# false.
require "date"
def load_file(path) = require(path)
require_relative "method_missing_split_unloaded_class/proxy"
p MSU.make.getbyte(0)
p Date.new(2026, 11, 25).strftime("%b %a")
p Date.strptime("Nov 25", "%b %d").mon
