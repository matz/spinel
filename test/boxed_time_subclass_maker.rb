# Time's maker on a boxed value that holds a Time subclass makes an instance
# of the subclass, as `Stamp.now` written out does: the boxed call's arms
# were Time's alone, so the subclass reached the dispatch and raised
# NoMethodError for `now`.
class Stamp < Time
end

class Mark < Time
  def self.now
    Time.at(0)
  end
end

ks = [Time, Stamp, Mark]
k = ks[ARGV.size + 1]
puts k.now.class
puts k.at(86400).utc.day
puts k.at(0).class

# a subclass with its own maker keeps it
m = ks[ARGV.size + 2]
puts m.now.to_i
puts m.now.class

t = ks[ARGV.size]
puts t.now.class
