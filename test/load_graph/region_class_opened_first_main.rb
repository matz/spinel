# A region may open a class before region 0's first header of it: here a
# file required from a method body opens Foo empty, binds Bar by a builder,
# makes an instance of each and calls the methods below, all before the
# headers that give the classes methods. No call may be bound to what those
# headers add, as if the classes could not be seen before them.
def fx = (Foo.x rescue :none)
def fy = ($f.y rescue :none)
def bx = (Bar.w rescue :none)
def by = ($b.z rescue :none)
def go = require_relative("region_class_opened_first_lib")
go
p fx, fy, bx, by
class Foo; def self.x = 1; def y = 2; end
class Bar; def self.w = 3; def z = 4; end
p fx, fy, bx, by
