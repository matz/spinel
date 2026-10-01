# A value placeholder on one switch name applies to all the names of that
# switch, also when a later name has no placeholder.
require "optparse"

got = []
parser = OptionParser.new("Usage: tool")
parser.on("-u NAME", "--user", "User") { |v| got << v }
parser.on("--repo", "-r REPO", "Repo") { |v| got << v }
argv = ["-u", "bob", "--user", "amy", "--repo", "a/b", "keep"]
parser.parse!(argv)
p got
p argv
puts parser
