# A user method named like a builtin iterator, called with a block on a
# receiver that may be one of several user classes (or a builtin container):
# it must run the user's method, not the builtin walk of the name.
class Base
  def each_pair
    yield self.class.name, 7
    :pair
  end

  def each_key
    yield :key
    :key
  end

  def each_value
    yield 1.5
    :value
  end

  def each_entry
    yield :entry
    :entry
  end

  def reverse_each
    yield :rev
    :rev
  end

  def uniq
    yield :u
    :uniq
  end

  def each_line
    yield 42
    :line
  end
end

class Child < Base; end

class Other
  def each_pair
    yield "other", 8
  end

  def each_line
    yield 43
  end
end

[Child.new, Base.new].each { |o| o.each_pair { |a, k| p [a, k] } }
o = ARGV.empty? ? Child.new : Base.new
r = o.each_pair { |a, k| p [a, k] }
p r
[Child.new, Other.new].each { |x| x.each_pair { |a, k| p [a, k] } }
[Child.new, Other.new].each { |x| x.each_line { |l| p l + 1 } }

xs = [Child.new, { a: 1, "b" => 2 }]
xs.each { |x| p(x.each_pair { |k, v| p [k, v] }) }
xs.each { |x| p(x.each_key { |k| p k }) }
xs.each { |x| p(x.each_value { |v| p v }) }
ys = [Base.new, [3, 1, 2], 1..3]
ys.each { |y| p(y.each_entry { |e| p e }) }
ys.each { |y| p(y.reverse_each { |e| p e }) }
ys.each { |y| p(y.uniq { |e| e.is_a?(Symbol) ? e : e % 2 }) }
[Base.new, [1, 2]].each do |y|
  y.each_pair { |k, v| p [k, v] }
rescue NoMethodError => e
  puts "NoMethodError: #{e.message[0, 26]}"
end

pr = proc { |k, v| p [k, v] }
[Child.new, { z: 9 }].each { |x| x.each_pair(&pr) }
n = 0
[Child.new, Base.new, { q: 1 }].each { |x| x.each_pair { |_k, v| n += v } }
p n
