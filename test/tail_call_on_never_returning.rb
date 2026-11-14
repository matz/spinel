# A method's tail call on the value of a call that never returns: a method
# that only raises, or one whose only value is a call on a raise's value.
# Its other return typed the slot an Array; the tail answered the no-method
# token as it stood, and the C did not compile.
def raises_only
  raise "no"
end

def raises_first
  (raise "no").select { |d| d }
end

def from_raising(x)
  return [] if x
  raises_only.map { |d| "#{d}" }
end

def from_raising_chain(x)
  return [] if x
  raises_first.map { |d| "#{d}/y" }
end

def joined(x) = (["a"] + from_raising_chain(x)).uniq

p from_raising(true)
p from_raising_chain(true)
p joined(true)
begin
  from_raising(false)
rescue => e
  puts e.message
end
begin
  joined(false)
rescue => e
  puts e.message
end

# A subclass override answers in its place: the self call returns there.
class Feed
  def items = (raise "abstract").select { |d| d }
  def titles(x)
    return [] if x
    items.map { |d| "#{d}!" }
  end
end
class NewsFeed < Feed
  def items = ["a", "b"]
end
p NewsFeed.new.titles(false)

# A return in the raise's argument leaves before the raise runs.
def source(x)
  (raise([1].each { return x })).select { |d| d }
end
p source(nil).nil?
