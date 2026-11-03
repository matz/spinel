# A module included into Enumerable sits behind Enumerable's own native
# methods, so a Hash's uniq and a Range's sum stay the builtin ones. A
# Struct's own select answers ahead of a program's Enumerable#select.
module Extra
  def uniq(*a) = :extra_uniq
  def sum(*a) = :extra_sum
  def zip(*a) = :extra_zip
end
module Enumerable
  include Extra
  def select(*a) = :enum_select
  def filter_map(*a) = :enum_filter_map
end
p({a: 1}.uniq)
p (1..3).sum
p({a: 1}.zip([1]))
p [1, 1].uniq
S = Struct.new(:a, :b)
p S.new(1, 2).select { |x| x > 1 }
p (1..3).select { |x| x > 1 }
p S.new(1, 2).filter_map { |x| x }
