# A names column exactly as wide as the summary width keeps the
# description on the same line. One more character moves it down.
require "optparse"

parser = OptionParser.new("Usage: tool", 10, "")
parser.on("--abcd", "Fits") {}
parser.on("--abcde", "Wraps") {}
parser.to_s.each_line { |line| p line }
