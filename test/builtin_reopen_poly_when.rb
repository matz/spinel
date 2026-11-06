# A `when` on a builtin class the program reopens (`class Struct; def m;
# end`) asks the runtime for a boxed value, as is_a? does. The reopening's
# class entry is not the class a boxed Struct, exception or Object carries.
require "set"
class Dir; def zz = 1; end
class Proc; def zz = 1; end
class Regexp; def zz = 1; end
class Struct; def zz = 1; end
class StandardError; def zz = 1; end
class Set; def zz = 1; end
class Object; def oo = 1; end
class MyErr < StandardError; end
class MySet < Set; end
Pt = Struct.new(:a)
class Pt2 < Struct.new(:b); end
w = [Dir.new("."), nil, 3, proc { 1 }, /a/, Pt.new(1), Pt2.new(2), MyErr.new("x"), RuntimeError.new("y"), Set[1], MySet[2], Object.new]
p w.map { |v| case v when Dir then 2 when Proc then 3 when Regexp then 4 when Struct then 5 when StandardError then 6 when Set then 7 else 0 end }
p w.map { |v| case v when MyErr then 1 when MySet then 2 when Pt then 3 when Object then 4 else 0 end }
p w.map { |v| [v.is_a?(Struct), v.is_a?(StandardError), v.is_a?(Object)] }
x = w[ARGV.size + 5]
p(case x when Struct then 1 else 0 end)
