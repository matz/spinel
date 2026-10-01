# The Array type splits the value on commas, and the block receives an Array.
require "optparse"

got = nil
parser = OptionParser.new
parser.on("--exclude=NAME,NAME2", Array, "Exclude repositories") { |v| got = v }
argv = ["--exclude=foo,bar,baz"]
parser.parse!(argv)
p got
p got.first(2)
