# A module prepended to Array that defines min, max, sample, at, fetch, dig,
# delete_at or delete answers each of them: the String its method makes, not
# an element of the Array. The Array's other reads ([], first, last, pop,
# shift) still hand back the element itself.
module OwnReads
  def min = +"pre-min"
  def max = +"pre-max"
  def sample = +"pre-sample"
  def at(i) = +"pre-at#{i}"
  def fetch(i) = +"pre-fetch#{i}"
  def dig(i) = +"pre-dig#{i}"
  def delete_at(i) = +"pre-delete_at#{i}"
  def delete(s) = +"pre-delete-#{s}"
end

class Array
  prepend OwnReads
end

def bang(x) = x << "!"

def in_method(a)
  x = a.sample
  x << "?"
  puts x, a.min + "/" + a.max, a.at(1).size
  bang(a.fetch(2))
  bang(a.dig(0))
  a.delete_at(1) << "#"
  puts a.delete("b").upcase
  puts a.inspect
end

a = [+"b", +"a", +"c"]
x = a.min
x << "!"
puts x
puts a.max, a.sample, a.at(0), a.fetch(1), a.dig(2), a.delete_at(0), a.delete("b")
puts bang(a.max), bang(a.sample), bang(a.at(2)), bang(a.fetch(0)), bang(a.dig(1))
puts bang(a.delete_at(2)), bang(a.delete("c"))
y = a.fetch(1)
y << "?"
puts y
b = []
b << a.at(1) << a.dig(0) << a.min << a.max << a.sample << a.fetch(2) << a.delete_at(1) << a.delete("a")
puts b.inspect
puts a.min.equal?(a[1]), a.max.equal?(a[2]), a.at(0).equal?(a[0]), a.fetch(0).equal?(a[0])
puts a.dig(0).equal?(a[0]), a.delete_at(0).equal?(a[0]), a.delete("b").equal?(a[0])
puts "#{a.min}|#{a.fetch(0)}|#{a.delete("x")}"
puts a.min == "pre-min", a.sample.start_with?("pre"), a.at(3) + a.dig(4)
puts a.inspect
in_method(a)
# the reads Array still answers itself are the elements
a.first << "1"
a[1] << "2"
a.last << "3"
k = a[2]
t = a.pop
puts t.equal?(k)
t << "4"
puts k, a.inspect
puts a.shift.equal?(a[0]), a.inspect
