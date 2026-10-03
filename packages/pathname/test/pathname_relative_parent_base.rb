require "pathname"

def show_relative(path, base)
  puts Pathname.new(path).relative_path_from(Pathname.new(base)).to_s
rescue ArgumentError
  puts "ArgumentError"
end

# A remaining `..` in the base cannot be turned into a relative route to
# the destination. A shared leading `..`, or one removed by cleanpath, can.
show_relative("a", "../b")
show_relative("a", "x/../../b")
show_relative(".", "..")
show_relative("../a", "../b")
show_relative("a", "b/..")
show_relative("../a", "b")
