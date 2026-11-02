# What module_function copies to the module's singleton (--dump-load-record,
# record_mf_copies_main.rb.load-record): a def, a define_method and
# `module_function :n` of a name, as CRuby copies them. An alias, an attr
# reader and an undef under the bare word get no copy.
module W
  module_function
  def a = 1
  alias b a
  alias_method :c, :a
  attr_reader :q
  define_method(:dm) { 7 }
  def z = 2
  undef_method :a
end
module X
  def m = 1
  alias n m
  module_function :n
  def n = 2
end
