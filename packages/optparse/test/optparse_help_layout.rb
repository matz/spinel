# A names column wider than the summary width puts the description on the
# next line. A switch with no description has no trailing spaces. Each
# extra description string is one more line.
require "optparse"

parser = OptionParser.new("Usage: tool [OPTIONS]", 20, "  ")
parser.on("--a-very-long-switch-name=VALUE", "Goes on the next line") {}
parser.on("--nodesc") {}
parser.on("-d", "--desc", "First line", "Second line") {}
parser.to_s.each_line { |line| p line }
