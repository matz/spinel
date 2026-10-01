# Words that only look like switches stay positional: a lone "-" and a
# word with a dash inside it. A long switch can have one letter, and the
# value of a switch can be the last word or one attached letter.
require "optparse"

seen = []
parser = OptionParser.new
parser.on("--x", "One letter") { seen << "x" }
parser.on("--repo=REPO", "Repo") { |v| seen << "repo:#{v}" }
parser.on("-u NAME", "User") { |v| seen << "u:#{v}" }

argv = ["-", "a-b", "--x", "-uX", "--repo", "last"]
parser.parse!(argv)
p seen
p argv
