# When parse! raises, the switches it read before the error are already
# gone from argv.
require "optparse"

parser = OptionParser.new
parser.on("-v", "--verbose", "Verbose") {}
parser.on("--repo=USER/REPO", "Searches one repository") {}

argv = ["-v", "keep", "--nope", "after"]
begin
  parser.parse!(argv)
rescue OptionParser::ParseError => e
  p e.message
end
p argv

argv = ["keep", "--verbose", "--repo"]
begin
  parser.parse!(argv)
rescue OptionParser::ParseError => e
  p e.message
end
p argv
