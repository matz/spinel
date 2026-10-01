# on_tail registers a switch like `on`, listed after the others in the help.
require "optparse"

calls = []
parser = OptionParser.new
parser.on_tail("-h", "--help", "Show this help message") { calls << "help" }
parser.parse!(["-h"])
parser.parse!(["--help"])
p calls
