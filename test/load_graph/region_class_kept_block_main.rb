# A class a kept block makes before its first header, in a program of
# several regions: Foo's body runs in a proc and Bar is written in a
# lambda, both called before the headers below give the classes methods.
# Their openings with no point (the proc's) or none (the lambda's write)
# do not bound the classes, so no call before the headers may be bound to
# what they add.
def fy = ($f.y rescue :none)
def bz = ($b.z rescue :none)
def bx = (Bar.x rescue :none)
def go = require_relative("region_class_kept_block_lib")
blk = proc { class Foo; end }
blk.call
lam = -> { Bar = Class.new; nil }
lam.()
$f = Foo.new
$b = Bar.new
p fy, bz, bx
class Foo; def y = 1; end
class Bar; def z = 2; def self.x = 3; end
p fy, bz, bx
go
