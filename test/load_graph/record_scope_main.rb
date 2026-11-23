# The load record's scopes (--dump-load-record,
# record_scope_main.rb.load-record): a mixin's module found through its
# lexical scope ahead of a top-level module of the same name, and through
# a path; the module a required file reopens; a class's own macro and a
# send recorded as code the record cannot follow; and a body's bare
# visibility words.
module Helper
  def from = "top"
end
module Outer
  module Helper
    def from = "outer"
  end
  class Thing
    include Helper
  end
end
require_relative "record_scope_lib"
class Other
  include Outer::Helper
end
class Macro
  def self.prop(n) = n
  prop :x
  send(:hash)
  private
  def hidden = 1
  public
  def shown = hidden + 1
end
puts Outer::Thing.new.from
puts Other.new.from
puts Outer::Thing.new.extra
puts Macro.new.shown
