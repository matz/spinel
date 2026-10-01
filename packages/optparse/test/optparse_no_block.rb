# A switch declared without a block still reads its value and leaves argv
# without it.
require "optparse"

parser = OptionParser.new
parser.on("--repo=REPO", "Repo")
parser.on("-v", "Verbose")
argv = ["--repo=x", "-v", "--repo", "y", "keep"]
parser.parse!(argv)
p argv
