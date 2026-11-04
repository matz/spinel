# `v::Foo` with v a run-time value and Foo in more than one scope: a value
# that is no class or module raises CRuby's TypeError, and a module that
# holds no Foo raises NameError (Object's message names Foo alone).
module Ext
  class Foo; def hi = "ext"; end
  module Nest; class Foo; def hi = "nested"; end; end
end
def foo(v) = v::Foo
puts foo(Ext).new.hi
puts foo(Ext::Nest).new.hi
[7, "s", nil, :sym, [1]].each do |v|
  begin
    foo(v)
  rescue TypeError => e
    puts "TypeError: #{e.message}"
  end
end
[Object, Comparable].each do |v|
  begin
    foo(v)
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
end
# A parameter that only ever holds an Integer
def int_foo(n) = n::Foo
begin
  int_foo(5)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
