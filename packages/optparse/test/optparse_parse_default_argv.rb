# parse! with no argument reads ARGV, as in CRuby: the usual
# `OptionParser.new { ... }.parse!` of a command-line tool.
require "optparse"

verbose = false
out = nil
parser = OptionParser.new do |o|
  o.on("-o FILE", "output") { |f| out = f }
  o.on("-v") { verbose = true }
end
ARGV.concat(["-v", "-o", "x", "y"])
p parser.parse(["-v", "z"])
p ARGV
parser.parse!
p [verbose, out, ARGV]
