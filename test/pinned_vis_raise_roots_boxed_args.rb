# The NoMethodError a call raises for a root's private method, called with an
# explicit receiver, stages the arguments the call was given for #args. Each
# boxed argument that holds heap (a Range's box is a fresh copy) is rooted
# while the next is built, so a collection between them frees none.

def off(a, b, c) = [:main, a, b, c]
if ARGV.size > 0
  class Object
    def off(a, b, c) = [:obj, a, b, c]
  end
end

r1, r2, r3 = 1..2, 3..4, 5..6
bad = 0
2000.times do
  nil.off(r1, r2, r3)
rescue NoMethodError => e
  bad += 1 unless e.args == [1..2, 3..4, 5..6]
end
p bad

def pr(a, b) = [:main, a, b]
if ARGV.size > 0
  class Object
    def pr(a, b) = [:obj, a, b]
  end
end
s = "tail"
bad = 0
2000.times do |i|
  nil.pr(1..i, "x#{i}")
rescue NoMethodError => e
  bad += 1 unless e.args == [1..i, "x#{i}"] && e.name == :pr
end
p bad
begin
  3.pr(s, 7)
rescue NoMethodError => e
  p e.args
  puts e.message
end
