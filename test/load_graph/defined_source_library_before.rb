# defined?(ERB::Escape) asks for the library's constant at the guard's point.
# A guard that runs before the library's require finds none (the library's
# class is defined further down the program), and a BEGIN block runs ahead
# of the whole program, its guard before the require too, whatever its place
# in the text. A class that raises before it defines its nested class never
# defines it.
if defined?(ERB::Escape)
  def early_h = "then"
else
  def early_h = "else"
end
early_x = defined?(ERB::Escape) ? 1 : 2
BEGIN { $begin_x = defined?(ERB::Escape) ? "then" : "else" }

begin
  module Test
    raise "boom"
    class Foo; end
  end
rescue => e
  p e.message
end
p(defined?(Test::Foo) ? "then" : "else")

p early_h
p early_x
p $begin_x

require "erb"
p(defined?(ERB::Escape) ? "then" : "else")
