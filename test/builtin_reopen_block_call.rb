# A String, Symbol or IO reopen's own method of a builtin name, called with
# a literal block, is the reopen's method (`p STDOUT.tap { }` built C that
# passed the handle where the reopen's Integer was read). An IO reopen
# method with a `&blk` parameter is called with the block as its last
# argument (a literal block ran the builtin or failed the C build, and
# `&pr` was refused).

class IO
  def tap = 42
  def then = :io_then
  def yield_self(&blk) = 45
  def each_line(&blk) = blk.lambda?
  def each_byte(n = 1, &blk) = n
end

class File
  def each_byte(n = 1, &blk) = n + 100
end

class String
  def tap = 43
  def yield_self = :string_yield_self
  def each_char(&blk) = blk.call("x")
end

class Symbol
  def tap = 44
end

p STDOUT.tap { }
p STDOUT.then { 1 }
p "a".tap { }
p "a".yield_self { 1 }
p "ab".each_char { |c| c * 2 }
p :a.tap { }
x = $stderr.tap { }
p x
p STDOUT.yield_self { 1 }
p STDOUT.each_line { }
pr = proc { }
p STDOUT.each_line(&pr)
p STDOUT.each_byte(2) { }
File.open(__FILE__) { |f| p f.each_byte(2) { } }
p STDOUT.each_byte
