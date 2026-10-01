# `x.sync = v` where x holds either a program object with an attr writer
# of that name or a real IO. The boxed-IO arm stepped in whenever a class
# had a sync method or reader, but not for an attr writer, so the Log raised
# NoMethodError; and the program's writer arms had none for the stream.
class Log
  attr_accessor :sync
end

[Log.new, $stdout].each do |o|
  p(o.sync = true)
  p o.sync
end
x = [Log.new, $stdout][ARGV.size + 1]
x.sync = false
p x.sync
