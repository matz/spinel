# A class or module whose own name holds "__" prints that name: M::Foo__Bar
# is "M::Foo__Bar", not "M::Bar" (Spinel keys a nested class as
# M__Foo__Bar, and the name was taken from the last part of that key).
module M
  class Foo__Bar; end
  class A__B__C; class D__E; end; end
  Built__X = Class.new
  Str__Y = Struct.new(:a)
  module Mod__Z; end
  class Err__Q < StandardError; end
  class Sub__S < Foo__Bar; end
end
class M::Path__P; end
module N; class Foo; end; end
module O; class Foo; end; class Two__Foo; end; end
p M::Foo__Bar, M::Foo__Bar.name, M::Foo__Bar.to_s, M::Foo__Bar.inspect
p M::A__B__C, M::A__B__C::D__E, M::A__B__C::D__E.name
p M::Built__X, M::Str__Y, M::Str__Y.new(1), M::Mod__Z
p M::Path__P, N::Foo, O::Foo, O::Two__Foo
puts M::Foo__Bar.new.inspect.sub(/0x\h+/, "X")
p M::Sub__S.superclass, M::Sub__S.ancestors.first(2)
begin; raise M::Err__Q, "boom"; rescue => e; p e.class, e; end
x = [M::Foo__Bar.new, 1].first
p x.class
begin; M::Foo__Bar.new.nope; rescue NoMethodError => e; puts e.message; end

module Late; module Q__R; Foo = 3 if ARGV.size > 5; end; end
w = Late::Q__R
begin; p w::Foo; rescue NameError => e; puts e.message; end

# the same leaf under two modules (Spinel renames both apart)
module CA; class Leaf__X; end; Two__S = Struct.new(:x); end
module CB; class Leaf__X; end; Two__S = Struct.new(:y); end
p CA::Leaf__X, CB::Leaf__X, CA::Two__S.new(1), CB::Two__S.new(2)
module CC; class Array; end; class Monitor__M; end; end
p CC::Array, CC::Monitor__M

# `w::Bar` through a module value does not find M::Foo__Bar
module DM; class Foo__Bar; def hi = 1; end; end
module DN; class Bar; def hi = 2; end; end
module DO; class Bar; def hi = 3; end; end
def dyn_hi(w) = w::Bar.new.hi
p dyn_hi(DN), dyn_hi(DO)
begin; p dyn_hi(DM); rescue NameError => e; puts e.message; end
[DN, DO, DM].each do |w|
  begin; p w::Bar; rescue NameError => e; puts e.message; end
end

# one other class of the leaf: a module without it raises NameError
module EX; class Foo__Bar; end; end
module EY; class Bar; end; end
module EZ; end
[EY, EX, EZ].each do |w|
  begin; p w::Bar; rescue NameError => e; puts e.message; end
end
# a builder written through a path
module PA; end
PA::Foo__Q = Class.new do; end
p PA::Foo__Q, PA::Foo__Q.name
# two modules each with a Foo__Bar, beside one other Bar
module FX; class Foo__Bar; end; end
module FZ; class Foo__Bar; end; end
module FY; class Bar; end; end
[FY, FX, FZ].each do |w|
  begin; p w::Bar; rescue NameError => e; puts e.message; end
end
# only keys end in the leaf: no class arm; Object gives the builtin
module GA; class Foo__String; end; end
module GB; class Other__String; end; end
class GK; end
[Object, GA, GB, GK].each do |w|
  begin; p w::String; rescue NameError => e; puts e.message; end
end
