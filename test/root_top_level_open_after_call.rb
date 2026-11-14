# Kernel#open and Kernel#puts called ahead of a top-level def of the same
# name are the builtins; the calls after the def run the def's.
require "tmpdir"
path = File.join(Dir.tmpdir, "root_open_#{$$}.txt")
File.write(path, "hi")
v = open(path) { |f| f.read }
p v
def open(*a) = :my_open
p open(path)
def puts(*a) = $stdout.write("MY #{a.inspect}\n")
puts 1
print "x\n"
File.delete(path)
