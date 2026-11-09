# An alias of a writer another file defines where a begin/rescue may reach it
# (the record has no version that surely ran) names that writer, over a reader
# of the alias's name an included module brings: the alias is bound where it
# stands among the includes, not after them.
module Pragmas
  def bt = :prag_reader
  def bt=(x); @v = [:prag, x]; end
  def v = @v
end
begin
  require_relative "alias_of_maybe_writer_native"
rescue LoadError
  require_relative "alias_of_maybe_writer_fallback"
end
class Database
  include Pragmas
  alias_method :bt, :bt=
end
d = Database.new
d.bt = 10
p d.v
d.bt(11)
p d.v
