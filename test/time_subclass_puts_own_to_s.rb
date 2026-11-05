# puts of a Time subclass instance prints what its to_s answers (its own,
# or one up its chain), as print and "#{}" do. A subclass without one
# prints the Time text, and a nil one prints puts nil's empty line.
class Stamp < Time
  def to_s = "STAMP"
end
class Plain < Time; end
class Deep < Stamp; end
puts Stamp.at(0).utc
puts Stamp.at(0).utc, Time.at(60).utc
puts Plain.at(0).utc
puts Deep.at(0).utc
puts Time.at(0).utc
def show(x) = puts(x)
show(Deep.at(5).utc)
show(Time.at(5).utc)
a = Stamp.at(0).utc
puts a.dup
puts a.getlocal("+00:00")
t = nil
t = Stamp.at(0).utc if ARGV.size > 3
puts t
puts "end"
