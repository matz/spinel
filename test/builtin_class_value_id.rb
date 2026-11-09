# The class of a builtin value carries its builtin class id, as the constant
# naming the class does, so what dispatches on a class value reaches the right
# arm: `new` on the class of a String, an Array, a Hash and a plain Object,
# the superclass and ancestors of an Integer, Float or Symbol, the class
# compared with a constant, kept as a Hash key, stored, and case-tested.
class A; end
class B < A; end

# c01_name_each
xs = [A.new, "s", [1], { a: 1 }, Object.new, 1, 1.5, :s, nil, true, false, (1..2), Time.at(0), proc { }, 2**70]
p xs.map { |x| x.class.name }
p xs.map { |x| x.class.to_s }
p xs.map { |x| x.class.inspect }

# c02_eq_const
xs = [A.new, "s", [1], { a: 1 }, Object.new, 1, 1.5, :s, nil, true]
p xs.map { |x| [x.class == String, x.class == Array, x.class == Hash, x.class == Object, x.class == Integer, x.class == Float, x.class == Symbol, x.class == NilClass, x.class == TrueClass, x.class == A] }

# c03_new_via_class
xs = [A.new, "s", [1], { a: 1 }, Object.new]
xs.each { |x| p x.class.new.class }
xs.each { |x| p x.class.new.inspect[0, 3] }

# c04_superclass
xs = [A.new, B.new, "s", [1], { a: 1 }, Object.new, 1, 1.5, :s, nil, true, (1..2)]
p xs.map { |x| x.class.superclass }

# c05_ancestors_head
xs = [A.new, "s", [1], { a: 1 }, Object.new, 1, 1.5, :s, nil]
p xs.map { |x| x.class.ancestors.first(2) }
p xs.map { |x| x.class.ancestors.include?(Comparable) }

# c07_case_when
xs = [A.new, "s", [1], { a: 1 }, 1, 1.5, :s, nil, true]
xs.each do |x|
  k = x.class
  r = if k == String then :string
      elsif k == Array then :array
      elsif k == Hash then :hash
      elsif k == Integer then :integer
      elsif k == Float then :float
      elsif k == Symbol then :symbol
      elsif k == NilClass then :nil
      elsif k == A then :a
      else :other
      end
  r2 = case k
       when Class then :class
       end
  p [r, r2]
end

# c08_hash_keys
h = { String => 1, Integer => 2, Array => 3, A => 4, Hash => 5, NilClass => 6 }
xs = [A.new, "s", [1], { a: 1 }, 1, nil, 1.5]
p xs.map { |x| h[x.class] }
p xs.map(&:class).tally.map { |k, v| [k.name, v] }
p xs.group_by(&:class).keys.map(&:name)
p xs.map(&:class).uniq.size

# c09_array_include
xs = [A.new, "s", [1], 1, nil]
p xs.map { |x| [String, Integer].include?(x.class) }
p xs.map { |x| x.class <= Object }
p xs.map { |x| x.class < Comparable }
p xs.map { |x| x.class.instance_of?(Class) }

# c10_sort_by_name
xs = ["s", 1, [2], { a: 1 }, 1.5, :s]
p xs.map(&:class).sort_by(&:name).map(&:to_s)
p xs.map(&:class).map(&:hash).uniq.size
p xs.map(&:class).map { |k| k.equal?(k) }

# c11_exception_class
class MyErr < StandardError; end
[MyErr, ArgumentError, RuntimeError].each do |k|
  begin
    raise k, "x"
  rescue => e
    p e.class.name
    p e.class.new("y").message
    p e.class.superclass
    p e.class == k
  end
end

# c12_nil_true_new
xs = [nil, true, false, 1, :s, 1.5]
xs.each do |x|
  begin
    p x.class.new
  rescue NoMethodError => e
    puts e.message[0, 30]
  end
end

# c14_class_value_stored
xs = [A.new, "s", [1], { a: 1 }, 1, nil]
ks = xs.map(&:class)
@ks = ks
h = {}
ks.each_with_index { |k, i| h[k] = i }
GC.start
p h.map { |k, v| [k.name, v] }
p @ks.map(&:name)
p ks.each_slice(2).map { |a| a.map(&:to_s) }

# c15_eqq_and_is_a
xs = [A.new, "s", [1], 1, nil]
p xs.map { |x| String === x }
p xs.map { |x| x.class === x }
p xs.map { |x| x.class.name.nil? }
p xs.map { |x| x.is_a?(x.class) }
p xs.map { |x| x.instance_of?(x.class) }
