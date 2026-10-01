# A value attached with "=" to a short switch that takes none raises
# NeedlessArgument, as it does for a long switch.
require "optparse"

parser = OptionParser.new
parser.on("-v", "--verbose", "Verbose") {}
begin
  parser.parse!(["-v=3"])
rescue OptionParser::NeedlessArgument => e
  p e.class
  p e.message
end
