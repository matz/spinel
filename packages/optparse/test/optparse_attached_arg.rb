# A switch name can carry its value placeholder with no separator, as in
# "-nNAME". The switch is "-n" and it takes a value.
require "optparse"

name = nil
parser = OptionParser.new("Usage: tool [OPTIONS]")
parser.on("-nNAME", "--name=NAME", "Sets the name") { |v| name = v }
parser.parse!(["-nbob"])
p name
parser.parse!(["-n", "amy"])
p name
puts parser
