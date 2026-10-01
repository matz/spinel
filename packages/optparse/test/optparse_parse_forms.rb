# parse! reads "-sVALUE", "--long VALUE" and "--", and returns argv. parse
# works on a copy. help is the same text as to_s.
require "optparse"

seen = []
parser = OptionParser.new("Usage: tool [OPTIONS]")
parser.on("-v", "Verbose") { |v| seen << "v:#{v}" }
parser.on("-n NAME", "Name") { |v| seen << "n:#{v}" }
parser.on("--list=A,B", Array, "List") { |v| seen << v.join("+") }
argv = ["-v", "-nfoo", "x", "--list", "a,b", "--", "-v", "--zzz"]
result = parser.parse!(argv)
p seen
p argv
p result.equal?(argv)

original = ["-n", "q", "keep"]
copy = parser.parse(original)
p copy
p original

puts parser.help
p parser.help == parser.to_s

begin
  parser.parse!(["-n"])
rescue OptionParser::MissingArgument => e
  p e.message
end
begin
  parser.parse!(["-z"])
rescue OptionParser::InvalidOption => e
  p e.message
end
