# An iterator that answers its receiver, in tail position, whose block
# `return`s something else: the method answers the block's value or the
# receiver. The receiver arm was lost and the method answered nil.
def t1(n) = n.times { |i| return "hit#{i}" if i == 2 }
p t1(5), t1(1)

def t2(n)
  n.times { |i| return "hit#{i}" if i == 2 }
end
p t2(5), t2(1)

def t3(n) = 1.upto(n) { |i| return "hit#{i}" if i == 2 }
p t3(5), t3(1)

def t4(n) = 7.downto(n) { |i| return "hit#{i}" if i == 5 }
p t4(1), t4(6)

def t5(n) = 1.step(n, 1) { |i| return "hit#{i}" if i == 2 }
p t5(5), t5(1)

def t6(n) = 1.0.step(n, 0.5) { |f| return "hit#{f}" if f == 2.0 }
p t6(3), t6(1)

def t7(n) = (0..n).step(2) { |i| return "hit#{i}" if i == 4 }
p t7(5), t7(1)

def t8(a) = a.each { |x| return "hit#{x}" if x == 2 }
p t8([1, 2, 3]), t8([1])

def t9(h) = h.each { |k, v| return "hit#{k}" if v == 2 }
p t9({ a: 1, b: 2 }), t9({ a: 1 })

def t10(a) = a.each_with_index { |x, i| return "hit#{x}" if i == 2 }
p t10([5, 6, 7]), t10([5])

def t11(n)
  r = (0...n)
  r.each { |i| return "hit#{i}" if i == 2 }
end
p t11(5), t11(1)

def t12(n) = n.times { |i| return 1.5 if i == 2 }
p t12(5), t12(1)

class C
  def initialize(n) = @n = n
  def iv = @n.times { |i| return "hit#{i}" if i == 2 }
end
p C.new(5).iv, C.new(1).iv

L = ->(n) { n.times { |i| return "hit#{i}" if i == 2 } }
p L.(5), L.(1)

# a receiver that is not a plain read answers itself too, block return or not
def u1(n) = 3.times { |i| }
def u2(n) = (n + 1).times { |i| }
def u3(n) = 1.upto(n) { |i| }
def u4(n) = 7.downto(n) { |i| }
def u5(n) = 1.step(n, 2) { |i| }
p u1(3), u2(3), u3(3), u4(3), u5(3)

# ... and one that acts runs once
$calls = 0
def nx = ($calls += 1; 2)
x = nx.times { |i| }
p x, $calls
y = nx.upto(3) { |i| }
p y, $calls

# under begin/rescue the slot is the begin's result, not the method's return
def b1(n)
  begin
    n.times { |i| return "hit#{i}" if i == 2 }
  rescue
    0
  end
end
p b1(5), b1(1)

def b2(n)
  begin
    n.times { |i| }
  rescue
    0
  end
end
p b2(5)

def b3(n)
  v = begin
    n.times { |i| }
  ensure
    nil
  end
  v + 1
end
p b3(2)
