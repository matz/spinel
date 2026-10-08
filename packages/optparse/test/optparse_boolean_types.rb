# A TrueClass or FalseClass switch reads yes, true and + as true and no,
# false, - and nil as false, each shortened as far as it stays clear; case
# counts. A word that matches none raises InvalidArgument, and an empty one
# matches words of both values and raises AmbiguousArgument. Without a
# value, "[=VALUE]" passes true for TrueClass and false for FalseClass,
# and a FalseClass flag passes false in both of its forms (#8059).
require "optparse"

parser = OptionParser.new("Usage: tool [OPTIONS]")
parser.on("-c", "--color=WHEN", TrueClass) { |v| p [:color, v] }
parser.on("-f", "--[no-]force[=YES]", TrueClass) { |v| p [:force, v] }
parser.on("-k[KEEP]", "--keep[=KEEP]", FalseClass) { |v| p [:keep, v] }
parser.on("--[no-]quiet", FalseClass) { |v| p [:quiet, v] }
parser.on("--[no-]loud", TrueClass) { |v| p [:loud, v] }
parser.on("--wait [YES]", TrueClass) { |v| p [:wait, v] }

p parser.parse(%w[--color=yes --color=no --color true --color=false --color=+ --color=-])
p parser.parse(%w[--color=nil --color=y --color=tr -cn -c ni --color=fa])
p parser.parse(%w[--force --no-force --force=no -f -fyes])
p parser.parse(%w[--keep --keep=yes -k -kt])
p parser.parse(%w[--quiet --no-quiet --loud --no-loud])
p parser.parse(%w[--wait --wait no --wait -c yes])

[
  ["--color=YES"],
  ["--color=maybe", "b"],
  ["--color", "yess", "b"],
  ["-ce"],
  ["--color="],
  ["--force=", "b"],
  ["--color"]
].each do |words|
  argv = words.dup
  begin
    parser.parse!(argv)
  rescue OptionParser::ParseError => e
    p [e.class, e.message, argv, e.is_a?(OptionParser::InvalidArgument)]
  end
end
