# A version entry's arm runs the version it is pinned to on every self,
# beside subclasses that override the method by another type: the arm's
# answer, and the arguments it passes, are that version's alone.
class Num
  if ARGV.size == 0
    def foo = 1
  else
    def foo = 2
  end
  def bar = foo + 1
end
class NumF < Num
  def foo = 1.5
end
p Num.new.bar
p NumF.new.bar
p [Num.new, NumF.new].map(&:bar)

class Name
  if ARGV.size == 0
    def name = +"aa"
  else
    def name = +"bb"
  end
  def shout = name.to_s + "!"
end
class NameSym < Name
  def name = :sym
end
p Name.new.shout
p NameSym.new.shout

class Val
  if ARGV.size == 0
    def v = 5
  else
    def v = nil
  end
  def w = v.to_s + "."
  def n = v.nil? ? -1 : v
end
class ValNil < Val
  def v = nil
end
class ValStr < Val
  def v = "c"
end
p Val.new.w, Val.new.n
p ValNil.new.w, ValNil.new.n
p ValStr.new.w, ValStr.new.n

class Cm
  if ARGV.size == 0
    def self.foo = 1
  else
    def self.foo = 2
  end
  def self.bar = foo + 10
end
class CmF < Cm
  def self.foo = 0.25
end
p Cm.bar
p CmF.bar

class Yl
  if ARGV.size == 0
    def each_val = yield(1)
  else
    def each_val = yield(2)
  end
  def total = each_val { |x| x + 1 }
end
class YlF < Yl
  def each_val = yield(0.5)
end
p Yl.new.total
p YlF.new.total

class Take
  if ARGV.size == 0
    def take(x) = x
  else
    def take(x) = [x]
  end
  def go(v) = take(v)
end
class TakeS < Take
  def take(x) = x.to_s * 2
end
p Take.new.go(3)
p TakeS.new.go(4)
p Take.new.go("s")
p TakeS.new.go(:k)
