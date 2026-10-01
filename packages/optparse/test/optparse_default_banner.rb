# With no banner, OptionParser names the program by its file name (CRuby
# 4.0 strips only an executable extension, which Linux has none of).
require "optparse"

parser = OptionParser.new
p parser.banner == "Usage: #{File.basename($0)} [options]"
p parser.to_s.start_with?("Usage: ")
