# A program's own then, yield_self or tap (on Object, Kernel or a class)
# answers in its own form, nil included, wherever the call reaches it: only
# Kernel's then answers its block's value, and Kernel's tap its receiver.
class Object
  def then = $v
  def tap = $v
end
module Kernel
  def yield_self = $w
end
class Foo
  def then = $f
end
$v = nil
$w = nil
$f = nil
p(5.then { 42 })
x = 5.then { 42 }
p x.nil?
p(2.5.then { 4.5 })
p(nil.then { 42 })
p(5.tap { 42 })
p(5.yield_self { 42 })
p(Foo.new.then { 42 })
[Foo.new, 5].each { |o| p(o.then { 7 }) }
$v = 3
$w = 4
$f = 9
p(5.then { 42 } + 1)
p(5.tap { 42 } + 1)
p(5.yield_self { 42 } + 1)
p(Foo.new.then { 42 } + 1)
[Foo.new, 5].each { |o| p(o.then { nil }) }
