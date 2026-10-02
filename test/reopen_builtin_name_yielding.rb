# An Array reopen that defines a builtin's name with a method that yields:
# the reopen owns the name, and a yielding method has no function of its
# own, so a call with or without a block goes through its proc form.
class Array
  def sum
    block_given? ? map { |x| yield x }.inject(0) { |a, b| a + b } : 42
  end
end

class Hash
  def count
    block_given? ? keys.map { |k| yield k }.size : -1
  end
end

p [1, 2, 3].sum
p([1, 2, 3].sum { |x| x * 10 })
p ["a", "b"].sum
p({ a: 1, b: 2 }.count)
p({ a: 1, b: 2 }.count { |k| k.to_s })
