# spinel: int64
# spinel: gc-stress

def nil_first(n)
  if n == 0
    value = nil
  elsif n == 1
    value = 0
  elsif n == 2
    value = 7
  else
    value = 18446744073709551616
  end
  value
end

def bigint_first(n)
  if n == 3
    value = 18446744073709551616
  elsif n == 0
    value = nil
  elsif n == 1
    value = 0
  else
    value = 7
  end
  value
end

class BigintHolder
  def initialize
    @value = nil
  end

  def store(n)
    @value = nil_first(n)
  end

  def check
    p !@value, !!@value, (not @value), (@value ? :truthy : :falsy)
  end

  def value
    @value
  end
end

$calls = 0
def counted_value(n)
  $calls += 1
  nil_first(n)
end

holder = BigintHolder.new
4.times do |n|
  value = nil_first(n)
  p !value, !!value, (not value), (value ? :truthy : :falsy)
  copy = value
  GC.start
  p !copy, !!copy, (not copy), (copy ? :truthy : :falsy)
  p !nil_first(n), !!nil_first(n), (not nil_first(n)), (nil_first(n) ? :truthy : :falsy)

  other = bigint_first(n)
  p !other, !!other, (not other), (other ? :truthy : :falsy)
  other_copy = other
  p !other_copy, !!other_copy, (not other_copy), (other_copy ? :truthy : :falsy)
  p !bigint_first(n), !!bigint_first(n), (not bigint_first(n)), (bigint_first(n) ? :truthy : :falsy)

  holder.store(n)
  GC.start
  holder.check
  p !holder.value, !!holder.value, (not holder.value), (holder.value ? :truthy : :falsy)

  p !counted_value(n), $calls
  p !!counted_value(n), $calls
  p (not counted_value(n)), $calls
  p (counted_value(n) ? :truthy : :falsy), $calls
end
