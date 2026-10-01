# An error in a cluster of short switches names the rest of the word from
# the letter that failed, not only that letter.
require "optparse"

parser = OptionParser.new
parser.on("-v", "Verbose") {}
["-xfoo", "-vxq", "-vv=1"].each do |word|
  begin
    parser.parse!([word])
  rescue OptionParser::ParseError => e
    p [e.class, e.message]
  end
end
