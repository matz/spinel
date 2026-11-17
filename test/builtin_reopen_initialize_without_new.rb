# A program initialize on a builtin class that nothing constructs through
# `new` is never run by CRuby either: the literals below build their values
# without it. Such a program compiles and answers as CRuby does, beside the
# refusal of one that does call `new` (test/reject/builtin_prepended_initialize_new.rb).
module Loud
  def initialize(*a)
    $x = a
    super
  end
end
Array.prepend(Loud)
class Hash
  def initialize(*a, &b)
    $y = 1
    super
  end
end
class String
  def initialize(*a)
    $w = 1
    super
  end
end
p [1, 2].size, $x
p({ a: 1 }, $y)
p "ab" + "c", $w
p [3, 4].map { |v| v + 1 }
