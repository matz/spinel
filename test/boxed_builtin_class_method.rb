# A class method of a builtin class called on a boxed value that holds the
# class: the dispatch knows the program's class methods only, so a Time read
# back out of an Array raised NoMethodError for `now` and `at`.
class Stamp < Time
  def stamped?
    true
  end
end

pick = [Time, Time][ARGV.size]
puts pick.now.year > 2000
puts pick.at(86400).utc.day

mixed = [Time, Stamp]
first = mixed[ARGV.size]
puts first.at(0).utc.year
puts first.at(0).class

# a boxed Time instance keeps its own utc
inst = [Time.at(0), 1][ARGV.size]
puts inst.utc.year
