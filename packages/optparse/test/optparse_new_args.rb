# OptionParser.new takes a banner, a summary width and a summary indent,
# and yields the new parser to a block.
require "optparse"

yielded = nil
parser = OptionParser.new("Usage: tool [OPTIONS]", 20, "    ") do |opts|
  yielded = opts
end
p yielded.equal?(parser)
p parser.banner
p parser.summary_width
p parser.summary_indent
