# A class body's loop over a constant (or literal) Hash or Array of literals
# that defines methods: each entry's methods, a nested loop's too.
class String
  COLORS = { red: 31, green: 32 }.freeze

  COLORS.each do |color, code|
    define_method(color) { "\e[#{code}m#{self}\e[0m" }
    define_method("on_#{color}") { "\e[#{code + 10}m#{self}\e[0m" }
  end
end
p "a".red, "b".on_green

class Foo1
  COLORS = { red: 31, green: 32 }
  COLORS.each do |color, code|
    define_method(color) { "#{code}:#{color}" }
  end
end
p Foo1.new.red, Foo1.new.green

class Foo2
  { red: 31, green: 32 }.each do |color, code|
    define_method(color) { code }
  end
end
p Foo2.new.red

class Paint
  COLORS = { "red" => 31, "green" => 32 }.freeze
  EXTRAS = [:bold, :dim]
  COLORS.each do |fg, fc|
    define_method(fg) { "#{fc}" }
    COLORS.each do |bg, bc|
      define_method("#{fg}_on_#{bg}") { "#{fc};#{bc + 10}" }
    end
  end
  EXTRAS.each do |e|
    define_method(e) { "<#{e}>" }
    define_method("#{e}!") { "<#{e}!>" }
  end
end
x = Paint.new
p x.red, x.green_on_red, x.red_on_red, x.bold, x.dim!
p Paint::COLORS.keys, Paint::EXTRAS
