# The definitions of a class body in a value (--dump-load-record, record_unplaced_defs_main.rb.load-record): an
# include, an attr, a def, a visibility change, an alias and an undef in a class body written as an argument run
# where its statement does, so they are placed at it (end=yes for the reader, the alias and the def). A class body
# that may not run with its statement (behind `&&`) is its class's body all the same, its definitions recorded
# after the placed bodies, `unplaced`, so no query at a point sees them (end=no), as the mixin is in the class's line.
module M; def hi = "hi"; end
p(class G
  include M
  attr_reader :z
  def y = 1
  private :y
  alias_method :w, :y
  undef_method :hi
  self
end)
p(false && (class H
  include M
  attr_reader :z
  def y = 1
  private :y
  alias_method :w, :y
  undef_method :hi
  self
end))
