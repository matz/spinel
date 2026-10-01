# An unknown switch raises InvalidOption, a switch without its value
# raises MissingArgument, and a value on a switch that takes none raises
# NeedlessArgument. All are ParseErrors with a readable message.
require "optparse"

parser = OptionParser.new
parser.on("--repo=USER/REPO", "Searches one repository") {}

begin
  parser.parse!(["--nope"])
rescue OptionParser::ParseError => e
  p e.class
  p e.message
  p e.message.capitalize
end

begin
  parser.parse!(["--repo"])
rescue OptionParser::ParseError => e
  p e.class
  p e.message
end

parser.on("--public-only", "Searches only public repositories") {}
begin
  parser.parse!(["--public-only=x"])
rescue OptionParser::NeedlessArgument => e
  p e.class
  p e.message
end
