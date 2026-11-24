# A class_exec / module_exec block binds its parameters from the call's
# arguments one to one: a literal splat spreads first, so `*[x]` binds x and
# not the Array.
class Box; end
module Mod; end

s = +"a"
Box.class_exec(*[s]) { |t| t << "!" }
p s
p Box.class_exec(*[1]) { |t| t + 1 }
p Box.class_exec(*[2, 3]) { |a, b| a + b }
p Mod.module_exec(*[4]) { |t| t * 2 }
p Box.class_exec(1, *[2]) { |a, b| [a, b] }
p Box.class_exec(*[]) { 7 }

# A keyword hash binds a block's positional parameter, in a class body too.
p Box.class_exec(k: 1) { |h| h }
class Box
  class_exec(1, k: 2) { |a, h| p [a, h] }
end

# A literal splat in a class body binds as it does anywhere else.
class Box
  class_exec(*[5]) { |t| define_method(:five) { t } }
  class_exec(*[6, 7]) { |a, b| define_method(:thirteen) { a + b } }
end
p Box.new.five, Box.new.thirteen
