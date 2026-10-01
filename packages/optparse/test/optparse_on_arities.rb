# `on` takes any number of switch names and a description, in any order.
# A short switch can take its value as the next word, a long switch as
# `=VALUE` or as the next word, and aliases share one handler.
require "optparse"

seen = []
parser = OptionParser.new
parser.on("--public-only", "Searches only public repositories") { seen << "public" }
parser.on("--repo=USER/REPO", "--repository=USER/REPO", "Searches one repository") { |v| seen << "repo:#{v}" }
parser.on("-u NAME", "--user=NAME", "Sets the user") { |v| seen << "user:#{v}" }
parser.on("--max-days NUMBER", "Sets the maximum days") { |v| seen << "max:#{v}" }

argv = ["ruby", "--public-only", "--repo=a/b", "--repository", "c/d", "-u", "me", "--user=you", "--max-days", "30", "extra"]
parser.parse!(argv)
p seen
p argv
