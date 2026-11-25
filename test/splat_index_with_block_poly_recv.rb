# An index call whose arguments are a splat of unknown length, with a block,
# on a receiver known only at run time: `[]` takes as many arguments with a
# block as without, so the splat is read as its one index, as a call with
# no block reads it. Through __send__ the String arm's C passed the whole
# array as the index and did not build.
class Xml
  def entry(*a, &b) = a
end
class W
  def initialize(x) = @x = x
  def go(m, *args)
    @x.__send__(m, *args) { 1 }
  end
  def get(*args)
    @x.[](*args) { 1 }
  end
end
w = W.new(ARGV.empty? ? Xml.new : "str")
p w.go(:entry, 1)
p W.new("abc").go(:[], 1)
p W.new(ARGV.empty? ? [5, 6] : "str").get(1)
p W.new("abc").get(2)
