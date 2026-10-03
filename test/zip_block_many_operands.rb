# Array#zip with a block and other than one operand (two, three, a splat,
# none; a Range receiver too) yields each tuple and answers nil.
r = []; [1, 2, 3].zip([4, 5, 6], [7, 8, 9]) { |c| r << c }
p r
r2 = []; [1, 2].zip([3, 4]) { |a, b| r2 << a + b }
p r2
r3 = []; p([1, 2].zip([3], ["a", "b"]) { |x| r3 << x })
p r3
r4 = []; [1, 2].zip([3, 4], [5, 6]) { |a, b, c| r4 << a + b + c }
p r4
r5 = []; [1, 2].zip { |x| r5 << x }
p r5
xs = [[10, 20], [30, 40]]
r6 = []; [1, 2].zip(*xs) { |t| r6 << t }
p r6
r7 = []; (1..2).zip([5, 6], [7, 8]) { |t| r7 << t.sum }
p r7

# Each receiver element is read when its tuple is yielded; array operands
# are snapshotted. The result carries a valued break, or nil on completion.
a = [1, 2]
b = [3, 4]
a.zip(b, []) do |tuple|
  p tuple
  a[1] = 20
  b[1] = 40
end
p([1, 2].zip([], []) { |tuple| break :done })
p([1, 2].zip { |tuple| break :zero })
p([1, 2].zip(*xs) { |tuple| break :splat })
p([1, 2].zip([], []) { |tuple| next :ignored })
p([1, 2].zip((10..1_000_000_000), []) { |tuple| break tuple })
p([1, 2, 3].zip((10...11), []) { |tuple| p tuple })

[1].zip([], []) { |*tuple| p tuple }
[1].zip([], []) { puts :no_params }
[[1, 2]].zip([3], [4]) { |(a,b),c,d| p [a,b,c,d] }
def stop_zip
  [1].zip([], []) { |x| return :returned }
  :wrong
end
p stop_zip

# Receiver length is checked again after each yield.
a = [1]
a.zip([], []) { |row| p row; a << 2 if a.length == 1 }
a = [1, 2]
a.zip([], []) { |row| p row; a.clear }

# Optional, rest and post parameters use Ruby block distribution.
[1].zip([], []) { |a, *rest| p [a, rest] }
[1].zip { |a, b=9| p [a, b] }
[1].zip([2], [3]) { |a, *rest, z| p [a, rest, z] }
[1].zip([2], [3]) { |a, b=9, *rest, z| p [a, b, rest, z] }
[1].zip { |a, *rest, z| p [a, rest, z] }
[1].zip([2], [3]) { |a,| p a }
[1].zip([2], [3]) { |a, *| p a }
[1].zip(*[[2], [3]]) { |a, *rest, z| p [a, rest, z] }
[1].zip { |a=9| p a }
[1].zip([2], [3]) { |*rest, z| p [rest, z] }

# Unused parameters retain declarations, and shadowed locals stay distinct.
[1].zip([], []) { |unused| puts :ok }
[1].zip([], []) { |unused, used, other| p used }
row = :outer
[1].zip([], []) { |row| p row }
p row
def unused_zip
  [1].zip([], []) { |unused| puts :ok }
  [1].zip([], []) { |unused, used, other| p used }
end
unused_zip
def shadow_zip
  row = [9]
  [1].zip([], []) { |row| p row }
  p row
end
shadow_zip

# Enumerator preparation stops at the receiver's initial length.
e = Enumerator.new do |y|
  y << 10
  raise "overread"
end
[1].zip(e, []) { |row| p row }
e = Enumerator.new do |y|
  i = 20
  loop do
    y << i
    i += 1
  end
end
[1, 2].zip(*[e, []]) { |row| p row }
[].zip(e, []) { |row| p row }
a = [1]
a.zip(e, []) { |row| p row; a << 2 if a.length == 1 }
[1, 2].zip([9].each, []) { |row| p row }

# Boxed nil must raise in every block operand shape.
a = [nil, [1], :other][0]
begin
  a.zip { |row| p row }
rescue NoMethodError
  puts :nil_zero
end
begin
  a.zip([]) { |row| p row }
rescue NoMethodError
  puts :nil_one
end
begin
  a.zip([], []) { |row| p row }
rescue NoMethodError
  puts :nil_many
end
begin
  a.zip(*[[], []]) { |row| p row }
rescue NoMethodError
  puts :nil_splat
end
a = [[1], nil, :other][0]
a.zip([], []) { |row| p row }

# Growing the receiver does not extend the prepared Array or Range operands.
a = [1]
a.zip([10, 20], (30..40)) { |row| p row; a << 2 if a.length == 1 }
a = [1]
a.zip(*[[10, 20], (30..40)]) { |row| p row; a << 2 if a.length == 1 }
