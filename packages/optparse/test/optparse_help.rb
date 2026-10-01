# to_s renders the banner and one line per switch, with the description
# aligned at the summary width, and the on_tail switches last.
require "optparse"

parser = OptionParser.new("", 35, "  ") do |opts|
  opts.on_tail("-h", "--help", "Show this help message") {}
end
parser.banner = "Usage: tool scan PRODUCT [OPTIONS]"
parser.on("--exclude=NAME,NAME2", Array, "Exclude repositories") {}
parser.on("--public-only", "Searches only public repositories") {}
parser.on("-u NAME", "--user=NAME", "Sets the user") {}
parser.separator ""
parser.separator "Other:"
parser.on("--max-eol-days-away NUMBER", "Sets the maximum number of days away") {}
puts parser
puts "--"
puts parser.to_s
