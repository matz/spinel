# A reopened Time's `self - o` with o holding either a Time or a number:
# the call is boxed, and a local taking a call of that method holds the
# boxed answer. Read while o's callers were still being typed, the `-` was
# answered as a Time, which pinned the local to a Time slot.
class Time
  def sub(o) = self - o

  def sub_local(o)
    r = self - o
    r
  end
end
t = Time.at(100).utc
[Time.at(40), 30].each do |x|
  r = t.sub(x)
  p r.is_a?(Time) ? r.to_i : r
end
x = [Time.at(60), 25][ARGV.size]
r = t.sub_local(x)
p r
y = [Time.at(60), 25][ARGV.size + 1]
r2 = t.sub_local(y)
p r2.to_i
