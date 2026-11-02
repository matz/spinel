# A NilClass method that yields, called on nil, runs with the call's block.
class NilClass
  def each_once
    yield 1
    nil
  end

  def with_self
    yield self
  end

  def maybe(x)
    block_given? ? yield(x * 2) : x
  end

  def each_arg(&b)
    b.call(:arg)
    :done
  end
end

nil.each_once { |v| p v }
p nil.with_self { |s| s.inspect }
p nil.maybe(3) { |y| y + 1 }
p nil.maybe(3)
p nil.each_arg { |a| p a }

x = nil
x.each_once { |v| p v + 10 }
p x.with_self { |s| s.nil? }
p x.each_arg { |a| p a.to_s }


def nothing
  puts "ran"
end
nothing.each_once { |v| p v + 20 }
@unset.each_once { |v| p v + 30 }
