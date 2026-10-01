# A typed File/IO's each_line / each / each_char yield Strings and its
# each_byte / each_codepoint Integers, and the block's body is inferred against
# that: an array the block pushes them into is a String (Integer) array, not a
# boxed one, whether it is pushed directly, through a local, or transformed.
require "tmpdir"
path = File.join(Dir.tmpdir, "sp_io_each_block_param_typed_#{Process.pid}.txt")
File.write(path, "one\ntwé\nthree\n")

lines = []
File.open(path) { |f| f.each_line { |line| lines << line } }
p lines

viaeach = []
File.open(path) { |f| f.each { |line| s = line; viaeach << s } }
p viaeach

chomped = []
File.open(path) { |f| f.each_line { |line| chomped.push(line.chomp) } }
p chomped

chars = []
File.open(path) { |f| f.each_char { |c| chars << c } }
p chars.length

bytes = []
File.open(path) { |f| f.each_byte { |b| bytes << b } }
p bytes.sum

cps = []
File.open(path) { |f| f.each_codepoint { |cp| cps << cp } }
p cps.sum

File.delete(path)
