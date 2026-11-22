# puts(*items) converts an element with its to_s before the write, so a
# to_s that freezes STDOUT makes the write raise FrozenError
out = STDOUT.dup
class Freezer
  def to_s
    STDOUT.freeze
    "z"
  end
end
items = [Freezer.new]
begin
  puts(*items)
  out.puts "wrote"
rescue FrozenError => e
  out.puts e.message
end
