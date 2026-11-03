# spinel: int64
# a class, module or singleton class body answers its last statement's
# value: a def's name, nil when empty, a nested body's value, or the value
# of an expression run in the body

x = class CbA; def k = 1; end
p x
p(module CbM; def k = 1; end)
class CbP; end
p(class << CbP; def k = 2; end)
p((class CbE; end), (module CbN; end), (class << CbP; end))
p((class CbX; 1 + 2; end), (module CbY; "s" * 2; end))
p(class CbS; self; end)
p(class CbT; self.name; end)
p(class CbBig; 2**40; end)
z = (class CbQ; def k = 1; end)
w = (class << CbP; self; end)
p z, w == CbP.singleton_class

# a nested body's value, and the nested class it defines
y = class CbO
  class CbI
    def k = 7
  end
end
p y, CbO::CbI.new.k

# the value of a method, a block or a lambda that ends in one
def cb_install
  class << CbP
    def j = :j
  end
end
p cb_install, CbP.j
def cb_count
  class << CbP
    [1, 2].sum
  end
end
p cb_count
p([1, 2].map { |i| class << CbP; def m = i; end })
f = -> { module CbM; def self.n = 3; end }
p f.call, CbM.n

class CbU
  def self.install
    class << self
      def ku = :ku
    end
  end
end
p CbU.install, CbU.ku

# a method whose value nobody takes keeps its body's defs
def cb_setup
  class << CbP
    def q = :q
  end
end
cb_setup
p CbP.q

# the value leaves of a conditional last statement answer: a path with no
# leaf answers nil, and every run starts afresh
def cb_pick(n)
  $cb_n = n
  class << CbP
    if $cb_n > 1 then :big end
  end
end
p cb_pick(2), cb_pick(0), cb_pick(3), cb_pick(1)
def cb_arm(n)
  $cb_n = n
  class << CbP
    case $cb_n
    when 1 then :one
    when 2 then 2 + 2
    else "other"
    end
  end
end
p cb_arm(1), cb_arm(2), cb_arm(3)
def cb_either(a, b)
  $cb_a = a
  $cb_b = b
  class << CbP
    $cb_a && $cb_b
  end
end
p cb_either(1, 2), cb_either(nil, 2), cb_either(1, nil)
y = class CbK
  begin
    @@cb = 5
    @@cb + 1
  end
end
p y

# a directive inside the last statement stays a statement of the body
class CbR; end
def cb_dm
  class << CbR
    if true
      define_method(:dm) { 6 }
    end
  end
end
cb_dm
p CbR.dm
def cb_dm2
  class << CbR
    if true
      define_method(:dm2) { 8 }
    end
    :v
  end
end
p cb_dm2, CbR.dm2

# a path with no leaf (a missing else, an empty arm) answers nil, also when
# the condition runs the same body again
$cb_d = 0
def cb_reenter
  class << CbP
    if ($cb_d += 1) == 2 || (cb_reenter; false)
      :inner
    end
  end
end
p cb_reenter
$cb_d = 0
$cb_f = -> {
  class CbRe
    if ($cb_d += 1) == 2 || ($cb_f.call; false)
      :inner
    end
  end
}
p $cb_f.call
$cb_d = 0
$cb_f = -> {
  class CbRc
    case
    when ($cb_d += 1) == 2 || ($cb_f.call; false) then :inner
    end
  end
}
p $cb_f.call
$cb_d = 0
$cb_f = -> {
  module CbRu
    unless ($cb_d += 1) == 1 && ($cb_f.call; true)
      :inner
    end
  end
}
p $cb_f.call
def cb_empty(n)
  $cb_n = n
  class << CbP
    if $cb_n == 1 then elsif $cb_n == 2 then :two else end
  end
end
p cb_empty(1), cb_empty(2), cb_empty(3)
def cb_case_empty(n)
  $cb_n = n
  class << CbP
    case $cb_n when 1 then when 2 then :two end
  end
end
p cb_case_empty(2), cb_case_empty(1), cb_case_empty(3)
def cb_unit
  class << CbP
    ()
  end
end
p cb_unit

# a directive inside the last statement or an if of a body that is a write's
# value
class CbV; end
w = class << CbV
  if true
    attr_accessor :zv
  end
  self
end
p w == CbV.singleton_class
CbV.zv = 1
p CbV.zv
u = class CbW
  if true
    attr_accessor :zw
    5
  end
end
a = CbW.new
a.zw = 4
p u, a.zw
class CbX; def self.qx = 1; end
x = class << CbX
  if true
    private :qx
  end
  self
end
p x == CbX.singleton_class, CbX.respond_to?(:qx)
