# A reader that answers an Integer or nil (or a Float or nil), called on a
# value that may be one of several classes, keeps its nil when an OpenStruct
# may answer the call too. The OpenStruct arm took the member as a plain
# Integer, which a slot holding nil cannot store, and the C did not compile:
# with ostruct required, date's Date.strptime did not build.
require "ostruct"

class Day
  def initialize(year, weight) = (@year = year; @weight = weight)
  def year = @year
  def weight = @weight
end

class Week
  def initialize(year, weight) = (@year = year; @weight = weight)
  def year = @year
  def weight = @weight
end

years = {}
weights = {}
[Day.new(2026, 1.5), Week.new(nil, nil), Day.new(nil, nil), Week.new(1999, 2.25)].each_with_index do |d, i|
  years[i] ||= d.year
  weights[i] ||= d.weight
end
p years, weights
member = OpenStruct.new(year: 1984, weight: 0.5)
p member.year, member.weight
