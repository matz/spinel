# A reopened Array or Hash method that yields runs with the call's block,
# in a statement or a value, as CRuby runs it: the builtin's loop of the
# same name does not answer for it.
class Array
  def each
    puts "mine"
    i = 0
    while i < size
      yield self[i]
      i += 1
    end
    self
  end
  def map
    r = []
    each { |e| r << yield(e) * 10 }
    r
  end
end
a = [3, 1, 5]
y = a.each { |e| p e }
p y
p a.map { |e| e + 1 }
a.each { |e| break if e == 1; p e }
p(a.each { |e| break e * 100 if e == 1 })
h = { a: 1 }
class Hash
  def each_pair
    puts "hmine"
    yield :k, 2
    self
  end
end
h.each_pair { |k, v| p [k, v] }
class Array
  def each_with_index
    puts "mine"
    i = 0
    while i < size
      yield self[i], i
      i += 1
    end
    self
  end
end
arr = [3, 1]
arr.each_with_index { |e, i| p [e, i] }
