# Short switches can share one dash: "-vq" is "-v -q". A switch that takes
# a value uses the rest of the word, or the next word when nothing is left.
# A letter that names no switch raises InvalidOption for that letter.
require "optparse"

seen = []
parser = OptionParser.new
parser.on("-v", "Verbose") { seen << "v" }
parser.on("-q", "Quiet") { seen << "q" }
parser.on("-u NAME", "User") { |v| seen << "u:#{v}" }

argv = ["-vq", "-vubob", "-qu", "amy", "keep"]
parser.parse!(argv)
p seen
p argv

seen.clear
begin
  parser.parse!(["-vx"])
rescue OptionParser::InvalidOption => e
  p seen
  p e.message
end

begin
  parser.parse!(["-v="])
rescue OptionParser::NeedlessArgument => e
  p e.message
end
