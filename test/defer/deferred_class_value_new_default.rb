# Compiled with --defer-refusals: `k.new` on a Class value has an arm per
# class it may hold, and each fills its initialize's defaults. A default
# the compiler refuses raises NotImplementedError in its own class's arm
# only: the call on another class constructs, and the statements after it
# run. Before, the refusal took the whole statement, and the ones after it.
# spinel: defer-refusals: 2:Plain [true, false] NotImplementedError done
class Plain
  def initialize(escape: true) = @escape = escape
  def escape = @escape
end

class Wide
  def initialize(width: proc { "a" }.ruby2_keywords) = @width = width
end

class Holder
  def self.use(k)
    a = k.new
    b = k.new(escape: false)
    puts a.class
    p [a.escape, b.escape]
  end
end

Holder.use([Plain, Wide][ARGV.size])
begin
  Holder.use([Wide, Plain][ARGV.size])
rescue NotImplementedError => e
  puts e.class
end
puts "done"
proc { "a" }.ruby2_keywords
puts "not reached"
